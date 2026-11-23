// Tests for ops_wait (src/posix/ops.c): the main loop's wait, which is select(2) behind poll(2)'s
// interface because Apple's poll() cannot watch a terminal. It must report exactly what poll would
// for the cases the loop uses: readable, not readable, end of file, a timeout, and no timeout.
#include "check.h"
#include "../src/posix/ops.h"
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static struct pollfd want(int fd) {
    struct pollfd p;
    p.fd = fd; p.events = POLLIN; p.revents = 0;
    return p;
}

static void put(int fd) { if (write(fd, "x", 1) != 1) exit(2); }

static void test_nothing_ready(void) {
    int a[2];
    CHECK(pipe(a) == 0);
    struct pollfd p = want(a[0]);
    CHECK_INT(ops_wait(&p, 1, 0), 0);
    CHECK_INT(p.revents, 0);

    long long t0 = now_ms();
    CHECK_INT(ops_wait(&p, 1, 60), 0);
    long long dt = now_ms() - t0;
    CHECK(dt >= 50 && dt < 1000);

    t0 = now_ms();                      // more than a second: the seconds and the rest both count
    CHECK_INT(ops_wait(&p, 1, 1150), 0);
    dt = now_ms() - t0;
    CHECK(dt >= 1100 && dt < 2000);
    CHECK_INT(p.revents, 0);
    close(a[0]); close(a[1]);
}

static void test_ready(void) {
    int a[2], b[2];
    CHECK(pipe(a) == 0 && pipe(b) == 0);
    put(a[1]);
    struct pollfd p = want(a[0]);
    long long t0 = now_ms();
    CHECK_INT(ops_wait(&p, 1, 5000), 1);          // it does not sit out the timeout
    CHECK(now_ms() - t0 < 500);
    CHECK_INT(p.revents, POLLIN);
    CHECK_INT(ops_wait(&p, 1, -1), 1);            // and with no timeout at all it still returns
    CHECK_INT(p.revents, POLLIN);

    // Of two, only the one with data is reported, whichever comes first, and the highest descriptor
    // is watched like the others.
    struct pollfd two[2] = { want(a[0]), want(b[0]) };
    CHECK_INT(ops_wait(two, 2, 0), 1);
    CHECK_INT(two[0].revents, POLLIN);
    CHECK_INT(two[1].revents, 0);
    char c;
    CHECK(read(a[0], &c, 1) == 1);
    put(b[1]);
    two[0] = want(a[0]); two[1] = want(b[0]);
    CHECK_INT(ops_wait(two, 2, 0), 1);
    CHECK_INT(two[0].revents, 0);
    CHECK_INT(two[1].revents, POLLIN);
    put(a[1]);
    two[0] = want(a[0]); two[1] = want(b[0]);
    CHECK_INT(ops_wait(two, 2, 0), 2);
    CHECK_INT(two[0].revents, POLLIN);
    CHECK_INT(two[1].revents, POLLIN);
    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
}

static void test_odd_descriptors(void) {
    int a[2];
    CHECK(pipe(a) == 0);

    close(a[1]);                                    // end of file is readable: the read says 0
    struct pollfd p = want(a[0]);
    CHECK_INT(ops_wait(&p, 1, 0), 1);
    CHECK(p.revents & POLLIN);
    close(a[0]);

    CHECK(pipe(a) == 0);
    put(a[1]);
    struct pollfd two[2] = { want(-1), want(a[0]) };    // a negative descriptor is skipped
    CHECK_INT(ops_wait(two, 2, 0), 1);
    CHECK_INT(two[0].revents, 0);
    CHECK_INT(two[1].revents, POLLIN);

    struct pollfd none = want(-1);                      // and with nothing else it only waits
    long long t0 = now_ms();
    CHECK_INT(ops_wait(&none, 1, 40), 0);
    CHECK(now_ms() - t0 >= 30);

    struct pollfd bad = want(900);                      // not open: an answer, not a hang (kernels differ:
    CHECK(fcntl(900, F_GETFD) < 0);                     // select says EBADF on some, "nothing" on others)
    int rb = ops_wait(&bad, 1, 0);
    CHECK(rb == 0 || (rb == 1 && bad.revents == POLLNVAL));

    int high = dup2(a[0], 1100);                        // too high for select: handed to poll
    if (high == 1100) {
        struct pollfd h = want(high);
        CHECK_INT(ops_wait(&h, 1, 0), 1);
        CHECK_INT(h.revents, POLLIN);
        close(high);
    }
    close(a[0]); close(a[1]);
}

int main(void) {
    alarm(60);                          // a wait that never returns is a failure, not a hang
    test_nothing_ready();
    test_ready();
    test_odd_descriptors();
    return checks_report("test_wait");
}
