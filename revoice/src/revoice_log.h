#pragma once

// Best-effort line-buffered loggers writing to date-stamped files under
//   cstrike/addons/amxmodx/logs/
//
//   RvLog        →  RVYYYYMMDD.log         (main game thread; includes sv=time)
//   RvLogUpload  →  RV_upload_YYYYMMDD.log (background upload worker thread; no sv=)
//
// Design goals:
//   - Logging must never crash the engine. Any failure (fopen denied,
//     format too long, gettimeofday error) is silently dropped.
//   - One fopen + one write + one fclose per line. No persistent FILE*
//     state to corrupt; no date-rollover detection needed.
//   - Both functions are thread-safe with respect to each other and
//     to themselves: they use localtime_r (not localtime), they write
//     to different files, and atomic-append (O_APPEND, <PIPE_BUF=4096)
//     keeps concurrent fwrite() calls from interleaving.
//   - Caller-supplied format strings must be string literals; %s args
//     must be guarded against NULL ("name ? name : \"(null)\"").
//
// The upload variant intentionally omits sv= because reading
// g_RehldsSv->GetTime() from a non-main thread is racy on 32-bit
// builds (8-byte double, non-atomic). The main thread uses sv= for
// correlation with frame-time events; the worker has nothing to
// correlate it against.

//   RvLogAction  →  RV_actions_YYYYMMDD.log (player-facing playback actions and
//                    their outcomes/issues; main game thread; not gated by
//                    REV_DebugLog so the audit trail is always kept).

#if defined(__GNUC__) || defined(__clang__)
void RvLog(const char *fmt, ...)       __attribute__((format(printf, 1, 2)));
void RvLogUpload(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void RvLogAction(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
void RvLog(const char *fmt, ...);
void RvLogUpload(const char *fmt, ...);
void RvLogAction(const char *fmt, ...);
#endif
