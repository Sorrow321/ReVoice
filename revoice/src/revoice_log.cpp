#include "precompiled.h"
#include "revoice_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

void RvLog(const char *fmt, ...)
{
	// Cvar-gated kill switch. If the cvar pointer isn't ready yet (very
	// early boot, before Revoice_Init_Cvars), we proceed and log — those
	// boot-time entries are worth keeping.
	if (g_pcv_rev_debug_log && g_pcv_rev_debug_log->value == 0.0f)
		return;

	if (!fmt) return;

	// Wall-clock with millisecond resolution. gettimeofday is POSIX and
	// sufficient for our timing needs (silence-gap math is ~100ms scale).
	struct timeval tv;
	if (gettimeofday(&tv, nullptr) != 0) return;

	time_t nowSec = tv.tv_sec;
	struct tm *tmv = localtime(&nowSec);   // single-threaded use only
	if (!tmv) return;

	// Server time, for correlating with silence-gap and changelevel logic.
	double svTime = -1.0;
	if (g_RehldsSv) svTime = g_RehldsSv->GetTime();

	// Build the entire line in a stack buffer first. Doing all formatting
	// before any I/O means a malformed format string can't leave a partial
	// half-flushed write on disk.
	char line[2048];
	int hdrLen = snprintf(line, sizeof(line),
		"[%02d:%02d:%02d.%03d sv=%.3f] ",
		tmv->tm_hour, tmv->tm_min, tmv->tm_sec,
		(int)(tv.tv_usec / 1000),
		svTime);
	if (hdrLen < 0 || hdrLen >= (int)sizeof(line)) return;

	va_list ap;
	va_start(ap, fmt);
	int bodyLen = vsnprintf(line + hdrLen, sizeof(line) - hdrLen, fmt, ap);
	va_end(ap);
	if (bodyLen < 0) return;

	int totalLen = hdrLen + bodyLen;
	if (totalLen > (int)sizeof(line) - 2) totalLen = sizeof(line) - 2;
	line[totalLen]     = '\n';
	line[totalLen + 1] = '\0';
	totalLen += 1;  // include the '\n' in the write count

	// Filename rolls daily: cstrike/addons/metamod/logs/RV20260509.log
	char path[256];
	int pathLen = snprintf(path, sizeof(path),
		"cstrike/addons/metamod/logs/RV%04d%02d%02d.log",
		tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday);
	if (pathLen < 0 || pathLen >= (int)sizeof(path)) return;

	// Append-only. fopen failure (no logs dir / EACCES / EMFILE) is silently
	// ignored — losing log lines is preferable to crashing the engine.
	FILE *f = fopen(path, "ab");
	if (!f) return;

	fwrite(line, 1, (size_t)totalLen, f);
	fclose(f);
}
