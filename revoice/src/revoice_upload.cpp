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
#include <set>
#include <deque>

static volatile bool g_uploadInProgress = false;
static pthread_mutex_t g_uploadLogMutex = PTHREAD_MUTEX_INITIALIZER;
static std::deque<std::string> g_uploadLogQueue;

// Thread-safe logger for the upload worker. UTIL_LogPrintf cannot be called
// from the worker thread (it has a static char[1024] buffer and ALERT(at_logged)
// touches engine state without locking — at best you get garbled lines, at worst
// a crash). So we enqueue formatted messages here under a mutex; the main thread
// drains the queue every server frame via Revoice_Upload_DrainLog() and calls
// UTIL_LogPrintf safely from there. A 10000-message cap prevents unbounded
// memory growth if the main thread is wedged for any reason.
static void UploadLog(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	pthread_mutex_lock(&g_uploadLogMutex);
	g_uploadLogQueue.emplace_back(buf);
	if (g_uploadLogQueue.size() > 10000)
		g_uploadLogQueue.pop_front();
	pthread_mutex_unlock(&g_uploadLogMutex);
}

// Called from the main thread (StartFrame_PreHook). Pulls all queued messages
// from the worker and writes them to logs/L*.log via UTIL_LogPrintf.
void Revoice_Upload_DrainLog()
{
	for (;;) {
		std::string msg;
		pthread_mutex_lock(&g_uploadLogMutex);
		if (g_uploadLogQueue.empty()) {
			pthread_mutex_unlock(&g_uploadLogMutex);
			return;
		}
		msg.swap(g_uploadLogQueue.front());
		g_uploadLogQueue.pop_front();
		pthread_mutex_unlock(&g_uploadLogMutex);

		UTIL_LogPrintf("%s", msg.c_str());
	}
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

// Returns HTTP status code on success, -1 on connection / protocol error.
//   path:        URI path (e.g. "/upload", "/verify")
//   xFilepath:   value for the X-Filepath header, or nullptr to omit it
//   contentType: e.g. "application/octet-stream" or "text/plain"
//   body / bodyLen: request body
//   respBody:    optional output — populated with the response body (after
//                the blank-line separating headers from body). nullptr to ignore.
//
// Reads the response by recv-looping until EOF (the server sets Connection: close)
// so we never miss the status line or body when TCP splits them across segments.
static int HttpPost(const ParsedUrl &url, const char *path, const char *xFilepath,
	const char *contentType, const char *body, long bodyLen, std::string *respBody)
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

	char header[2048];
	int hdrLen;
	if (xFilepath) {
		hdrLen = snprintf(header, sizeof(header),
			"POST %s HTTP/1.1\r\n"
			"Host: %s:%s\r\n"
			"Content-Type: %s\r\n"
			"Content-Length: %ld\r\n"
			"X-Filepath: %s\r\n"
			"Connection: close\r\n"
			"\r\n",
			path, url.host, url.port, contentType, bodyLen, xFilepath);
	} else {
		hdrLen = snprintf(header, sizeof(header),
			"POST %s HTTP/1.1\r\n"
			"Host: %s:%s\r\n"
			"Content-Type: %s\r\n"
			"Content-Length: %ld\r\n"
			"Connection: close\r\n"
			"\r\n",
			path, url.host, url.port, contentType, bodyLen);
	}

	// snprintf returns the would-be length on truncation; sending hdrLen bytes from a
	// truncated buffer would read past the end of `header` and leak adjacent stack memory
	// onto the wire (and likely crash). Bail out instead.
	if (hdrLen < 0 || hdrLen >= (int)sizeof(header)) {
		close(sockfd);
		return -1;
	}

	if (!SendAll(sockfd, header, hdrLen) || (bodyLen > 0 && !SendAll(sockfd, body, bodyLen))) {
		close(sockfd);
		return -1;
	}

	// Read full response (server sends Connection: close, so EOF marks end).
	std::string resp;
	char chunk[4096];
	while (resp.size() < 16 * 1024 * 1024) { // 16 MB cap as a safety net
		ssize_t n = recv(sockfd, chunk, sizeof(chunk), 0);
		if (n <= 0) break;
		resp.append(chunk, (size_t)n);
	}
	close(sockfd);

	if (resp.empty()) return -1;

	int httpCode = 0;
	if (sscanf(resp.c_str(), "HTTP/%*d.%*d %d", &httpCode) != 1)
		return -1;

	if (respBody) {
		size_t bodyOff = resp.find("\r\n\r\n");
		if (bodyOff != std::string::npos)
			*respBody = resp.substr(bodyOff + 4);
		else
			respBody->clear();
	}

	return httpCode;
}

// Crash recovery: any "M_*.wav" left on disk from a previous server run is an
// orphan — the writer was killed before CloseWavIfOpen could strip the prefix.
// At plugin load there is no live writer yet, so it is safe to rename them all.
// Without this, orphans would never be picked up by ScanWavFiles.
static void RecoverOrphanedRecordings(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d) return;

	struct dirent *ent;
	while ((ent = readdir(d)) != nullptr) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;

		char fullPath[512];
		snprintf(fullPath, sizeof(fullPath), "%s/%s", dir, ent->d_name);

		struct stat st;
		if (stat(fullPath, &st) != 0) continue;

		if (S_ISDIR(st.st_mode)) {
			RecoverOrphanedRecordings(fullPath);
		} else if (S_ISREG(st.st_mode)
			&& ent->d_name[0] == 'M' && ent->d_name[1] == '_')
		{
			char finalPath[512];
			snprintf(finalPath, sizeof(finalPath), "%s/%s", dir, ent->d_name + 2);
			if (rename(fullPath, finalPath) == 0) {
				UploadLog("[ReVoice] Recovered orphan recording: %s\n", finalPath);
			}
		}
	}
	closedir(d);
}

bool Revoice_Upload_Init()
{
	SERVER_PRINT("[ReVoice] rv_upload_dump available (raw socket HTTP)\n");
	RecoverOrphanedRecordings("cstrike/data");
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
			// Skip in-progress recordings: CRevoicePlayer::AppendWav opens new files
			// with an "M_" prefix and CRevoicePlayer::CloseWavIfOpen renames it away
			// once the WAV is finalized. Reading an M_-prefixed file would race with
			// the writer (header rewrite + tail append).
			if (ent->d_name[0] == 'M' && ent->d_name[1] == '_')
				continue;
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

	int code = HttpPost(url, url.path, relPath, "application/octet-stream", buf, fileSize, nullptr);
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

	// NOTE: this build runs with -fno-exceptions, so std::bad_alloc and friends
	// would terminate the process rather than unwind the stack here. The cleanup
	// at the bottom (delete job; g_uploadInProgress = false) therefore only needs
	// to run on the normal-flow paths, and every early return below performs that
	// cleanup explicitly.

	UploadLog("[ReVoice Upload] Starting dump of %d files to http://%s:%s%s\n",
		total, job->url.host, job->url.port, job->url.path);

	// Phase 0: receiver health check. Empty POST /verify is treated as a ping by
	// the receiver. If this fails, the receiver is down/hung/unreachable; abort
	// the entire job — no uploads attempted, no files touched, no deletions.
	{
		std::string ping;
		int code = HttpPost(job->url, "/verify", nullptr, "text/plain", "", 0, &ping);
		if (code != 200) {
			UploadLog("[ReVoice Upload] ABORT: receiver health check failed (code=%d). "
				"No files uploaded, no files deleted.\n", code);
			delete job;
			g_uploadInProgress = false;
			return nullptr;
		}
		UploadLog("[ReVoice Upload] Receiver healthy, beginning upload\n");
	}

	std::vector<bool> uploaded(total, false);
	std::vector<bool> confirmed(total, false);
	int uploadedCount = 0;

	// Phase 1: upload each file. A file that fails here is left on disk for the
	// next rv_upload_dump to retry — we never delete a file that wasn't confirmed.
	for (int i = 0; i < total; i++) {
		const std::string &fullPath = job->files[i].first;
		const std::string &relPath  = job->files[i].second;

		if (UploadOneFile(job->url, fullPath.c_str(), relPath.c_str())) {
			uploaded[i] = true;
			uploadedCount++;
			UploadLog("[ReVoice Upload] [%d/%d] OK: %s\n", i + 1, total, relPath.c_str());
		} else {
			UploadLog("[ReVoice Upload] [%d/%d] FAIL: %s\n", i + 1, total, relPath.c_str());
		}
	}

	// Phase 2: ask the receiver to confirm which uploads actually landed on disk.
	// Body is one relpath per line; response is "OK <relpath>" / "MISSING <relpath>"
	// per line. A file is only deleted locally if the receiver explicitly confirms it.
	int confirmedCount = 0;
	if (uploadedCount > 0) {
		std::string verifyBody;
		verifyBody.reserve((size_t)uploadedCount * 80);
		for (int i = 0; i < total; i++) {
			if (uploaded[i]) {
				verifyBody += job->files[i].second;
				verifyBody += '\n';
			}
		}

		std::string verifyResp;
		int code = HttpPost(job->url, "/verify", nullptr, "text/plain",
			verifyBody.c_str(), (long)verifyBody.size(), &verifyResp);

		if (code == 200) {
			std::set<std::string> confirmedSet;
			size_t pos = 0;
			while (pos < verifyResp.size()) {
				size_t eol = verifyResp.find('\n', pos);
				size_t len = (eol == std::string::npos ? verifyResp.size() : eol) - pos;
				if (len >= 3 && verifyResp.compare(pos, 3, "OK ") == 0) {
					std::string p = verifyResp.substr(pos + 3, len - 3);
					// Trim possible trailing '\r' if the receiver uses CRLF.
					if (!p.empty() && p[p.size() - 1] == '\r') p.resize(p.size() - 1);
					confirmedSet.insert(p);
				}
				if (eol == std::string::npos) break;
				pos = eol + 1;
			}
			for (int i = 0; i < total; i++) {
				if (uploaded[i] && confirmedSet.count(job->files[i].second)) {
					confirmed[i] = true;
					confirmedCount++;
				}
			}
		} else {
			UploadLog("[ReVoice Upload] /verify request failed (code=%d) — no files will be deleted\n", code);
		}
	}

	// Phase 3: delete confirmed files locally.
	// IMPORTANT: deletion is currently disabled for testing. Once the verify path
	// has been validated end-to-end, uncomment the unlink() below.
	for (int i = 0; i < total; i++) {
		if (!confirmed[i]) continue;
		const std::string &fullPath = job->files[i].first;
		// unlink(fullPath.c_str());
		(void)fullPath;
	}

	double percent = total > 0 ? (100.0 * confirmedCount / total) : 0.0;
	UploadLog("[ReVoice Upload] Done: uploaded=%d/%d, confirmed=%d/%d (%.1f%%)\n",
		uploadedCount, total, confirmedCount, total, percent);

	delete job;
	g_uploadInProgress = false;
	return nullptr;
}

void Cmd_UploadDump()
{
	UTIL_LogPrintf("[ReVoice Upload] rv_upload_dump invoked\n");

	if (g_uploadInProgress) {
		UTIL_LogPrintf("[ReVoice Upload] rejected: upload already in progress\n");
		SERVER_PRINT("[ReVoice] Upload already in progress.\n");
		return;
	}

	if (!g_pcv_rev_upload_url || !g_pcv_rev_upload_url->string || g_pcv_rev_upload_url->string[0] == '\0') {
		UTIL_LogPrintf("[ReVoice Upload] rejected: REV_UploadURL not set\n");
		SERVER_PRINT("[ReVoice] REV_UploadURL not set. Example: REV_UploadURL \"http://yourserver:5000/upload\"\n");
		return;
	}

	ParsedUrl url;
	if (!ParseHttpUrl(g_pcv_rev_upload_url->string, url)) {
		UTIL_LogPrintf("[ReVoice Upload] rejected: invalid REV_UploadURL '%s'\n", g_pcv_rev_upload_url->string);
		SERVER_PRINT("[ReVoice] Invalid REV_UploadURL. Must be http://host:port/path\n");
		return;
	}

	UploadJob *job = new UploadJob();
	job->url = url;
	ScanWavFiles("cstrike/data", "cstrike/data", job->files);

	if (job->files.empty()) {
		UTIL_LogPrintf("[ReVoice Upload] no WAV files found in cstrike/data/\n");
		SERVER_PRINT("[ReVoice] No WAV files found in cstrike/data/\n");
		delete job;
		return;
	}

	UTIL_LogPrintf("[ReVoice Upload] queued %d files for upload to http://%s:%s%s\n",
		(int)job->files.size(), url.host, url.port, url.path);

	g_uploadInProgress = true;

	pthread_t tid;
	if (pthread_create(&tid, nullptr, UploadThreadFunc, job) != 0) {
		UTIL_LogPrintf("[ReVoice Upload] failed to create worker thread\n");
		SERVER_PRINT("[ReVoice] Failed to create upload thread\n");
		delete job;
		g_uploadInProgress = false;
		return;
	}
	pthread_detach(tid);
}
