/* ViTube — InnerTube API (YouTube Android client). */
#include "yt/innertube.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "log.h"
#include "net/http.h"
#include "yt/oauth.h"
#include "yt/parse.h"
#include "yt/region.h"
#include "ui/strings.h"

#define IT_BASE "https://youtubei.googleapis.com/youtubei/v1/"
/* The VISIONOS/HLS resolve goes to www.youtube.com instead, with an Origin
 * header — byte-for-byte what yt-dlp does, and that path stays out of the
 * LOGIN_REQUIRED reputation gate far more reliably than the bare googleapis
 * host does (measured 2026-08-28: googleapis flips to LOGIN_REQUIRED under
 * load where this one keeps answering). */
#define IT_WEB_BASE "https://www.youtube.com/youtubei/v1/"
#define IT_CLIENT_NAME "ANDROID"
#define IT_CLIENT_VERSION "21.26.364"
#define IT_CLIENT_NAME_ID "3"
#define IT_UA "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip"

/* The TVHTML5 context matches the OAuth token's origin (YouTube on TV) —
 * personalized browse rejects the android context with HTTP 400. */
#define IT_TV_VERSION "7.20250120.19.00"
#define IT_TV_UA "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version"

/* VISIONOS is the client that hands out an HLS manifest, which is the only
 * way to 720p that this app can serve on its own: the playlist URIs are
 * already signed (no `n` parameter, so no JavaScript challenge to solve) and
 * the segments are plain MPEG-TS/H.264+AAC. It also needs no player JS at
 * all. Measured 2026-08-28. */
#define IT_VISION_VERSION "1.02"
#define IT_VISION_UA \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 " \
    "(KHTML, like Gecko) Version/26.0 Safari/605.1.15"

typedef enum {
    IT_CLIENT_ANDROID = 0,
    IT_CLIENT_TV,
    IT_CLIENT_VISIONOS
} it_client_t;

/* fields filter: trims list responses from ~2 MB to a few hundred KB.
 * ONLY fields that exist on ALL of search/next/browse — a single unknown
 * name makes the API reject the whole request with HTTP 400
 * ("Error expanding 'fields' parameter"). Verified live 2026-08-27. */
#define IT_LIST_FIELDS "contents,continuationContents"

static char g_last_error[96];

/* YouTube answers a player request from an unknown visitor with
 * LOGIN_REQUIRED — even from a residential IP — but every InnerTube response
 * carries a fresh visitorData in its responseContext. Echoing that back as
 * X-Goog-Visitor-Id on the next call is what turns LOGIN_REQUIRED into OK
 * (measured 2026-08-28). One 860-byte rejection per app start is enough to
 * seed it; from then on it rides along on every call. */
static char g_visitor_data[768];
static char g_visitor_header[832];

/* Cheap scan instead of parsing what can be a two-megabyte response: the
 * field sits in responseContext near the top and its value is plain
 * base64url with no escapes to unpick. */
static void harvest_visitor_data(const char *body) {
    if (!body) return;
    static const char VD_KEY[] = "\"visitorData\":\"";
    const char *key = strstr(body, VD_KEY);
    if (!key) return;
    /* sizeof-1 = the literal's length, so this can never drift off by a
     * character the way a hand-counted offset did (it was 16 for a 15-char
     * key, which lopped the first character off every visitor id and got
     * every VISIONOS request answered with LOGIN_REQUIRED). */
    const char *value = key + (sizeof VD_KEY - 1);
    const char *end = strchr(value, '"');
    if (!end || end == value) return;
    size_t len = (size_t)(end - value);
    if (len >= sizeof g_visitor_data) return;
    if (memcmp(g_visitor_data, value, len) == 0 && g_visitor_data[len] == '\0')
        return; /* unchanged */
    memcpy(g_visitor_data, value, len);
    g_visitor_data[len] = '\0';
    snprintf(g_visitor_header, sizeof g_visitor_header,
             "X-Goog-Visitor-Id: %s", g_visitor_data);
    vt_log("api: visitor id acquired (%u chars)", (unsigned)len);
}

const char *yt_last_error(void) {
    return g_last_error;
}

static unsigned out_len(const http_buf_t *buf) {
    return buf ? (unsigned)buf->len : 0u;
}

static json_t *client_context(it_client_t client) {
    if (client == IT_CLIENT_VISIONOS)
        /* Field-for-field what yt-dlp sends for this client (userAgent inside
         * the context, timeZone/utcOffsetMinutes present): a bare context is
         * likelier to be waved into LOGIN_REQUIRED when this IP has been busy,
         * a complete one passes. */
        return json_pack("{s:{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:i}}",
            "client",
            "clientName", "VISIONOS",
            "clientVersion", IT_VISION_VERSION,
            "deviceMake", "Apple",
            "deviceModel", "RealityDevice17,1",
            "userAgent", IT_VISION_UA,
            "osName", "visionOS",
            "osVersion", "26.5.23O471",
            "hl", yt_region_hl(),
            "gl", yt_region_gl(),
            "timeZone", "UTC",
            "utcOffsetMinutes", 0);
    if (client == IT_CLIENT_TV)
        return json_pack("{s:{s:s,s:s,s:s,s:s}}",
            "client",
            "clientName", "TVHTML5",
            "clientVersion", IT_TV_VERSION,
            "hl", yt_region_hl(),
            "gl", yt_region_gl());
    return json_pack("{s:{s:s,s:s,s:i,s:s,s:s,s:s,s:s,s:s}}",
        "client",
        "clientName", IT_CLIENT_NAME,
        "clientVersion", IT_CLIENT_VERSION,
        "androidSdkVersion", 30,
        "userAgent", IT_UA,
        "osName", "Android",
        "osVersion", "11",
        "hl", yt_region_hl(),
        "gl", yt_region_gl());
}

/* POST an InnerTube endpoint. body_extra is adopted (reference stolen).
 * with_auth attaches the OAuth token — only for personalized endpoints;
 * anonymous endpoints stay anonymous so playback never depends on a
 * possibly-stale token. */
static int it_call(const char *endpoint, const char *fields,
                   json_t *body_extra, int with_auth, it_client_t client,
                   volatile int *cancel, http_buf_t *out) {
    json_t *body = body_extra ? body_extra : json_object();
    json_object_set_new(body, "context", client_context(client));
    char *payload = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    if (!payload) return -1;

    const char *base = client == IT_CLIENT_VISIONOS ? IT_WEB_BASE : IT_BASE;
    char url[512];
    if (fields && fields[0]) {
        char *escaped = http_escape(fields);
        snprintf(url, sizeof url, "%s%s?prettyPrint=false&fields=%s",
                 base, endpoint, escaped ? escaped : "");
        free(escaped);
    } else {
        snprintf(url, sizeof url, "%s%s?prettyPrint=false", base, endpoint);
    }

    const char *headers[8];
    int n = 0;
    if (client == IT_CLIENT_TV) {
        headers[n++] = "X-Youtube-Client-Name: 7";
        headers[n++] = "X-Youtube-Client-Version: " IT_TV_VERSION;
        headers[n++] = "User-Agent: " IT_TV_UA;
    } else if (client == IT_CLIENT_VISIONOS) {
        headers[n++] = "X-Youtube-Client-Name: 101";
        headers[n++] = "X-Youtube-Client-Version: " IT_VISION_VERSION;
        headers[n++] = "User-Agent: " IT_VISION_UA;
        headers[n++] = "Origin: https://www.youtube.com";
        if (g_visitor_header[0]) headers[n++] = g_visitor_header;
    } else {
        headers[n++] = "X-Youtube-Client-Name: " IT_CLIENT_NAME_ID;
        headers[n++] = "X-Youtube-Client-Version: " IT_CLIENT_VERSION;
    }
    const char *auth = NULL;
    if (with_auth && yt_oauth_logged_in()) {
        auth = yt_oauth_authorization(cancel);
        /* Still signed in but no usable token: the refresh failed (or was
         * cancelled). Sending the personalised request anonymously instead
         * answered with an empty shell that the caller then presented as
         * "no subscriptions" under a signed-in top bar; a network error is
         * what happened, so report that. */
        if (!auth && yt_oauth_logged_in()) {
            vt_log("api %s: no access token, not sending anonymously",
                   endpoint);
            snprintf(g_last_error, sizeof g_last_error, "%s",
                     vt_str(S_ERR_AUTH_REFRESH));
            free(payload);
            return -1;
        }
    }
    if (auth) headers[n++] = auth;
    headers[n] = NULL;

    long status = 0;
    int ret = http_post(url, "application/json", payload, strlen(payload),
                        headers, cancel, out, &status);
    free(payload);
    if (ret < 0) {
        vt_log("api %s: transport error %d", endpoint, ret);
        snprintf(g_last_error, sizeof g_last_error, vt_str(S_ERR_TRANSPORT),
                 ret);
        return ret;
    }
    if (status != 200) {
        char head[241] = {0};
        if (out && out->data) {
            snprintf(head, sizeof head, "%s", out->data);
            for (char *c = head; *c; c++)
                if (*c == '\n' || *c == '\r') *c = ' ';
        }
        vt_log("api %s: HTTP %ld %s", endpoint, status, head);
        snprintf(g_last_error, sizeof g_last_error, vt_str(S_ERR_HTTP), status);
        return -1;
    }
    vt_log("api %s: HTTP 200, %u bytes", endpoint,
           out ? (unsigned)out->len : 0u);
    if (out) harvest_visitor_data(out->data);
    g_last_error[0] = '\0';
    return 0;
}

static int it_list_call(const char *endpoint, json_t *body, int with_auth,
                        it_client_t client, volatile int *cancel,
                        yt_video_list_t *out) {
    http_buf_t buf = {0};
    int ret = it_call(endpoint, IT_LIST_FIELDS, body, with_auth, client,
                      cancel, &buf);
    if (ret == 0) {
        int count = yt_parse_video_list(buf.data, buf.len, out);
        vt_log("parse %s: %d videos, continuation=%d", endpoint, count,
               out->continuation[0] != '\0');
        if (count == 0)
            snprintf(g_last_error, sizeof g_last_error,
                     vt_str(S_ERR_EMPTY_RESPONSE), (unsigned)buf.len);
        ret = count >= 0 ? 0 : -1;
    }
    http_buf_free(&buf);
    return ret;
}

int yt_search(const char *query, volatile int *cancel, yt_video_list_t *out,
              yt_channel_list_t *out_channels) {
    /* Not it_list_call(): the search response carries channel results next to
     * the videos and both are pulled out of one parsed tree. IT_LIST_FIELDS
     * keeps them — every channel item lives under
     * contents.sectionListRenderer.contents[...] (verified live 2026-08-28).
     * It does strip frameworkUpdates, i.e. the live subscribed flag; that is
     * intentional. This call is anonymous, so the flag would read false for
     * every channel anyway, and asking for frameworkUpdates in the shared
     * fields filter inflates /next by ~1.5 MB. Whether the user subscribes to
     * a result is answered from the FEchannels list instead. */
    http_buf_t buf = {0};
    int ret = it_call("search", IT_LIST_FIELDS,
                      json_pack("{s:s}", "query", query), 0,
                      IT_CLIENT_ANDROID, cancel, &buf);
    if (ret == 0) {
        int count = yt_parse_search(buf.data, buf.len, out, out_channels);
        int channels = out_channels ? out_channels->count : 0;
        vt_log("parse search: %d videos, %d channels, continuation=%d", count,
               channels, out->continuation[0] != '\0');
        if (count == 0 && channels == 0)
            snprintf(g_last_error, sizeof g_last_error,
                     vt_str(S_ERR_EMPTY_RESPONSE), (unsigned)buf.len);
        ret = count >= 0 ? 0 : -1;
    }
    http_buf_free(&buf);
    return ret;
}

int yt_continue(const char *continuation, volatile int *cancel,
                yt_video_list_t *out) {
    return it_list_call("search",
                        json_pack("{s:s}", "continuation", continuation), 0,
                        IT_CLIENT_ANDROID, cancel, out);
}

int yt_related(const char *video_id, volatile int *cancel,
               yt_video_list_t *out) {
    return it_list_call("next", json_pack("{s:s}", "videoId", video_id), 0,
                        IT_CLIENT_ANDROID, cancel, out);
}

int yt_home_feed(volatile int *cancel, yt_video_list_t *out) {
    /* Two different feeds behind one call.
     *
     * Signed in, the TV client with the OAuth token returns the account's own
     * recommendations. Signed out it returns nothing usable: an anonymous
     * TVHTML5 browse answers with a ~2 KB shell holding a search bar and no
     * videos, and the android client answers with the consent/onboarding flow
     * instead of a feed — measured 2026-08-30, identical for gl=DE and gl=US
     * and unaffected by the usual consent cookies.
     *
     * VISIONOS is the one client that serves a real anonymous feed: ~75
     * videos as compactVideoRenderer, which the parser already handles. It
     * works for the same reason the HLS resolve does — www.youtube.com with
     * an Origin header and the harvested visitor id, rather than the bare
     * googleapis host that flips to LOGIN_REQUIRED. Its continuation token
     * is over 4 KB and does not fit YT_CONTINUATION_MAX, so the parser drops
     * it (see set_continuation) and this stays one page of ~60 rows — which
     * is what the Start tab shows anyway, since only search loads more.
     *
     * The anonymous call doubles as the fallback for a signed-in session
     * whose TV browse comes back empty, which is what a revoked or
     * reputation-gated token looks like from here. */
    if (yt_oauth_logged_in()) {
        int ret = it_list_call("browse",
                               json_pack("{s:s}", "browseId", "FEwhat_to_watch"),
                               1, IT_CLIENT_TV, cancel, out);
        if (ret == 0 && out->count > 0) return 0;
        if (cancel && *cancel) return ret;
        vt_log("home: tv feed empty (ret=%d), retrying anonymously", ret);
    }
    return it_list_call("browse",
                        json_pack("{s:s}", "browseId", "FEwhat_to_watch"), 0,
                        IT_CLIENT_VISIONOS, cancel, out);
}

int yt_subscriptions_feed(volatile int *cancel, yt_video_list_t *out) {
    if (!yt_oauth_logged_in()) return -2;
    /* The TV context matches the OAuth token; the android context is kept
     * as a fallback in case the TV path regresses. */
    int ret = it_list_call("browse",
                           json_pack("{s:s}", "browseId", "FEsubscriptions"),
                           1, IT_CLIENT_TV, cancel, out);
    /* Sorted explicitly: the stable sort is a no-op when YouTube already
     * returns the feed newest-first, and a real fix when the TV surface
     * groups it by channel or by shelf instead. */
    if (ret == 0 && out->count > 0) {
        yt_sort_newest_first(out);
        return 0;
    }
    vt_log("subs via tv client failed, trying android");
    ret = it_list_call("browse",
                       json_pack("{s:s}", "browseId", "FEsubscriptions"), 1,
                       IT_CLIENT_ANDROID, cancel, out);
    if (ret == 0) yt_sort_newest_first(out);
    return ret;
}

int yt_channel_list(volatile int *cancel, yt_channel_list_t *out) {
    if (!yt_oauth_logged_in()) return -2;
    http_buf_t buf = {0};
    int ret = it_call("browse", IT_LIST_FIELDS,
                      json_pack("{s:s}", "browseId", "FEchannels"), 1,
                      IT_CLIENT_TV, cancel, &buf);
    if (ret == 0) {
        int count = yt_parse_channel_list(buf.data, buf.len, out);
        /* Avatar count included so a blank-square report can be told apart
         * without a rebuild: "N channels, 0 with avatar" means extraction
         * broke, "N with avatar" plus blank squares means fetch or decode. */
        int with_avatar = 0;
        for (int i = 0; i < count; i++)
            if (out->items[i].avatar_url[0]) with_avatar++;
        vt_log("parse FEchannels: %d channels, %d with avatar", count,
               with_avatar);
        if (count == 0)
            snprintf(g_last_error, sizeof g_last_error,
                     vt_str(S_ERR_EMPTY_RESPONSE), (unsigned)buf.len);
        ret = count >= 0 ? 0 : -1;
    }
    http_buf_free(&buf);
    return ret;
}

int yt_channel_videos(const char *channel_id, volatile int *cancel,
                      yt_video_list_t *out, char *channel_title,
                      size_t channel_title_cap) {
    /* Browse the channel's *uploads playlist* ("UU" + the channel id's
     * suffix), not the channel itself. browseId=UC... returns the curated
     * channel home surface — featured playlists, "Beliebte Videos" (ordered
     * by view count), Shorts, past live streams — in editorial order, and
     * TVHTML5 ignores the Videos-tab "params" entirely (verified live
     * 2026-08-28: byte-for-byte the same 12-shelf response with and without
     * them). Collecting tiles in document order therefore produced the mix of
     * old and new the user reported, and the YT_LIST_MAX cap was reached
     * before the uploads shelf was ever visited. The uploads playlist is
     * strictly newest-first. */
    if (strncmp(channel_id, "UC", 2) != 0) return -1;
    char browse_id[YT_CHANNEL_ID_LEN + 4];
    snprintf(browse_id, sizeof browse_id, "VLUU%s", channel_id + 2);
    http_buf_t buf = {0};
    json_t *body = json_pack("{s:s}", "browseId", browse_id);
    int ret = it_call("browse", IT_LIST_FIELDS, body, 0, IT_CLIENT_TV, cancel,
                      &buf);
    if (ret == 0) {
        /* A playlist surface carries no channel header, so channel_title
         * stays empty and list_screen.c keeps the name it already has from
         * the subscriptions list. */
        int count = yt_parse_channel_page(buf.data, buf.len, out,
                                          channel_title, channel_title_cap);
        yt_sort_newest_first(out);
        vt_log("parse channel uploads: %d videos", count);
        ret = count >= 0 ? 0 : -1;
    }
    http_buf_free(&buf);
    return ret;
}

/* {"actions":[{"updateSubscribeButtonAction":{"subscribed":true,
 *              "channelId":"UC..."}}], ...} — the new state as YouTube sees
 * it. Returns 1/0, or -1 when the reply carried no such action. A reply
 * without it is NOT a failure: both reference clients ignore this body
 * entirely and treat HTTP 200 as the success signal. */
static int subscribe_echoed_state(const char *json, size_t len) {
    if (!json || !len) return -1;
    json_error_t err;
    json_t *root = json_loadb(json, len, 0, &err);
    if (!root) return -1;
    int state = -1;
    json_t *actions = json_object_get(root, "actions");
    size_t i;
    json_t *action;
    json_array_foreach(actions, i, action) {
        json_t *upd = json_object_get(action, "updateSubscribeButtonAction");
        if (upd) {
            state = json_is_true(json_object_get(upd, "subscribed")) ? 1 : 0;
            break;
        }
    }
    json_decref(root);
    return state;
}

/* Write to the account: subscribe to / unsubscribe from one channel.
 *
 * InnerTube accepts the plain OAuth Bearer token this app already holds for
 * these endpoints — no browser cookies and no SAPISIDHASH signature.
 * Verified against two independent clients built on the same "YouTube on TV"
 * device flow: SmartTube/MediaServiceCore ships the byte-identical client id
 * and secret from oauth.c and its captured request is Bearer + a TVHTML5
 * context + {"channelIds":[...]}; YouTube.js sets "Authorization: Bearer"
 * for every InnerTube request and offers SAPISIDHASH only as the cookie
 * alternative.
 *
 * The TV context is mandatory, not a preference — the android context has no
 * valid session for this token (see the note at the top of this file).
 *
 * "params" carries notification defaults and is channel-independent: these
 * two constants decode to six protobuf flag bytes with no channel id inside,
 * and YouTube.js sends exactly them for every channel. */
int yt_subscribe(const char *channel_id, int subscribe, volatile int *cancel) {
    if (!yt_oauth_logged_in()) return -2;
    if (strncmp(channel_id, "UC", 2) != 0) return -1;
    json_t *body = json_pack("{s:[s],s:s}",
                             "channelIds", channel_id,
                             "params", subscribe ? "EgIIAhgA" : "CgIIAhgA");
    http_buf_t buf = {0};
    /* fields=NULL: IT_LIST_FIELDS names contents/continuationContents, and a
     * fields name that does not exist on the response makes YouTube reject
     * the whole request with HTTP 400. */
    int ret = it_call(subscribe ? "subscription/subscribe"
                                : "subscription/unsubscribe",
                      NULL, body, 1, IT_CLIENT_TV, cancel, &buf);
    if (ret == 0)
        vt_log("subscribe %s %s: echoed state %d", channel_id,
               subscribe ? "on" : "off",
               subscribe_echoed_state(buf.data, buf.len));
    http_buf_free(&buf);
    return ret;
}

int yt_hls_manifest(const char *video_id, volatile int *cancel,
                    char *url_out, size_t url_cap) {
    if (!video_id || !url_out || url_cap == 0) return -1;
    url_out[0] = '\0';

    /* Retry exists only to seed the visitor id, and only when we start
     * without one: the very first player request a fresh process makes comes
     * back LOGIN_REQUIRED but carries the visitorData that it_call() latches
     * onto, so a second request with it attached goes through. Once a visitor
     * id already exists — and one usually does, since the direct-stream
     * resolve runs first and sets it — a LOGIN_REQUIRED is the datacenter/
     * reputation gate answering, not a missing id, and firing an identical
     * second request only pushes this IP further into that gate (measured
     * 2026-08-28: bursts of player calls flip VISIONOS to LOGIN_REQUIRED for
     * ~tens of seconds; a single call per video rides under it). So attempt
     * twice only when seeding, once otherwise. */
    int had_visitor = g_visitor_header[0] != 0;
    int attempts = had_visitor ? 1 : 2;
    for (int attempt = 0; attempt < attempts; attempt++) {
        /* playbackContext mirrors what yt-dlp sends for this client. */
        json_t *body = json_pack(
            "{s:s,s:b,s:b,s:{s:{s:s}}}",
            "videoId", video_id,
            "contentCheckOk", 1,
            "racyCheckOk", 1,
            "playbackContext", "contentPlaybackContext",
            "html5Preference", "HTML5_PREF_WANTS");
        http_buf_t buf = {0};
        int ret = it_call("player", NULL, body, 0, IT_CLIENT_VISIONOS, cancel,
                          &buf);
        if (ret < 0) {
            http_buf_free(&buf);
            return ret;
        }
        json_error_t err;
        json_t *root = buf.data ? json_loads(buf.data, 0, &err) : NULL;
        const char *manifest = json_string_value(json_object_get(
            json_object_get(root, "streamingData"), "hlsManifestUrl"));
        const char *status = json_string_value(json_object_get(
            json_object_get(root, "playabilityStatus"), "status"));
        if (manifest && manifest[0]) {
            snprintf(url_out, url_cap, "%s", manifest);
            vt_log("hls %s: manifest ready (attempt %d/%d)", video_id,
                   attempt + 1, attempts);
        } else {
            vt_log("hls %s: no manifest, playability=%s (attempt %d/%d, "
                   "resp=%u B)", video_id, status ? status : "?", attempt + 1,
                   attempts, out_len(&buf));
            /* YouTube's own words first ("Dieses Video ist privat", "Sign
             * in to confirm your age"), the status enum only as a fallback:
             * the enum is what the viewer used to see. */
            const char *why = json_string_value(json_object_get(
                json_object_get(root, "playabilityStatus"), "reason"));
            if (why && why[0])
                snprintf(g_last_error, sizeof g_last_error, "%s", why);
            else
                snprintf(g_last_error, sizeof g_last_error, "HLS: %s",
                         status ? status : vt_str(S_ERR_NO_STREAM_DATA));
        }
        if (root) json_decref(root);
        http_buf_free(&buf);
        if (url_out[0]) return 0;
        /* Only a first, seeding attempt is worth repeating; if it still did
         * not produce a visitor id, there is nothing new to send. */
        if (!g_visitor_header[0]) break;
    }
    return -1;
}

int yt_resolve_stream(const char *video_id, volatile int *cancel,
                      yt_stream_t *out) {
    json_t *body = json_pack("{s:s,s:b,s:b}",
                             "videoId", video_id,
                             "contentCheckOk", 1,
                             "racyCheckOk", 1);
    http_buf_t buf = {0};
    int ret = it_call("player", NULL, body, 0, IT_CLIENT_ANDROID, cancel,
                      &buf);
    if (ret == 0) ret = yt_parse_player(buf.data, buf.len, out);
    http_buf_free(&buf);
    return ret;
}
