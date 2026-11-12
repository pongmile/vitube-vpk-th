/* Host-side tests for the list-load retry policy (src/net/net_retry.c).
 * The module is deliberately free of psp2 so this runs on the build machine:
 * which failures are retried at all, the backoff steps, where the budget
 * ends, and the two clock edges the device would otherwise only show after
 * a real suspend — a reconnect pulling the next attempt forward, and a wake
 * restarting the budget.
 *
 *   gcc -Isrc -Wall -Werror -o /tmp/tnr tests/test_net_retry.c \
 *       src/net/net_retry.c && /tmp/tnr
 */
#include "../src/net/net_retry.h"

#include <stdio.h>

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

/* An arbitrary non-zero start, so nothing passes by accident at t = 0. */
#define T0 1000000ull

static void test_transient(void) {
    CHECK(vt_net_failure_transient(1, 1, 0), "cut off by a suspend");
    CHECK(vt_net_failure_transient(0, 0, 0), "offline");
    CHECK(vt_net_failure_transient(0, 1, 1), "transport error while online");
    CHECK(!vt_net_failure_transient(0, 1, 0),
          "online, no transport error: YouTube's answer, not an outage");
}

static void test_backoff(void) {
    CHECK(vt_net_retry_delay_ms(1) == 1000, "1st: %llu",
          (unsigned long long)vt_net_retry_delay_ms(1));
    CHECK(vt_net_retry_delay_ms(2) == 2000, "2nd");
    CHECK(vt_net_retry_delay_ms(3) == 4000, "3rd");
    CHECK(vt_net_retry_delay_ms(4) == 8000, "4th");
    CHECK(vt_net_retry_delay_ms(5) == 8000, "capped");
    CHECK(vt_net_retry_delay_ms(1000) == 8000, "no overflow far out");
}

static void test_permanent_failure_gives_up_at_once(void) {
    VtNetRetry r;
    vt_net_retry_start(&r, T0);
    CHECK(!vt_net_retry_failed(&r, T0 + 300, 0, 1),
          "a non-transient failure is reported straight away");
}

/* The reported case: woken, Wi-Fi back a few seconds later. */
static void test_resume_then_reconnect(void) {
    VtNetRetry r;
    vt_net_retry_start(&r, T0);
    uint64_t t = T0 + 200; /* first attempt failed fast: no route */
    CHECK(vt_net_retry_failed(&r, t, 1, 0), "retry scheduled");
    CHECK(vt_net_retry_seconds_left(&r, t) == 1, "countdown %d",
          vt_net_retry_seconds_left(&r, t));
    CHECK(vt_net_retry_poll(&r, t + 500, 0) == VT_NET_RETRY_WAIT, "waits");
    CHECK(vt_net_retry_poll(&r, t + 1000, 0) == VT_NET_RETRY_GO,
          "goes on schedule even while SceNetCtl says offline");
    /* second attempt fails as well, backoff 2 s */
    t += 1300;
    CHECK(vt_net_retry_failed(&r, t, 1, 0), "second retry scheduled");
    CHECK(r.next_ms == t + 2000, "2 s backoff");
    /* 300 ms later the console reports CONNECTED: the edge pulls the
     * attempt forward to the settle delay */
    CHECK(vt_net_retry_poll(&r, t + 300, 1) == VT_NET_RETRY_WAIT,
          "not instantly — settle first");
    CHECK(r.next_ms == t + 300 + VT_NET_RETRY_SETTLE_MS, "pulled forward");
    CHECK(vt_net_retry_poll(&r, t + 300 + VT_NET_RETRY_SETTLE_MS, 1) ==
              VT_NET_RETRY_GO, "goes once settled");
}

static void test_edge_never_pushes_back(void) {
    VtNetRetry r;
    vt_net_retry_start(&r, T0);
    CHECK(vt_net_retry_failed(&r, T0, 1, 0), "scheduled");
    /* due in 1000 ms; reconnect at 900 ms must not delay it to 1500 */
    vt_net_retry_poll(&r, T0 + 900, 1);
    CHECK(r.next_ms == T0 + 1000, "kept %llu",
          (unsigned long long)(r.next_ms - T0));
}

static void test_steady_connected_no_edge(void) {
    VtNetRetry r;
    vt_net_retry_start(&r, T0);
    /* online the whole time, transport error (e.g. DNS hiccup) */
    CHECK(vt_net_retry_failed(&r, T0, 1, 1), "scheduled");
    CHECK(vt_net_retry_failed(&r, T0 + 1000, 1, 1), "scheduled");
    CHECK(r.next_ms == T0 + 3000, "plain backoff without an edge");
    CHECK(vt_net_retry_poll(&r, T0 + 1500, 1) == VT_NET_RETRY_WAIT, "waits");
    CHECK(r.next_ms == T0 + 3000, "no edge, nothing pulled forward");
}

static void test_budget(void) {
    VtNetRetry r;
    vt_net_retry_start(&r, T0);
    uint64_t t = T0;
    int scheduled = 0;
    /* every attempt fails instantly at its due time */
    while (vt_net_retry_failed(&r, t, 1, 0)) {
        scheduled++;
        CHECK(r.next_ms - T0 <= VT_NET_RETRY_BUDGET_MS,
              "attempt %d at %llu ms is past the budget", scheduled,
              (unsigned long long)(r.next_ms - T0));
        t = r.next_ms;
        if (scheduled > 50) break;
    }
    /* 1+2+4+8+8+8+8 = 39 s; one more 8 s step would be 47 s > 45 s */
    CHECK(scheduled == 7, "retries within the budget: %d", scheduled);
    CHECK(r.failures == 8, "failures counted: %d", r.failures);
}

static void test_attempt_cap(void) {
    VtNetRetry r;
    vt_net_retry_start(&r, T0);
    int scheduled = 0;
    /* a clock that does not move: only the attempt cap can end this */
    while (vt_net_retry_failed(&r, T0, 1, 0) && scheduled < 100) scheduled++;
    CHECK(scheduled == VT_NET_RETRY_MAX_ATTEMPTS - 1, "cap: %d", scheduled);
}

static void test_resume_restarts_budget(void) {
    VtNetRetry r;
    vt_net_retry_start(&r, T0);
    uint64_t t = T0;
    while (vt_net_retry_failed(&r, t, 1, 0)) t = r.next_ms;
    /* budget gone — then the console sleeps and wakes */
    uint64_t wake = t + 600000;
    vt_net_retry_resumed(&r, wake);
    CHECK(r.failures == 0, "failures reset");
    CHECK(r.next_ms == wake + VT_NET_RETRY_SETTLE_MS, "settle after wake");
    CHECK(vt_net_retry_failed(&r, wake + 100, 1, 0),
          "a failure right after the wake is retried again");
    /* the first CONNECTED after the wake is an edge even though the last
     * sample before the sleep was connected too */
    VtNetRetry s;
    vt_net_retry_start(&s, T0);
    CHECK(vt_net_retry_failed(&s, T0, 1, 1), "scheduled");
    vt_net_retry_resumed(&s, T0 + 5000);
    CHECK(vt_net_retry_failed(&s, T0 + 5100, 1, 0), "post-wake failure");
    CHECK(vt_net_retry_poll(&s, T0 + 5200, 1) == VT_NET_RETRY_WAIT, "wait");
    CHECK(s.next_ms == T0 + 5200 + VT_NET_RETRY_SETTLE_MS,
          "reconnect after wake pulls forward");
}

static void test_seconds_left(void) {
    VtNetRetry r;
    vt_net_retry_start(&r, T0);
    r.next_ms = T0 + 4000;
    CHECK(vt_net_retry_seconds_left(&r, T0) == 4, "4");
    CHECK(vt_net_retry_seconds_left(&r, T0 + 1) == 4, "rounds up");
    CHECK(vt_net_retry_seconds_left(&r, T0 + 3001) == 1, "last second is 1");
    CHECK(vt_net_retry_seconds_left(&r, T0 + 4000) == 0, "due");
    CHECK(vt_net_retry_seconds_left(&r, T0 + 9000) == 0, "overdue");
}

static void test_resume_gap(void) {
    CHECK(!vt_net_is_resume_gap(T0, T0 + 250000), "a normal tick");
    CHECK(!vt_net_is_resume_gap(T0, T0 + 3000000), "a slow tick");
    CHECK(vt_net_is_resume_gap(T0, T0 + 4000001), "just past the gap");
    CHECK(vt_net_is_resume_gap(T0, T0 + 3600000000ull), "an hour asleep");
    CHECK(!vt_net_is_resume_gap(T0 + 3600000000ull, T0),
          "clock set back is not a wake");
    CHECK(!vt_net_is_resume_gap(T0, T0), "no time at all");
}

int main(void) {
    test_transient();
    test_backoff();
    test_permanent_failure_gives_up_at_once();
    test_resume_then_reconnect();
    test_edge_never_pushes_back();
    test_steady_connected_no_edge();
    test_budget();
    test_attempt_cap();
    test_resume_restarts_budget();
    test_seconds_left();
    test_resume_gap();
    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL NET RETRY TESTS PASSED\n");
    return 0;
}
