#include "precompiled.h"
#include "revoice_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

// Shared writer used by both RvLog and RvLogUpload. Static so it can't be
// called from outside the translation unit. Formats the line in a single
// stack buffer (no heap allocation, no shared state), opens the day's log
// file in append mode, writes the line as a single fwrite (atomic on POSIX
// for sizes ≤ PIPE_BUF=4096 when the fd was opened with O_APPEND), and
// closes. Every failure path returns silently.
//
// Uses localtime_r for thread safety — both threads can call WriteLogLine
// concurrently without contending for a static struct tm.
static void WriteLogLine(const char *fileNamePrefix, bool includeSvTime,
                         const char *fmt, va_list ap)
{
	if (!fmt || !fileNamePrefix) return;

	struct timeval tv;
	if (gettimeofday(&tv, nullptr) != 0) return;

	time_t nowSec = tv.tv_sec;
	struct tm tmvBuf;
	struct tm *tmv = localtime_r(&nowSec, &tmvBuf);
	if (!tmv) return;

	char line[2048];
	int hdrLen;
	if (includeSvTime) {
		// Reading g_RehldsSv->GetTime() here is safe because the caller
		// (RvLog) is only invoked from the main game thread.
		double svTime = -1.0;
		if (g_RehldsSv) svTime = g_RehldsSv->GetTime();
		hdrLen = snprintf(line, sizeof(line),
			"[%02d:%02d:%02d.%03d sv=%.3f] ",
			tmv->tm_hour, tmv->tm_min, tmv->tm_sec,
			(int)(tv.tv_usec / 1000),
			svTime);
	} else {
		hdrLen = snprintf(line, sizeof(line),
			"[%02d:%02d:%02d.%03d] ",
			tmv->tm_hour, tmv->tm_min, tmv->tm_sec,
			(int)(tv.tv_usec / 1000));
	}
	if (hdrLen < 0 || hdrLen >= (int)sizeof(line)) return;

	int bodyLen = vsnprintf(line + hdrLen, sizeof(line) - hdrLen, fmt, ap);
	if (bodyLen < 0) return;

	int totalLen = hdrLen + bodyLen;
	if (totalLen > (int)sizeof(line) - 2) totalLen = sizeof(line) - 2;
	line[totalLen]     = '\n';
	line[totalLen + 1] = '\0';
	totalLen += 1;

	char path[256];
	int pathLen = snprintf(path, sizeof(path),
		"cstrike/addons/amxmodx/logs/%s%04d%02d%02d.log",
		fileNamePrefix,
		tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday);
	if (pathLen < 0 || pathLen >= (int)sizeof(path)) return;

	FILE *f = fopen(path, "ab");
	if (!f) return;
	fwrite(line, 1, (size_t)totalLen, f);
	fclose(f);
}

void RvLog(const char *fmt, ...)
{
	// Cvar-gated kill switch. If the cvar pointer isn't ready yet (very
	// early boot, before Revoice_Init_Cvars), we still log — those
	// boot-time entries are worth keeping.
	if (g_pcv_rev_debug_log && g_pcv_rev_debug_log->value == 0.0f)
		return;

	va_list ap;
	va_start(ap, fmt);
	WriteLogLine("RV", true, fmt, ap);
	va_end(ap);
}

void RvLogUpload(const char *fmt, ...)
{
	// Same cvar gates both files. If you want to be able to silence the
	// main log while keeping upload-worker traces, split the gate into a
	// separate cvar — but normally they should rise and fall together.
	if (g_pcv_rev_debug_log && g_pcv_rev_debug_log->value == 0.0f)
		return;

	va_list ap;
	va_start(ap, fmt);
	WriteLogLine("RV_upload_", false, fmt, ap);
	va_end(ap);
}
