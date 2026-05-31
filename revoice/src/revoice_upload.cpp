#include "precompiled.h"
#include "revoice_upload.h"

#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <vector>
#include <string>
#include <utility>
#include <set>

// Hard total time budget for a single HTTP round-trip (connect + send + recv).
// Wall-clock seconds, enforced via a deadline + select() so that a slow or
// half-dead network cannot pin the worker thread for longer than this.
// Atomicity is preserved either way: a timeout means HttpPost returns -1,
// the file is not marked uploaded, no local copy is deleted, and the next
// rv_upload_dump retries.
static const int HTTP_TIMEOUT_SEC = 30;

// Single guard: at most one upload worker may run at a time. Set true in
// Cmd_UploadDump (main thread) before pthread_create; cleared by the worker
// on exit. volatile is sufficient here — single writer + single reader,
// no cross-thread ordering requirement beyond "worker eventually observes
// the true value, main eventually observes the false value." Both happen
// across pthread_create / pthread_exit synchronization points.
static volatile bool g_uploadInProgress = false;

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

// Percent-encode a path for use in single-line HTTP fields (the X-Filepath
// header and the verify-body lines). Preserves ASCII unreserved characters
// (RFC 3986: A-Z a-z 0-9 - _ . ~) plus '/' so path separators stay readable.
// Everything else — including every byte of a UTF-8 multibyte sequence such
// as the Cyrillic in "ушастик" — becomes %XX. This keeps the wire bytes pure
// ASCII, which round-trips cleanly through Python's http.server header parser
// (which decodes headers as Latin-1) — the receiver calls
// urllib.parse.unquote() to restore the original UTF-8.
static void PercentEncode(const char *src, std::string &out)
{
	out.clear();
	if (!src) return;
	static const char hex[] = "0123456789ABCDEF";
	for (; *src; ++src) {
		unsigned char c = (unsigned char)*src;
		if ((c >= 'A' && c <= 'Z') ||
		    (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') ||
		     c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
		{
			out.push_back((char)c);
		} else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0xF]);
		}
	}
}

// Block until `fd` is ready for read or write, or `deadline` (a wall-clock
// time_t) elapses. Returns 0 if ready, -1 on timeout / error / EOF.
// Retries on EINTR so a stray signal doesn't abort an otherwise-healthy op.
static int WaitForSocket(int fd, bool forRead, time_t deadline)
{
	for (;;) {
		time_t now = time(nullptr);
		if (now >= deadline) return -1;

		struct timeval tv;
		tv.tv_sec  = deadline - now;
		tv.tv_usec = 0;

		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);

		int rc = select(fd + 1,
			forRead  ? &fds : nullptr,
			forRead  ? nullptr : &fds,
			nullptr, &tv);
		if (rc > 0) return 0;        // ready
		if (rc == 0) return -1;       // hit deadline
		if (errno == EINTR) continue; // benign, retry remaining time
		return -1;                    // other error
	}
}

// Send all `len` bytes of `data` on a non-blocking socket, bounded by
// `deadline`. Returns false on timeout, connection failure, or partial write
// that can't complete in time. EAGAIN/EWOULDBLOCK from a non-blocking send
// is treated as "kernel buffer full" — we wait for writability and retry.
static bool SendAll(int fd, const char *data, size_t len, time_t deadline)
{
	size_t sent = 0;
	while (sent < len) {
		if (WaitForSocket(fd, false, deadline) < 0) return false;
		ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
			return false;
		}
		if (n == 0) return false;  // unexpected
		sent += (size_t)n;
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
	// One wall-clock deadline shared by connect + send + recv. If the
	// network is slow at any phase, the unused budget from earlier phases
	// flows into later ones; if any single phase exceeds the budget, we
	// bail and the caller (UploadOneFile / UploadThreadFunc) treats the
	// file as failed.
	const time_t deadline = time(nullptr) + HTTP_TIMEOUT_SEC;

	struct addrinfo hints = {}, *res = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(url.host, url.port, &hints, &res) != 0 || !res)
		return -1;

	int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sockfd < 0) { freeaddrinfo(res); return -1; }

	// Non-blocking for the entire socket lifetime. SO_SNDTIMEO /
	// SO_RCVTIMEO are NOT used — they don't affect connect() (which would
	// otherwise spin for ~75 s on a firewalled peer via kernel default
	// SYN-retry behaviour), and WaitForSocket() already enforces the
	// per-call timeout off the shared deadline.
	int oldFlags = fcntl(sockfd, F_GETFL, 0);
	if (oldFlags == -1 ||
	    fcntl(sockfd, F_SETFL, oldFlags | O_NONBLOCK) == -1)
	{
		freeaddrinfo(res);
		close(sockfd);
		return -1;
	}

	int rc = connect(sockfd, res->ai_addr, res->ai_addrlen);
	if (rc < 0 && errno != EINPROGRESS) {
		freeaddrinfo(res);
		close(sockfd);
		return -1;
	}

	if (rc < 0) {
		// EINPROGRESS: connect handshake started, wait for completion.
		if (WaitForSocket(sockfd, false, deadline) < 0) {
			freeaddrinfo(res);
			close(sockfd);
			return -1;
		}
		// Socket is writable — could mean "connected" OR "connect failed
		// with an asynchronous error". SO_ERROR distinguishes.
		int sockerr = 0;
		socklen_t errLen = sizeof(sockerr);
		if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &sockerr, &errLen) < 0
		    || sockerr != 0)
		{
			freeaddrinfo(res);
			close(sockfd);
			return -1;
		}
	}
	// rc == 0 → connect completed synchronously (uncommon but legal,
	// e.g. localhost). Either way, we're now connected.

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

	// snprintf returns the would-be length on truncation; sending hdrLen bytes from
	// a truncated buffer would read past the end of `header` and leak adjacent stack
	// memory onto the wire (and likely crash). Bail out instead.
	if (hdrLen < 0 || hdrLen >= (int)sizeof(header)) {
		close(sockfd);
		return -1;
	}

	if (!SendAll(sockfd, header, hdrLen, deadline)
	    || (bodyLen > 0 && !SendAll(sockfd, body, bodyLen, deadline)))
	{
		close(sockfd);
		return -1;
	}

	// Read full response (server sends Connection: close, so EOF marks end).
	// Each recv() is gated by WaitForSocket against the shared deadline, so
	// a hung peer can't drag the recv past HTTP_TIMEOUT_SEC total.
	std::string resp;
	char chunk[4096];
	while (resp.size() < 16 * 1024 * 1024) {  // 16 MB safety cap
		if (WaitForSocket(sockfd, true, deadline) < 0) break;
		ssize_t n = recv(sockfd, chunk, sizeof(chunk), 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
			break;  // hard error
		}
		if (n == 0) break;  // peer FIN
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

	// Percent-encode the relpath so non-ASCII filenames survive the
	// Latin-1 header decode on the Python side. relPath itself is kept
	// for logging.
	std::string encodedRel;
	PercentEncode(relPath, encodedRel);

	int code = HttpPost(url, url.path, encodedRel.c_str(), "application/octet-stream", buf, fileSize, nullptr);
	free(buf);

	if (code != 200) {
		if (code < 0)
			RvLogUpload("[upload] connection/timeout (≤%ds) — skipping: %s",
				HTTP_TIMEOUT_SEC, relPath);
		else
			RvLogUpload("[upload] HTTP %d — skipping: %s", code, relPath);
		return false;
	}
	return true;
}

static void *UploadThreadFunc(void *arg)
{
	UploadJob *job = (UploadJob *)arg;
	int total = (int)job->files.size();

	// NOTE: this build runs with -fno-exceptions, so std::bad_alloc and friends
	// would terminate the process rather than unwind the stack here. Every
	// early return below performs the cleanup (delete job; g_uploadInProgress=false)
	// explicitly.

	RvLogUpload("[upload] starting dump of %d files to http://%s:%s%s",
		total, job->url.host, job->url.port, job->url.path);

	// Phase 0: receiver health check. Empty POST /verify is treated as a ping
	// by the receiver. If this fails, the receiver is down/hung/unreachable;
	// abort the entire job — no uploads attempted, no files touched.
	{
		std::string ping;
		int code = HttpPost(job->url, "/verify", nullptr, "text/plain", "", 0, &ping);
		if (code != 200) {
			RvLogUpload("[upload] ABORT: receiver health check failed (code=%d). "
				"No files uploaded, no files deleted.", code);
			delete job;
			g_uploadInProgress = false;
			return nullptr;
		}
		RvLogUpload("[upload] receiver healthy, beginning upload");
	}

	std::vector<bool> uploaded(total, false);
	std::vector<bool> confirmed(total, false);
	int uploadedCount = 0;

	// Phase 1: upload each file. A file that fails here is left on disk for
	// the next rv_upload_dump to retry — we never delete a file that wasn't
	// confirmed.
	for (int i = 0; i < total; i++) {
		const std::string &fullPath = job->files[i].first;
		const std::string &relPath  = job->files[i].second;

		if (UploadOneFile(job->url, fullPath.c_str(), relPath.c_str())) {
			uploaded[i] = true;
			uploadedCount++;
			RvLogUpload("[upload] [%d/%d] OK: %s", i + 1, total, relPath.c_str());
		} else {
			RvLogUpload("[upload] [%d/%d] FAIL: %s", i + 1, total, relPath.c_str());
		}
	}

	// Phase 2: ask the receiver to confirm which uploads actually landed on
	// disk. Body is one relpath per line; response is "OK <relpath>" /
	// "MISSING <relpath>" per line. A file is only deleted locally if the
	// receiver explicitly confirms it.
	int confirmedCount = 0;
	if (uploadedCount > 0) {
		std::string verifyBody;
		verifyBody.reserve((size_t)uploadedCount * 80);
		std::string encoded;
		for (int i = 0; i < total; i++) {
			if (uploaded[i]) {
				// Same percent-encoding scheme as the X-Filepath header,
				// so the receiver decodes both via urllib.parse.unquote().
				PercentEncode(job->files[i].second.c_str(), encoded);
				verifyBody += encoded;
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
			RvLogUpload("[upload] /verify request failed (code=%d) — no files will be deleted", code);
		}
	}

	// Phase 3: delete confirmed files locally.
	//
	// SAFETY — every fullPath reaching unlink() satisfies all four:
	//   (1) S_ISREG(st.st_mode) — regular file, not a directory, not a
	//       directory through a symlink (stat would resolve the symlink
	//       and the if-branch above this code calls ScanWavFiles
	//       recursively only on S_ISDIR; a symlink-to-dir would land
	//       in S_ISDIR and recurse, not in S_ISREG).
	//   (2) filename ends in ".wav" (case-insensitive, via strcasecmp).
	//   (3) path is rooted at "cstrike/data/" (the initial dir argument
	//       to ScanWavFiles; recursion only descends into subdirs of it).
	//   (4) confirmed[i] is true ONLY when the Python /verify response
	//       contained literally "OK <relpath>" for this exact relpath.
	//
	// Therefore unlink() can NEVER reach cstrike/, the engine binary, a
	// configuration file, or any non-.wav. The worst case it can do is
	// erase a .wav that was placed in cstrike/data/ by someone other than
	// the recorder — but the upload+verify chain already proved that copy
	// exists on the receiver, so that's the intended behaviour.
	int deletedCount = 0;
	int unlinkErrors = 0;
	// Parent directories of every successfully-unlinked file. std::set
	// dedupes (one dir typically contains many wavs) and keeps memory
	// bounded by number of distinct players, not number of files.
	std::set<std::string> emptiedDirs;
	for (int i = 0; i < total; i++) {
		if (!confirmed[i]) continue;
		const std::string &fullPath = job->files[i].first;
		if (unlink(fullPath.c_str()) == 0) {
			deletedCount++;
			RvLogUpload("[upload] [%d/%d] deleted: %s",
				deletedCount, confirmedCount, fullPath.c_str());
			size_t slash = fullPath.find_last_of('/');
			if (slash != std::string::npos)
				emptiedDirs.insert(fullPath.substr(0, slash));
		} else {
			unlinkErrors++;
			RvLogUpload("[upload] WARN unlink failed: %s (errno=%d)",
				fullPath.c_str(), errno);
		}
	}

	// Remove now-empty per-player subdirectories. rmdir() refuses to
	// remove a non-empty directory (ENOTEMPTY), so it's self-policing:
	// if a directory still contains an in-progress recording, a wav we
	// failed to confirm, or any other file, it stays untouched.
	//
	// SAFETY — explicit floor: never remove "cstrike/data" itself or any
	// shorter path. ScanWavFiles only ever produces paths under
	// "cstrike/data/<auth>/<file>.wav", so the only parents that reach
	// here are "cstrike/data/<auth>", which is exactly what we want to
	// clean up. The startsWith check is belt-and-suspenders.
	int dirsRemoved = 0;
	for (const std::string &d : emptiedDirs) {
		if (d.compare(0, 13, "cstrike/data/") != 0) continue;  // not under our root
		if (d == "cstrike/data") continue;                      // is our root
		if (rmdir(d.c_str()) == 0) {
			dirsRemoved++;
			RvLogUpload("[upload] removed empty dir: %s", d.c_str());
		}
		// Common failure modes:
		//   ENOTEMPTY: directory still has files (e.g., in-progress wav,
		//              an unconfirmed leftover). Correct outcome — keep it.
		//   ENOENT:    something else removed it between unlink and rmdir.
		//              No problem.
		//   EACCES / EBUSY: rare, leave alone. Manual cleanup if needed.
	}

	double percent = total > 0 ? (100.0 * confirmedCount / total) : 0.0;
	RvLogUpload("[upload] done: uploaded=%d/%d, confirmed=%d/%d (%.1f%%), deleted=%d, dirs_removed=%d, unlink_errors=%d",
		uploadedCount, total, confirmedCount, total, percent,
		deletedCount, dirsRemoved, unlinkErrors);

	delete job;
	g_uploadInProgress = false;
	return nullptr;
}

// ── Auto-dump scheduler ─────────────────────────────────────────────────
// Persisted state lives in this file. Format: a single line "YYYY-MM-DD\n"
// recording the local calendar date of the last auto-trigger. Manual
// rv_upload_dump invocations do NOT touch this file — they are independent
// of the auto schedule.
static const char *AUTO_DUMP_STATE_PATH = "cstrike/addons/amxmodx/logs/rv_last_dump.txt";

// Earliest local hour-of-day at which an auto-dump is allowed to fire on
// a day change. Pinned at 04:00 so the heavy work happens when no players
// are on the server.
static const int AUTO_DUMP_HOUR_GATE = 4;

// Read the persisted last-dump date into `out` ("YYYY-MM-DD\0", 11 bytes).
// Returns true on success, false on absent/unreadable/malformed file.
// On every exit path the FILE* is closed — no fd leak even on early bails.
static bool AutoDump_ReadLastDate(char out[11])
{
	FILE *f = fopen(AUTO_DUMP_STATE_PATH, "rb");
	if (!f) return false;

	char buf[32] = {0};
	size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	if (rd == 0) return false;
	buf[rd] = '\0';

	// Trim trailing whitespace / newline so we tolerate hand-edited files.
	for (size_t i = 0; i < rd; i++) {
		char c = buf[i];
		if (c == '\r' || c == '\n' || c == ' ' || c == '\t') { buf[i] = '\0'; break; }
	}

	// Must be exactly "YYYY-MM-DD". A malformed file is treated as "no
	// record" → triggers an immediate dump on the next call. That's
	// fail-open, which matches the user-requested "missing file => fire"
	// behaviour.
	if (strlen(buf) != 10) return false;

	memcpy(out, buf, 11);
	return true;
}

// Overwrite the last-dump date file. Failures are logged but not fatal —
// at worst we'll try to fire again on the next map change today, and
// Cmd_UploadDump's g_uploadInProgress guard prevents overlapping workers.
static void AutoDump_WriteLastDate(const char *dateStr)
{
	FILE *f = fopen(AUTO_DUMP_STATE_PATH, "wb");
	if (!f) {
		RvLog("[AUTO] WARN cannot open %s for write: errno=%d",
			AUTO_DUMP_STATE_PATH, errno);
		return;
	}
	fwrite(dateStr, 1, strlen(dateStr), f);
	fputc('\n', f);
	fclose(f);
}

void Revoice_AutoDump_MaybeTrigger()
{
	if (!g_pcv_rev_auto_upload_dump || g_pcv_rev_auto_upload_dump->value == 0.0f)
		return;

	time_t now = time(nullptr);
	struct tm tmvBuf;
	struct tm *tmv = localtime_r(&now, &tmvBuf);
	if (!tmv) return;

	char todayStr[11];   // "YYYY-MM-DD\0"
	snprintf(todayStr, sizeof(todayStr), "%04d-%02d-%02d",
		tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday);

	char lastStr[11] = {0};
	bool haveLast = AutoDump_ReadLastDate(lastStr);

	// Decision:
	//   no file              → trigger
	//   file == today        → skip (already done today)
	//   file != today, h>=4  → trigger
	//   file != today, h<4   → skip (still pre-dawn; wait)
	bool shouldTrigger = false;
	if (!haveLast) {
		shouldTrigger = true;
		RvLog("[AUTO] no state file at %s — firing initial dump (today=%s)",
			AUTO_DUMP_STATE_PATH, todayStr);
	} else if (strcmp(lastStr, todayStr) == 0) {
		return;  // already fired today
	} else if (tmv->tm_hour >= AUTO_DUMP_HOUR_GATE) {
		shouldTrigger = true;
		RvLog("[AUTO] date rolled %s -> %s (hour=%d >= gate=%d) — firing dump",
			lastStr, todayStr, tmv->tm_hour, AUTO_DUMP_HOUR_GATE);
	} else {
		// new day but still before 04:00 — wait
		return;
	}

	if (!shouldTrigger) return;

	// Persist the new date BEFORE triggering. If we wrote it AFTER and the
	// engine crashed/restarted during the upload, we'd re-fire on every map
	// change for the rest of today. Writing first is the conservative
	// choice — if the user wants to retry today after a known failure, they
	// can always invoke `rv_upload_dump` manually (which is independent of
	// the state file).
	AutoDump_WriteLastDate(todayStr);

	Cmd_UploadDump();
}

void Cmd_UploadDump()
{
	// All console output for the cmd-trigger phase goes to the main game
	// thread's console only — we are still on the main thread here. The
	// worker takes over for the rest via RvLogUpload.
	SERVER_PRINT("[ReVoice Upload] rv_upload_dump invoked\n");

	if (g_uploadInProgress) {
		SERVER_PRINT("[ReVoice Upload] rejected: upload already in progress\n");
		return;
	}

	if (!g_pcv_rev_upload_url || !g_pcv_rev_upload_url->string || g_pcv_rev_upload_url->string[0] == '\0') {
		SERVER_PRINT("[ReVoice Upload] rejected: REV_UploadURL not set. Example: REV_UploadURL \"http://yourserver:5000/upload\"\n");
		return;
	}

	ParsedUrl url;
	if (!ParseHttpUrl(g_pcv_rev_upload_url->string, url)) {
		char msg[512];
		snprintf(msg, sizeof(msg),
			"[ReVoice Upload] rejected: invalid REV_UploadURL '%s' (must be http://host:port/path)\n",
			g_pcv_rev_upload_url->string);
		SERVER_PRINT(msg);
		return;
	}

	UploadJob *job = new UploadJob();
	job->url = url;
	ScanWavFiles("cstrike/data", "cstrike/data", job->files);

	if (job->files.empty()) {
		SERVER_PRINT("[ReVoice Upload] no WAV files found in cstrike/data/\n");
		delete job;
		return;
	}

	{
		char msg[256];
		snprintf(msg, sizeof(msg),
			"[ReVoice Upload] queued %d files for upload to http://%s:%s%s\n",
			(int)job->files.size(), url.host, url.port, url.path);
		SERVER_PRINT(msg);
	}

	g_uploadInProgress = true;

	pthread_t tid;
	if (pthread_create(&tid, nullptr, UploadThreadFunc, job) != 0) {
		SERVER_PRINT("[ReVoice Upload] failed to create worker thread\n");
		delete job;
		g_uploadInProgress = false;
		return;
	}
	pthread_detach(tid);
}
