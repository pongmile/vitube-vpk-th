/* ViTube — data model shared between parser, UI and player. */
#ifndef VITUBE_YT_MODEL_H
#define VITUBE_YT_MODEL_H

#include <stddef.h>

#define YT_VIDEO_ID_LEN 12       /* 11 chars + NUL */
#define YT_CHANNEL_ID_LEN 25     /* "UC" + 22 chars + NUL */
#define YT_TITLE_MAX 160
#define YT_CHANNEL_MAX 80
#define YT_META_MAX 64
/* Real i.ytimg.com/yt3.ggpht.com URLs commonly run 150-170+ bytes once
 * their sqp/rs query parameters are included (verified against live
 * fixtures) — keep generous headroom above that. */
#define YT_THUMB_URL_MAX 256
#define YT_URL_MAX 4096
/* Room for one continuation token, NUL included. A token that does not fit
 * is dropped whole (parse.c set_continuation), never truncated: a cut token
 * looks valid to every caller and is rejected by the API.
 *
 * 2048 was too small for the surface this app lands on first: the anonymous
 * VISIONOS home feed (browse, www.youtube.com) hands out a 4482-character
 * next-page token on page 1 (checked 2026-11-13 in
 * tests/fixtures/browse_home_visionos.json), so the Start tab had no token
 * and could never load a second page. And a token grows with every page it
 * is passed on: measured live on that feed, page 2 returns a 7498-character
 * one, page 3 10788, page 4 12892. 16384 carries that through the page cap
 * (list_screen.c ITEMS_MAX) with a margin; the drop stays visible
 * (yt_parse_last_dropped_continuation_len) should it ever not be enough. The
 * other token in each response, reloadContinuationData, is never a next page
 * and is ignored by key, not by size. Every list carries one of these buffers
 * (all static, or inside one fetch task on the main thread's stack). */
#define YT_CONTINUATION_MAX 16384
/* The visitor id a continuation has to be sent with: the visitorData string
 * of the response that issued the token (~445 characters seen, base64url).
 * See yt_video_list_t.visitor. */
#define YT_VISITOR_MAX 768
/* Rows one response can hand over. The anonymous home feed delivers 66 to 84
 * video renderers in a single page (randomised per call; 76 in the fixture);
 * with a cap of 60 the tail of every page was silently dropped, and since the
 * page's continuation token starts after it, paging could never bring those
 * rows back. 128 leaves room above the largest page seen. The cap also sizes
 * the channel lists (yt_channel_list_t, and ChannelListState in
 * list_screen.c). */
#define YT_LIST_MAX 128
#define YT_FORMATS_MAX 4         /* progressive formats rarely exceed 1-2 today */

typedef struct {
    char video_id[YT_VIDEO_ID_LEN];
    char title[YT_TITLE_MAX];
    char channel[YT_CHANNEL_MAX];
    /* Owning channel, when the response carried one. Empty is normal for
     * surfaces that omit the byline endpoint; match on this rather than on
     * the display name, whose spelling differs between surfaces. */
    char channel_id[YT_CHANNEL_ID_LEN];
    char views[YT_META_MAX];      /* human-readable, e.g. "1,2 Mio. Aufrufe" */
    char duration[YT_META_MAX];   /* e.g. "12:34" */
    char thumb_url[YT_THUMB_URL_MAX];
    /* Minutes since publication, derived from YouTube's localized relative
     * age text ("vor 3 Tagen") because the API sends no timestamp for list
     * items; -1 when no age was recognisable. Coarse by nature ("vor 1 Monat"
     * covers a whole month) — only ever use it as a *stable* sort key. */
    int age_min;
} yt_video_t;

/* Which request produced a page — and therefore where its continuation token
 * has to be sent back. A token is only good at the request that issued it: a
 * browse token posted to "search" is an HTTP 400 or an empty page, and the old
 * yt_continue() did exactly that for everything because it hardcoded search +
 * ANDROID. Lists store the surface next to the token (and, where one is
 * needed, the visitor id) and hand them to yt_continue(); innertube.c is the
 * only place that maps a surface to a request (one table, both directions).
 *
 * A surface is "pageable" when yt_continue() has a route for it. Only the
 * ones verified against the live API are; a page from any other request is
 * stamped NONE so no list ever offers a "load more" that cannot work.
 *
 * NONE must stay 0: a zeroed or memset list means "no surface" and the UI
 * relies on that after a reset. */
typedef enum {
    YT_SURF_NONE = 0,
    /* Pageable: */
    YT_SURF_SEARCH,         /* search, ANDROID, anonymous */
    YT_SURF_BROWSE_WEB,     /* browse, VISIONOS on www.youtube.com, anonymous,
                             * AND the visitor id the token was issued under:
                             * the signed-out Start feed */
    YT_SURF_CHANNEL_TV,     /* browse, TV, anonymous, no visitor: a channel's
                             * uploads (a VLUU... playlist) */
    /* Reserved, NOT pageable yet: these need a signed-in capture to confirm
     * where the feed's own continuation sits (in the Abos feed the first
     * nextContinuationData in document order continues one shelf, not the
     * feed). Pages from them are stamped NONE; adding a row to the route
     * table in innertube.c is all it takes once they are verified. */
    YT_SURF_BROWSE_TV,      /* browse, TV, with the OAuth token: Start/Abos signed in */
    YT_SURF_BROWSE_ANDROID  /* browse, ANDROID, with the OAuth token: Abos fallback */
} yt_surface_t;

typedef struct {
    yt_video_t items[YT_LIST_MAX];
    int count;
    char continuation[YT_CONTINUATION_MAX]; /* empty string if none */
    /* The request that produced this page, i.e. where `continuation` goes
     * back to; only meaningful together with a non-empty token. YT_SURF_NONE
     * when the request has no (verified) continuation route. Set by
     * innertube.c after a successful parse — the parser itself never knows
     * the endpoint and leaves it NONE. */
    yt_surface_t surface;
    /* The visitor id that goes with `continuation`, for the surfaces whose
     * tokens are bound to one (YT_SURF_BROWSE_WEB): YouTube answers a token
     * sent with a different id, or none, with a 200 and an empty shell — no
     * error, just zero rows. It is the visitorData of the page-1 response, and
     * every later page carries the same one forward (a continuation response
     * is never allowed to replace it), so the id is stable for the whole
     * list however many other requests the app makes meanwhile — the global
     * one in innertube.c is rewritten by every unfiltered response, e.g. a
     * player call. Empty for surfaces that need none. */
    char visitor[YT_VISITOR_MAX];
} yt_video_list_t;

typedef struct {
    char channel_id[YT_CHANNEL_ID_LEN];
    char title[YT_CHANNEL_MAX];
    char avatar_url[YT_THUMB_URL_MAX];
} yt_channel_t;

typedef struct {
    yt_channel_t items[YT_LIST_MAX];
    int count;
    char continuation[YT_CONTINUATION_MAX];
} yt_channel_list_t;

/* A single progressive (muxed video+audio) format. YouTube only serves
 * progressive streams up to 360p as of 2026 (verified live); this struct
 * stays quality-agnostic so the app automatically benefits if that ever
 * changes for a given video. */
typedef struct {
    int itag;
    int width;
    int height;
    int fps;
    long long content_length;  /* 0 if unknown */
    char url[YT_URL_MAX];
} yt_format_t;

typedef struct {
    char video_id[YT_VIDEO_ID_LEN];
    char title[YT_TITLE_MAX];
    char channel[YT_CHANNEL_MAX];
    yt_format_t formats[YT_FORMATS_MAX]; /* sorted by height, descending */
    int format_count;
    int selected;              /* index into formats[]; default = highest */
    int length_seconds;        /* 0 for a live stream: it has no length yet */
    /* A stream that is on the air right now. YouTube publishes no progressive
     * format for one, so this is also what says "HLS or nothing" — see
     * hls_manifest below and player_screen.c. */
    int is_live;
    /* streamingData.hlsManifestUrl, kept only for a live stream: its variants
     * are muxed and its playlist is a short moving window, which is exactly
     * what live needs and NOT what the on-demand path wants (that resolves
     * its own demuxed manifest through yt_hls_manifest). Empty otherwise. */
    char hls_manifest[YT_URL_MAX];
    char playability[48];      /* "OK" or error status */
    char reason[160];          /* human-readable error if not OK */
} yt_stream_t;

#endif
