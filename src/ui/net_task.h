/* ViTube — vt_ui_run_task() for loads that need the network, with the
 * retrying that a console just woken from suspend needs. */
#ifndef VITUBE_UI_NET_TASK_H
#define VITUBE_UI_NET_TASK_H

#include "ui/ui.h"

/* Same contract as vt_ui_run_task(), and a drop-in for it: same spinner,
 * same Circle-cancels via *cancel_flag, same return value. The difference
 * is what happens when the attempt fails because the network was not there
 * (net/net_retry.h says which failures count): instead of returning that
 * failure for the caller to show as an error, it shows "Reconnecting ..."
 * with the reason and a countdown, and tries again — sooner once the
 * console reports the network back — until the load works, the budget runs
 * out (then the last failure is returned as before) or Circle is pressed
 * (then *cancel_flag is 1, exactly as for a cancelled spinner).
 *
 * An attempt still in flight when the console sleeps is cut off on wake —
 * its sockets died with the suspend, and curl would only find out from its
 * timeouts — and simply made again.
 *
 * fn may run several times with the same arg, so it must not keep state in
 * arg between runs beyond what it overwrites (every list task here fills its
 * page from scratch). The return value -2 ("not signed in") is an answer and
 * never retried. */
int vt_ui_run_net_task(const char *label, VtTaskFn fn, void *arg,
                       volatile int *cancel_flag);

#endif
