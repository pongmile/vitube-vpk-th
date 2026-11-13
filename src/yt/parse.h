/* ViTube — InnerTube response parsing (pure C + jansson, host-testable). */
#ifndef VITUBE_YT_PARSE_H
#define VITUBE_YT_PARSE_H

#include "model.h"

/* Parse any list-shaped InnerTube response (search, next/related, browse):
 * collects compactVideoRenderer / videoWithContextRenderer / videoRenderer /
 * tileRenderer items and the first continuation token — the next page's
 * (nextContinuationData / continuationCommand), never a reloadContinuationData.
 * Returns number of items, or -1 on malformed JSON. out->surface is left
 * YT_SURF_NONE: only the caller knows which request this response answered
 * (yt/innertube.c stamps it). */
int yt_parse_video_list(const char *json, size_t len, yt_video_list_t *out);

/* Parse a search response into BOTH lists in a single pass over a single
 * parsed tree. out_channels (optional, may be NULL) receives the channel
 * results the same response already carries — the ANDROID client ships them
 * as Elements "compactChannelData" items, typically 1-2 per query. Returns
 * the VIDEO count (or -1 on malformed JSON); check out_channels->count
 * separately, a query can legitimately return channels and no videos. */
int yt_parse_search(const char *json, size_t len, yt_video_list_t *out,
                    yt_channel_list_t *out_channels);

/* Parse a channel's Videos-tab browse response: same video collection as
 * yt_parse_video_list, plus (optionally) the channel's display name. */
int yt_parse_channel_page(const char *json, size_t len, yt_video_list_t *out,
                          char *out_title, size_t title_cap);

/* Parse a subscribed-channels list (browse "FEchannels") response. */
int yt_parse_channel_list(const char *json, size_t len, yt_channel_list_t *out);

/* What the most recent yt_parse_video_list / yt_parse_search /
 * yt_parse_channel_page / yt_parse_channel_list call had to throw away. Both
 * are 0 for a clean parse. They exist because each of these drops used to be
 * invisible: the list just ended, which is indistinguishable from "there is
 * no more". State is per process, reset by every parse entry point; parse
 * one response at a time.
 *
 * - dropped continuation length: the length of the next-page token that did
 *   not fit YT_CONTINUATION_MAX (the list then has no token, by design — a
 *   truncated one would be rejected by the API). The first token in document
 *   order is THE token; once it is dropped, later ones are not taken instead.
 * - dropped items: video-like rows beyond YT_LIST_MAX. An upper bound (a
 *   channel tile in a video list is counted too). A page that overflows loses
 *   those rows for good, since its continuation starts after them. */
int yt_parse_last_dropped_continuation_len(void);
int yt_parse_last_dropped_items(void);

/* The visitorData of a response: `"responseContext":{"visitorData":"..."}`,
 * the first occurrence in document order, copied to out. Returns its length,
 * or 0 (out is then ""): no such field, an empty value, a value with a JSON
 * escape in it, or one that does not fit cap. `json` must be NUL-terminated.
 * Not a JSON parse, on purpose — see parse.c. The visitor id is what a
 * browse continuation has to be sent with (yt_video_list_t.visitor). */
int yt_parse_visitor_data(const char *json, char *out, size_t cap);

/* Reorder a parsed list newest-first by each item's age_min. Stable: items
 * sharing a coarse age ("vor 1 Monat") or having none keep the order YouTube
 * returned them in. */
void yt_sort_newest_first(yt_video_list_t *list);

/* Parse a /player response: playability, metadata and every usable
 * progressive stream format (sorted best-first). Returns 0 on success
 * (even when playability is not OK — check out->playability), -1 on
 * malformed JSON. */
int yt_parse_player(const char *json, size_t len, yt_stream_t *out);

#endif
