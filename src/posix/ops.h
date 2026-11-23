// What the agent's actions turn into on Linux and macOS: processes, HTTP, files, timers.
//
// Every operation ends in exactly one event handed to the deliver callback
// (or none, if it was cancelled), and the callback only queues it: it must
// not call the agent, because the agent's actions are still being carried out
// when most of these run.
#ifndef BARB_POSIX_OPS_H
#define BARB_POSIX_OPS_H

#include <poll.h>
#include "../core/agent.h"

typedef void (*deliver_fn)(const ev_t* ev);

void ops_init(deliver_fn deliver, const char* base_url, const char* api_key);

void ops_http(int64_t id, str_t path, str_t body);
void ops_spawn(int64_t id, str_t cmd, int64_t timeout_ms, int bg, str_t log);
void ops_read(int64_t id, str_t path);
void ops_write(int64_t id, str_t path, str_t data);
void ops_timer(int64_t id, int64_t ms);
void ops_cancel(int64_t id);

// For the main loop's poll(): the descriptors operations want watched, how long
// it may sleep, and the call that does what is due (reads output, reaps children,
// fires timers, enforces time limits).
int ops_nfds(void);
void ops_fill(struct pollfd* fds);
int ops_timeout_ms(void);          // -1 if nothing is waiting on the clock
void ops_service(const struct pollfd* fds);

// Waits like poll(2) for POLLIN on the descriptors in fds, but through select(2): Apple's poll()
// does not support terminals. Descriptors that are not readable get revents 0; end of file counts as
// readable (the read then returns 0). A negative fd is ignored. timeout_ms < 0 waits for ever.
int ops_wait(struct pollfd* fds, nfds_t n, int timeout_ms);

void ops_shutdown(void);           // kill everything still running, remove temporary files

#endif
