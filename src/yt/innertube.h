/* ViTube — InnerTube API (YouTube Android/TV clients). */
#ifndef VITUBE_YT_INNERTUBE_H
#define VITUBE_YT_INNERTUBE_H

#include "yt/model.h"

/* All calls return 0 on success, <0 on failure. Setting *cancel = 1 from
 * another thread aborts the request (may be NULL). */
/* Search. out_channels (optional, may be NULL) receives the channel results
 * the SAME response already carries — no second request is issued. Channels
 * only appear on the first page, so yt_continue() keeps its video-only
 * signature. */
int yt_search(const char *query, volatile int *cancel, yt_video_list_t *out,
              yt_channel_list_t *out_channels);
int yt_continue(const char *continuation, volatile int *cancel,
                yt_video_list_t *out);
int yt_related(const char *video_id, volatile int *cancel,
               yt_video_list_t *out);

/* Personalized "what to watch" home feed. Empty when logged out or the
 * account has no watch history yet — never a hard error. */
int yt_home_feed(volatile int *cancel, yt_video_list_t *out);

/* Subscription feed: latest videos across all subscribed channels.
 * Requires login; returns -2 when logged out. */
int yt_subscriptions_feed(volatile int *cancel, yt_video_list_t *out);

/* The user's subscribed channels (for drilling into one channel).
 * Requires login; returns -2 when logged out. */
int yt_channel_list(volatile int *cancel, yt_channel_list_t *out);

/* A single channel's own Videos tab — public, no login required.
 * channel_title (optional, may be NULL) receives the channel's display
 * name. */
int yt_channel_videos(const char *channel_id, volatile int *cancel,
                      yt_video_list_t *out, char *channel_title,
                      size_t channel_title_cap);

/* Subscribe to (subscribe != 0) or unsubscribe from a channel. Requires
 * login; returns -2 when logged out, -1 on transport/API failure, 0 when
 * YouTube accepted the write. Subscribing to an already-subscribed channel
 * is a server-side no-op, so callers may fire it without knowing the
 * current state. */
int yt_subscribe(const char *channel_id, int subscribe, volatile int *cancel);

/* Resolve every usable progressive stream format for a video. */
int yt_resolve_stream(const char *video_id, volatile int *cancel,
                      yt_stream_t *out);

/* URL of the video's HLS master playlist — the app's own route to 720p, with
 * no server, no account and no JavaScript involved (see IT_CLIENT_VISIONOS in
 * innertube.c). Returns 0 and fills url_out, or <0 when YouTube offers no
 * playable manifest (age-gated, members-only, region-locked). */
int yt_hls_manifest(const char *video_id, volatile int *cancel,
                    char *url_out, size_t url_cap);

/* Human-readable cause of the last failed/empty API call ("" if none). */
const char *yt_last_error(void);

#endif
