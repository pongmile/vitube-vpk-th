/* ViTube — notices when the console wakes from suspend and when the network
 * comes and goes, for the code that has to react to it (src/ui/net_task.c,
 * the empty Start screen in list_screen.c).
 *
 * A background thread rather than a check in each frame loop: the app has
 * a dozen loops of its own (lists, player, spinners, toasts, the keyboard),
 * and a sleep can land in any of them. */
#ifndef VITUBE_NET_NETWATCH_H
#define VITUBE_NET_NETWATCH_H

/* After http_init() succeeded; stop before http_shutdown(), since the
 * thread asks SceNetCtl for its state. */
void vt_netwatch_start(void);
void vt_netwatch_stop(void);

/* Bumped once per detected wake from suspend. Compare against a copy taken
 * earlier to learn whether the console slept in between. */
unsigned vt_netwatch_resume_count(void);
/* Bumped on every wake and every time the network comes back up — "a good
 * moment to try again whatever failed". */
unsigned vt_netwatch_generation(void);
/* SceNetCtl's view, refreshed a few times a second. 1 before the thread has
 * run, so nothing waits on a watcher that failed to start. */
int vt_netwatch_connected(void);

#endif
