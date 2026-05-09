#pragma once

// Best-effort line-buffered logger to cstrike/addons/metamod/logs/RVYYYYMMDD.log.
//
// Design goals:
//   - The logger must never crash. Any failure (fopen denied, format too long,
//     stat() errno = anything) is silently dropped. Diagnostic logging that
//     can take down the server is worse than no logging.
//   - One open + one write + one close per line. No persistent FILE* state to
//     corrupt; no date-rollover detection needed (the filename is recomputed
//     from wall-clock each call).
//   - Single-threaded use only. The current build has no live worker threads
//     reaching this (the upload subsystem is disabled). If a worker thread
//     is ever re-introduced, wrap RvLog under the same mutex it uses.
//   - Format strings must be string literals at the call site, never user
//     input. vsnprintf with %s + a NULL pointer is undefined behaviour, so
//     callers must guard ("name ? name : \"(null)\"").
//
// File path: cstrike/addons/metamod/logs/RVYYYYMMDD.log
// Line format: [HH:MM:SS.mmm sv=12.345] <message>\n
#if defined(__GNUC__) || defined(__clang__)
void RvLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
void RvLog(const char *fmt, ...);
#endif
