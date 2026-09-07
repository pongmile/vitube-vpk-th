/* ViTube — the small pile of state that has to outlive a run, in one file
 * (ux0:data/ViTube/state.json): what has been watched, what has been searched
 * for, and how the channel list is sorted.
 *
 * One file rather than three, because each of these is a handful of bytes and
 * every ux0: access has to be serialized behind the same lock anyway (see
 * log.c) — three files would mean three times the lock traffic for no gain.
 *
 * Writes happen on change, not on a timer: adding to the history is once per
 * video, a search once per query, the sort once per press. */
#ifndef VITUBE_STORE_H
#define VITUBE_STORE_H

#include "yt/model.h"

/* Deliberately short. This is a "what did I just watch" list, not an archive,
 * and every entry costs ~665 bytes of BSS plus its share of a synchronous
 * card write on every change. */
#define VT_HISTORY_MAX 30
#define VT_SEARCHES_MAX 10

void vt_store_load(void); /* startup, after vt_log_init() */

/* Bumped on every change. A screen holding a copy of the history compares
 * it against the value it copied at, which is cheaper and more honest than
 * comparing counts: rewatching a video reorders the list without changing
 * its length. */
unsigned vt_store_revision(void);

int vt_store_history_count(void);
const yt_video_t *vt_store_history_at(int index); /* 0 = most recent */
/* Moves an already-present video back to the front instead of duplicating
 * it, and saves. */
void vt_store_history_add(const yt_video_t *video);

int vt_store_search_count(void);
const char *vt_store_search_at(int index); /* 0 = most recent */
void vt_store_search_add(const char *query);

/* Both cleared together: they are the two things a user emptying their
 * traces would mean by it. */
void vt_store_clear_personal(void);

/* Channel-list order. The values live here rather than in list_screen.c
 * because two screens need them: the Abos tab that applies the order and
 * the Mehr tab that mirrors it. */
enum { VT_CHANNEL_SORT_RECENT = 0, VT_CHANNEL_SORT_ALPHA };
int vt_store_channel_sort(void);
void vt_store_set_channel_sort(int sort);

#endif
