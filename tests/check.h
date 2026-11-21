// A tiny test harness: CHECK counts failures and prints where, and the
// test program's exit status says whether any happened.
#ifndef BARB_CHECK_H
#define BARB_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core/str.h"

static int checks_run, checks_failed;

#define CHECK(cond) do { \
    checks_run++; \
    if (!(cond)) { checks_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_STR(got, want) do { \
    str_t g_ = (got); const char* w_ = (want); \
    checks_run++; \
    if (!str_eqz(g_, w_)) { checks_failed++; \
        printf("FAIL %s:%d: got \"%.*s\", want \"%s\"\n", __FILE__, __LINE__, (int)g_.n, g_.p ? g_.p : "", w_); } \
} while (0)

#define CHECK_INT(got, want) do { \
    long long g_ = (long long)(got), w_ = (long long)(want); \
    checks_run++; \
    if (g_ != w_) { checks_failed++; \
        printf("FAIL %s:%d: %s is %lld, want %lld\n", __FILE__, __LINE__, #got, g_, w_); } \
} while (0)

static int checks_report(const char* name) {
    printf("%s: %d checks, %d failed\n", name, checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}

// A mem_t over the C heap, optionally failing after `fail_after` allocations.
static long mem_count, mem_fail_after = -1;
static long mem_live;
static void* t_alloc(size_t n) {
    if (mem_fail_after >= 0 && mem_count >= mem_fail_after) return 0;
    mem_count++;
    void* p = malloc(n);
    if (p) mem_live++;
    return p;
}
static void t_release(void* p) { if (p) { mem_live--; free(p); } }
static const mem_t test_mem = { t_alloc, t_release };

// For the test code's own work, which out-of-memory injection must not touch.
static void* p_alloc(size_t n) { return malloc(n); }
static void p_release(void* p) { free(p); }
static const mem_t plain_mem = { p_alloc, p_release };

#endif
