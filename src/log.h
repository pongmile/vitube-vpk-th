/* ViTube — tiny append-only diagnostics log (ux0:data/ViTube/log.txt).
 * The previous run's log survives one relaunch as log.prev.txt (rotated
 * out of the way, not overwritten) — check that one too after a crash. */
#ifndef VITUBE_LOG_H
#define VITUBE_LOG_H

void vt_log_init(void);            /* rotates in a fresh log for this run */
void vt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Global serialization for ALL of this app's ux0: file accesses. vt_log
 * holds it internally; any other code doing sceIo* on ux0: (e.g. the HD
 * preference file) must wrap those calls in this lock — concurrent SceIo
 * traffic from our many logger threads was pinned as the real-hardware
 * crash cause on 2026-08-27 (see log.c for the incident chain). */
void vt_io_lock(void);
void vt_io_unlock(void);

/* Diagnostics sink of the two decoder packages, defined strongly in log.c so
 * their internal error codes land in this same log instead of the weak no-op
 * they ship with (see log.c). Not meant to be called by app code — use
 * vt_log(). */
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
