// What the agent's actions turn into on Windows: processes, HTTP, files, timers.
//
// The same contract as src/posix/ops.h: every operation ends in exactly one event handed to
// the deliver callback (or none, if it was cancelled), and the callback only queues it.
//
// Commands run in Git Bash (Git for Windows), each in a job object, so stopping one stops
// everything it started. HTTP goes through curl.exe with the key on its standard input.
// Paths are UTF-8; the Git Bash spellings /c/... and /tmp/... are understood too.
#ifndef BARB_WIN32_OPS_H
#define BARB_WIN32_OPS_H

#include <wchar.h>
#include "../core/agent.h"

typedef void (*deliver_fn)(const ev_t* ev);

void ops_init(deliver_fn deliver, const char* base_url, const char* api_key);

void ops_http(int64_t id, str_t path, str_t body);
void ops_spawn(int64_t id, str_t cmd, int64_t timeout_ms, int bg, str_t log);
void ops_read(int64_t id, str_t path);
void ops_write(int64_t id, str_t path, str_t data);
void ops_timer(int64_t id, int64_t ms);
void ops_cancel(int64_t id);

// For the main loop's WaitForMultipleObjects: the handles operations want watched (up to
// `max`), how long it may sleep, and the call that does what is due (reads output, notices
// exits, fires timers, enforces time limits).
int ops_handles(void** handles, int max);
int ops_timeout_ms(void);              // -1 if nothing is waiting on the clock
void ops_service(void);

int64_t ops_now_ms(void);

// The bash commands run in (UTF-8), or 0 if no Git Bash was found.
const char* ops_shell(void);

void ops_shutdown(void);               // kill everything still running, remove temporary files

// UTF-8 and UTF-16, for main.c too. The result is malloc'd (0 if memory ran out).
wchar_t* ops_wide(const char* s, size_t n);
char* ops_utf8(const wchar_t* w, int n);   // n < 0: up to the NUL

#endif
