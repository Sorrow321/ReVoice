#include "precompiled.h"
#include "revoice_upload.h"

#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <utility>

static volatile bool g_uploadInProgress = false;
static pthread_mutex_t g_uploadLogMutex = PTHREAD_MUTEX_INITIALIZER;

static void UploadLog(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	pthread_mutex_lock(&g_uploadLogMutex);
	fputs(buf, stdout);
	fflush(stdout);
	pthread_mutex_unlock(&g_uploadLogMutex);
}

struct ParsedUrl {
	char host[256];
	char port[8];
	char path[256];
};

static bool ParseHttpUrl(const char *url, ParsedUrl &out)
{
	if (strncmp(url, "http://", 7) != 0) return false;
	const char *hostStart = url + 7;
	const char *slash = strchr(hostStart, '/');
	const char *colon = strchr(hostStart, ':');

	if (colon && (!slash || colon < slash)) {
		size_t hlen = colon - hostStart;
		if (hlen == 0 || hlen >= sizeof(out.host)) return false;
		memcpy(out.host, hostStart, hlen);
		out.host[hlen] = '\0';

		const char *portStart = colon + 1;
		const char *portEnd = slash ? slash : portStart + strlen(portStart);
		size_t plen = portEnd - portStart;
		if (plen == 0 || plen >= sizeof(out.port)) return false;
		memcpy(out.port, portStart, plen);
		out.port[plen] = '\0';
	} else {
		const char *hostEnd = slash ? slash : hostStart + strlen(hostStart);
		size_t hlen = hostEnd - hostStart;
		if (hlen == 0 || hlen >= sizeof(out.host)) return false;
		memcpy(out.host, hostStart, hlen);
		out.host[hlen] = '\0';
		strcpy(out.port, "80");
	}

	if (slash) {
		strncpy(out.path, slash, sizeof(out.path) - 1);
		out.path[sizeof(out.path) - 1] = '\0';
	} else {
		strcpy(out.path, "/");
	}
	return true;
}

static bool SendAll(int fd, const char *data, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
		if (n <= 0) return false;
		sent += n;
	}
	return true;
}

static int HttpPost(const ParsedUrl &url, const char *relPath, const char *body, long bodyLen)
{
	struct addrinfo hints = {}, *res = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(url.host, url.port, &hints, &res) != 0 || !res)
		return -1;

	int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sockfd < 0) { freeaddrinfo(res); return -1; }

	struct timeval tv;
	tv.tv_sec = 300;
	tv.tv_usec = 0;
	setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
		freeaddrinfo(res);
		close(sockfd);
		return -1;
	}
	freeaddrinfo(res);

	char header[1024];
	int hdrLen = snprintf(header, sizeof(header),
		"POST %s HTTP/1.1\r\n"
		"Host: %s:%s\r\n"
		"Content-Type: application/octet-stream\r\n"
		"Content-Length: %ld\r\n"
		"X-Filepath: %s\r\n"
		"Connection: close\r\n"
		"\r\n",
		url.path, url.host, url.port, bodyLen, relPath);

	if (!SendAll(sockfd, header, hdrLen) || !SendAll(sockfd, body, bodyLen)) {
		close(sockfd);
		return -1;
	}

	char resp[256];
	ssize_t n = recv(sockfd, resp, sizeof(resp) - 1, 0);
	close(sockfd);

	if (n <= 0) return -1;
	resp[n] = '\0';

	int httpCode = 0;
	if (sscanf(resp, "HTTP/%*d.%*d %d", &httpCode) != 1)
		return -1;

	return httpCode;
}

bool Revoice_Upload_Init()
{
	SERVER_PRINT("[ReVoice] rv_upload_dump available (raw socket HTTP)\n");
	return true;
}

static void ScanWavFiles(const char *dir, const char *baseDir, std::vector<std::pair<std::string, std::string>> &out)
{
	DIR *d = opendir(dir);
	if (!d) return;

	size_t baseDirLen = strlen(baseDir);
	struct dirent *ent;
	while ((ent = readdir(d)) != nullptr) {
		if (ent->d_name[0] == '.') continue;

		char fullPath[512];
		snprintf(fullPath, sizeof(fullPath), "%s/%s", dir, ent->d_name);

		struct stat st;
		if (stat(fullPath, &st) != 0) continue;

		if (S_ISDIR(st.st_mode)) {
			ScanWavFiles(fullPath, baseDir, out);
		} else if (S_ISREG(st.st_mode)) {
			const char *ext = strrchr(ent->d_name, '.');
			if (ext && strcasecmp(ext, ".wav") == 0) {
				const char *rel = fullPath + baseDirLen;
				if (*rel == '/') rel++;
				out.push_back({std::string(fullPath), std::string(rel)});
			}
		}
	}
	closedir(d);
}

struct UploadJob {
	ParsedUrl url;
	std::vector<std::pair<std::string, std::string>> files;
};

static bool UploadOneFile(const ParsedUrl &url, const char *fullPath, const char *relPath)
{
	FILE *f = fopen(fullPath, "rb");
	if (!f) return false;

	fseek(f, 0, SEEK_END);
	long fileSize = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (fileSize <= 0) { fclose(f); return false; }

	char *buf = (char *)malloc(fileSize);
	if (!buf) { fclose(f); return false; }

	size_t rd = fread(buf, 1, fileSize, f);
	fclose(f);
	if ((long)rd != fileSize) { free(buf); return false; }

	int code = HttpPost(url, relPath, buf, fileSize);
	free(buf);

	if (code != 200) {
		if (code < 0)
			UploadLog("[ReVoice Upload] Connection failed: %s\n", relPath);
		else
			UploadLog("[ReVoice Upload] HTTP %d: %s\n", code, relPath);
		return false;
	}
	return true;
}

static void *UploadThreadFunc(void *arg)
{
	UploadJob *job = (UploadJob *)arg;
	int total = (int)job->files.size();
	int success = 0;

	for (int i = 0; i < total; i++) {
		const std::string &fullPath = job->files[i].first;
		const std::string &relPath  = job->files[i].second;

		if (UploadOneFile(job->url, fullPath.c_str(), relPath.c_str())) {
			success++;
			UploadLog("[ReVoice Upload] [%d/%d] OK: %s\n", i + 1, total, relPath.c_str());
		} else {
			UploadLog("[ReVoice Upload] [%d/%d] FAIL: %s\n", i + 1, total, relPath.c_str());
		}
	}

	UploadLog("[ReVoice Upload] Done: %d/%d succeeded\n", success, total);

	delete job;
	g_uploadInProgress = false;
	return nullptr;
}

void Cmd_UploadDump()
{
	if (g_uploadInProgress) {
		SERVER_PRINT("[ReVoice] Upload already in progress.\n");
		return;
	}

	if (!g_pcv_rev_upload_url || !g_pcv_rev_upload_url->string || g_pcv_rev_upload_url->string[0] == '\0') {
		SERVER_PRINT("[ReVoice] REV_UploadURL not set. Example: REV_UploadURL \"http://yourserver:5000/upload\"\n");
		return;
	}

	ParsedUrl url;
	if (!ParseHttpUrl(g_pcv_rev_upload_url->string, url)) {
		SERVER_PRINT("[ReVoice] Invalid REV_UploadURL. Must be http://host:port/path\n");
		return;
	}

	UploadJob *job = new UploadJob();
	job->url = url;
	ScanWavFiles("cstrike/data", "cstrike/data", job->files);

	if (job->files.empty()) {
		SERVER_PRINT("[ReVoice] No WAV files found in cstrike/data/\n");
		delete job;
		return;
	}

	char msg[256];
	snprintf(msg, sizeof(msg), "[ReVoice] Starting async upload of %d files to %s:%s%s\n",
		(int)job->files.size(), url.host, url.port, url.path);
	SERVER_PRINT(msg);

	g_uploadInProgress = true;

	pthread_t tid;
	if (pthread_create(&tid, nullptr, UploadThreadFunc, job) != 0) {
		SERVER_PRINT("[ReVoice] Failed to create upload thread\n");
		delete job;
		g_uploadInProgress = false;
		return;
	}
	pthread_detach(tid);
}
