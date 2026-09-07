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
#define YT_CONTINUATION_MAX 2048
#define YT_LIST_MAX 60
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

typedef struct {
    yt_video_t items[YT_LIST_MAX];
    int count;
    char continuation[YT_CONTINUATION_MAX]; /* empty string if none */
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
