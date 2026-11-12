/* ViTube — retry policy for list loads; see net_retry.h for why it exists.
 * Deliberately free of psp2 so tests/test_net_retry.c can run it on the
 * build machine. */
#include "net/net_retry.h"

void vt_net_retry_start(VtNetRetry *r, uint64_t now_ms) {
    r->started_ms = now_ms;
    r->next_ms = now_ms;
    r->failures = 0;
    r->was_connected = 1;
}

void vt_net_retry_resumed(VtNetRetry *r, uint64_t now_ms) {
    r->started_ms = now_ms;
    r->failures = 0;
    /* Not "now": straight after a wake the radio is still off, and an
     * attempt that fails at once would only burn the first backoff step. */
    r->next_ms = now_ms + VT_NET_RETRY_SETTLE_MS;
    /* Whatever was sampled before the sleep is stale; with this the first
     * CONNECTED after the wake counts as the edge it is. */
    r->was_connected = 0;
}

uint64_t vt_net_retry_delay_ms(int failures) {
    uint64_t delay = VT_NET_RETRY_FIRST_DELAY_MS;
    for (int i = 1; i < failures && delay < VT_NET_RETRY_MAX_DELAY_MS; i++)
        delay *= 2;
    return delay > VT_NET_RETRY_MAX_DELAY_MS ? VT_NET_RETRY_MAX_DELAY_MS
                                             : delay;
}

int vt_net_retry_failed(VtNetRetry *r, uint64_t now_ms, int transient,
                        int connected) {
    r->failures++;
    r->was_connected = connected;
    if (!transient) return 0;
    if (r->failures >= VT_NET_RETRY_MAX_ATTEMPTS) return 0;
    uint64_t next = now_ms + vt_net_retry_delay_ms(r->failures);
    /* Do not schedule an attempt the budget no longer covers: giving up now
     * is better than a countdown that ends in the error anyway. A clock
     * that reads earlier than the start (it never should) counts as no
     * time spent rather than wrapping into "budget long gone". */
    uint64_t spent = next > r->started_ms ? next - r->started_ms : 0;
    if (spent > VT_NET_RETRY_BUDGET_MS) return 0;
    r->next_ms = next;
    return 1;
}

VtNetRetryAction vt_net_retry_poll(VtNetRetry *r, uint64_t now_ms,
                                   int connected) {
    if (connected && !r->was_connected &&
        r->next_ms > now_ms + VT_NET_RETRY_SETTLE_MS)
        r->next_ms = now_ms + VT_NET_RETRY_SETTLE_MS;
    r->was_connected = connected;
    return now_ms >= r->next_ms ? VT_NET_RETRY_GO : VT_NET_RETRY_WAIT;
}

int vt_net_retry_seconds_left(const VtNetRetry *r, uint64_t now_ms) {
    if (now_ms >= r->next_ms) return 0;
    return (int)((r->next_ms - now_ms + 999) / 1000);
}

int vt_net_failure_transient(int aborted_by_resume, int connected,
                             int transport_failed) {
    return aborted_by_resume || !connected || transport_failed;
}

int vt_net_is_resume_gap(uint64_t previous_us, uint64_t now_us) {
    return now_us > previous_us && now_us - previous_us > VT_NET_RESUME_GAP_US;
}
