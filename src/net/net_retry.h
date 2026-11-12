/* ViTube — when a failed list load is worth another attempt, and when.
 *
 * Reported on hardware (2026-11-13): leave the app open, lock the console
 * with PS/power, wake it and go back in — and the app sits on its "no
 * videos loaded" error, with nothing saying it would ever try again, while
 * the Wi-Fi quietly came back a few seconds later. Suspend kills every
 * socket, and the console needs those few seconds to re-associate; the one
 * request made in that window failed, and that single failure was final.
 *
 * This is the policy half of the fix: pure arithmetic over a millisecond
 * clock, no psp2, no vita2d, no threads, so tests/test_net_retry.c can run
 * it on the build machine. The frames, the thread and the console clocks
 * that drive it live in src/ui/net_task.c and src/net/netwatch.c. */
#ifndef VITUBE_NET_NET_RETRY_H
#define VITUBE_NET_NET_RETRY_H

#include <stdint.h>

/* How long one load may keep retrying before its error is shown after all.
 * Re-association after a wake took "a few seconds" in the report; 45 s
 * leaves room for a slow access point and DHCP on top of that without
 * keeping someone on a spinner for longer than they would wait anyway
 * (Circle ends it at any time). */
#define VT_NET_RETRY_BUDGET_MS 45000u
/* Backoff after the n-th failure: 1 s, 2 s, 4 s, then every 8 s. */
#define VT_NET_RETRY_FIRST_DELAY_MS 1000u
#define VT_NET_RETRY_MAX_DELAY_MS 8000u
/* A hard stop independent of the clock, in case attempts fail instantly. */
#define VT_NET_RETRY_MAX_ATTEMPTS 10
/* How soon after the console reports the network up again the next attempt
 * goes out, instead of waiting out the rest of the backoff. Not zero: the
 * state flips to CONNECTED when the address is assigned, and the first
 * lookup straight after that is the one most likely to still fail. */
#define VT_NET_RETRY_SETTLE_MS 600u
/* Wall-clock jump between two samples, taken a fraction of a second apart,
 * that can only mean the console was asleep in between. */
#define VT_NET_RESUME_GAP_US 4000000ull

typedef enum {
    VT_NET_RETRY_WAIT = 0, /* keep showing the reconnect screen */
    VT_NET_RETRY_GO        /* make the next attempt now */
} VtNetRetryAction;

typedef struct {
    uint64_t started_ms;  /* start of the budget */
    uint64_t next_ms;     /* earliest time of the next attempt */
    int failures;         /* failed attempts so far */
    int was_connected;    /* last connectivity sample, for edge detection */
} VtNetRetry;

/* At the first attempt of a load. */
void vt_net_retry_start(VtNetRetry *r, uint64_t now_ms);

/* The console was suspended while this load was running or waiting: what
 * failed before the sleep says nothing about the network after it, so the
 * budget and the backoff start over. */
void vt_net_retry_resumed(VtNetRetry *r, uint64_t now_ms);

/* After a failed attempt. `transient` is vt_net_failure_transient()'s
 * verdict; `connected` is the console's state right now. Returns 1 when
 * another attempt is scheduled, 0 when the caller should give up and show
 * its error. */
int vt_net_retry_failed(VtNetRetry *r, uint64_t now_ms, int transient,
                        int connected);

/* Once per frame while waiting. Coming back online pulls the next attempt
 * forward to VT_NET_RETRY_SETTLE_MS from now. */
VtNetRetryAction vt_net_retry_poll(VtNetRetry *r, uint64_t now_ms,
                                   int connected);

/* Whole seconds until the next attempt, rounded up, for the countdown. */
int vt_net_retry_seconds_left(const VtNetRetry *r, uint64_t now_ms);

/* The backoff after `failures` failed attempts (failures >= 1). */
uint64_t vt_net_retry_delay_ms(int failures);

/* Is a failure the kind that another attempt can fix?
 *   aborted_by_resume — the attempt was in flight when the console slept
 *                        and was cut off on wake (its sockets were dead);
 *   connected         — the console's network state after the failure;
 *   transport_failed  — the HTTP layer saw a transport-level error (DNS,
 *                        connect, TLS, timeout) during the attempt.
 * Anything else — an HTTP error, an empty or unparsable answer, "not
 * signed in" — is YouTube's answer, and asking again will not change it. */
int vt_net_failure_transient(int aborted_by_resume, int connected,
                             int transport_failed);

/* Two wall-clock samples (microseconds) taken by a thread that sleeps a
 * fraction of a second between them: 1 when they are so far apart that the
 * console must have been suspended in between. A clock that went backwards
 * (the user set the time) is not a resume. */
int vt_net_is_resume_gap(uint64_t previous_us, uint64_t now_us);

#endif
