/* ViTube — InnerTube response parsing (pure C + jansson, host-testable). */
#ifndef VITUBE_YT_PARSE_H
#define VITUBE_YT_PARSE_H

#include "model.h"

/* Parse any list-shaped InnerTube response (search, next/related, browse):
 * collects compactVideoRenderer / videoWithContextRenderer / videoRenderer /
 * tileRenderer items and the first continuation token. Returns number of
 * items, or -1 on malformed JSON. */
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
