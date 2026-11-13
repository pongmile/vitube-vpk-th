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

/* The anonymous VISIONOS browse (Start, signed out), page 1 AND every
 * continuation: the plain filter plus the one field that carries the visitor
 * id. Without it the id stays unknown to the app — the filter strips
 * responseContext — and a continuation needs the visitor id its token was
 * issued under (see YT_VISITOR_MAX for what "the id" means: not the exact
 * string). Verified live: a "responseContext/
 * visitorData" sub-path is accepted on this endpoint and the value comes back
 * about 445 characters long near the start of the body; the filtered body is
 * ~244 KB where an unfiltered one is ~649 KB, so the filter stays. This is
 * deliberately NOT part of IT_LIST_FIELDS: it is untested on search, next and
 * the TV browse, and one unknown name there is an HTTP 400. */
#define IT_WEB_BROWSE_FIELDS "contents,continuationContents,responseContext/visitorData"

/* Where a page's continuation token goes back to, in one table.
 *
 * A continuation is only valid at the request that issued it — which is also
 * why yt_continue() used to answer nothing but search results: it sent EVERY
 * token to "search" as the ANDROID client, so a browse token (Start, a
 * channel's videos) was a 400 or an empty page and only the search list ever
 * paged. A list now remembers the surface its token came from
 * (yt_video_list_t.surface) and yt_continue() looks the request up here.
 *
 * The stamp goes the other way, but is NOT inferred from the request. It used
 * to be (a lookup of the request's endpoint, client and auth in this table),
 * and that is a trap: the day someone adds an anonymous TV browse for some
 * other purpose, it would match the channel-uploads row, and its pages would
 * be stamped, and offered to the user, as a route nobody verified for them.
 * Every call site now NAMES the surface of the page it fetches (YT_SURF_NONE
 * for the ones with no route), and stamp_surface() below only checks that the
 * request really made is the row of the surface it was given. A new call that
 * names nothing does not compile; one that names YT_SURF_NONE changes nothing
 * about paging; one that names a surface for a request that is not its row is
 * stamped NONE and logged.
 *
 * `fields` is what the CONTINUATION request asks for (page 1 of the same
 * surface uses the same, see the call sites). `needs_visitor`: the token is
 * bound to the visitor id it was issued under (not to the exact string, see
 * YT_VISITOR_MAX), so the page carries the id of the response that issued it
 * (yt_video_list_t.visitor) and the continuation sends that.
 *
 * Only surfaces verified against the live API have a row. Pages from every
 * other request get YT_SURF_NONE, so no list ever offers a load-more that
 * cannot work:
 *   - signed-in Start (TV, OAuth) and the Abos feed (TV with an ANDROID
 *     fallback, OAuth): both need a login to capture, and in the Abos feed
 *     the first nextContinuationData in document order continues one SHELF,
 *     not the feed. When they are verified, add rows here (and make
 *     list_screen.c sort each new Abos page with yt_sort_newest_first before
 *     absorbing it — a continuation is not sorted by yt_subscriptions_feed);
 *   - related videos ("next"): nothing asks for a second page. */
typedef struct {
    yt_surface_t surface;
    const char *endpoint;
    it_client_t client;
    int with_auth;
    int needs_visitor;
    const char *fields;
} it_route_t;

static const it_route_t g_routes[] = {
    /* Unchanged from what yt_continue() always did for search. */
    { YT_SURF_SEARCH,     "search", IT_CLIENT_ANDROID,  0, 0, IT_LIST_FIELDS },
    /* The signed-out Start feed (see yt_home_feed). Anonymous on purpose:
     * this client is the one that works without an account. A token sent
     * without the issuing visitor id, or with another, is answered with HTTP
     * 200 and an empty shell (~4 KB, zero rows) — no error to react to. */
    { YT_SURF_BROWSE_WEB, "browse", IT_CLIENT_VISIONOS, 0, 1,
      IT_WEB_BROWSE_FIELDS },
    /* A channel's uploads (yt_channel_videos): the TV client, anonymous, no
     * visitor id. Verified live: the 124-character token of page 1 returns
     * the next 15 tiles as continuationContents.playlistVideoListContinuation
     * with exactly this request. */
    { YT_SURF_CHANNEL_TV, "browse", IT_CLIENT_TV,       0, 0, IT_LIST_FIELDS },
};

static const it_route_t *route_for_surface(yt_surface_t surface) {
    for (size_t i = 0; i < sizeof g_routes / sizeof *g_routes; i++)
        if (g_routes[i].surface == surface) return &g_routes[i];
    return NULL; /* YT_SURF_NONE, the reserved TV/ANDROID ones, anything else */
}

/* The surface a page is stamped with: the one its call site NAMED, provided
 * the request that was actually made (endpoint, client, with_auth) is that
 * surface's row in the table; YT_SURF_NONE otherwise. A page stamped NONE is
 * never paged (video_list_absorb drops its token), so everything doubtful ends
 * here: no row for the surface (YT_SURF_NONE itself, the reserved TV/ANDROID
 * ones), or a row for a different request, in which case the token would be
 * sent back somewhere that did not issue it. */
static yt_surface_t stamp_surface(yt_surface_t surface, const char *endpoint,
                                  it_client_t client, int with_auth) {
    if (surface == YT_SURF_NONE) return YT_SURF_NONE;
    const it_route_t *r = route_for_surface(surface);
    if (!r) return YT_SURF_NONE;
    if (r->client == client && (r->with_auth != 0) == (with_auth != 0) &&
        strcmp(r->endpoint, endpoint) == 0)
        return surface;
    vt_log("surface %d named for a %s request (client %d, auth %d) that is not "
           "its route, not pageable", (int)surface, endpoint, (int)client,
           with_auth != 0);
    return YT_SURF_NONE;
}

static char g_last_error[96];

/* YouTube answers a player request from an unknown visitor with
 * LOGIN_REQUIRED — even from a residential IP — but every InnerTube response
 * carries a fresh visitorData in its responseContext. Echoing that back as
 * X-Goog-Visitor-Id on the next call is what turns LOGIN_REQUIRED into OK
 * (measured 2026-08-28). One 860-byte rejection per app start is enough to
 * seed it; from then on it rides along on every call. */
static char g_visitor_data[768];
static char g_visitor_header[832];

/* Cheap scan instead of parsing what can be a two-megabyte response; the scan
 * itself (yt_parse_visitor_data) is shared with the list calls and covered by
 * the host tests.
 *
 * The scratch the id is read into is on the heap, not in this frame. It is
 * 768 bytes and this runs at the end of EVERY request, on whichever thread made
 * it: for the list loads that is the attempt thread of ui/net_task.c, created
 * with pthread_create and no stack size (the download and HLS workers, which
 * also run curl, ask for 256 KiB explicitly), and the frames below it on that
 * stack are http_post -> perform (a VitaHttpsResponse with a 2 KiB
 * effective_url, a 128-byte header line) -> curl -> Mbed TLS. What that
 * default is, and how deep the TLS handshake goes, cannot be read from this
 * tree, so the 1.6 KB this and the header copy in it_call() would otherwise
 * add is not spent on a stack of unknown size. If the allocation fails the
 * scan is skipped: the id is a latch that the next response sets again. */
static void harvest_visitor_data(const char *body) {
    if (!body) return;
    char *value = malloc(sizeof g_visitor_data);
    if (!value) return;
    size_t len = (size_t)yt_parse_visitor_data(body, value, sizeof g_visitor_data);
    if (len && !(memcmp(g_visitor_data, value, len) == 0 &&
                 g_visitor_data[len] == '\0')) {
        memcpy(g_visitor_data, value, len);
        g_visitor_data[len] = '\0';
        snprintf(g_visitor_header, sizeof g_visitor_header,
                 "X-Goog-Visitor-Id: %s", g_visitor_data);
        vt_log("api: visitor id acquired (%u chars)", (unsigned)len);
    }
    free(value);
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
 * possibly-stale token. visitor (VISIONOS only): NULL sends the app's latched
 * id; a string sends exactly that one as X-Goog-Visitor-Id instead — what a
 * continuation does, since its token is bound to the id it was issued under. */
static int it_call(const char *endpoint, const char *fields,
                   json_t *body_extra, int with_auth, it_client_t client,
                   const char *visitor, volatile int *cancel, http_buf_t *out) {
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

    /* The OAuth header first: when it cannot be had there is nothing left to
     * allocate or free below. */
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

    /* A copy, not a pointer to g_visitor_header: any other thread's response
     * (a player call, say) may rewrite the static while this request is being
     * put together, and a header torn in half is a request for a visitor
     * nobody issued. On the heap, not in this frame: the copy is 832 bytes on
     * the stack of whichever thread runs the request, and that stack has curl
     * and Mbed TLS under it (see harvest_visitor_data for what is known of it
     * and what is not). It is freed once, straight after http_post(): the
     * only return between the allocation and there is the out-of-memory one,
     * which has nothing of it to free. A request that cannot carry the id it
     * needs is not sent without it: a continuation sent with no visitor id is
     * answered with an empty page that looks like the end of the feed. */
    char *visitor_hdr = NULL;
    if (client == IT_CLIENT_VISIONOS) {
        visitor_hdr = malloc(sizeof g_visitor_header);
        if (!visitor_hdr) {
            vt_log("api %s: out of memory for the visitor header", endpoint);
            snprintf(g_last_error, sizeof g_last_error, "%s",
                     vt_str(S_UNKNOWN_ERROR));
            free(payload);
            return -1;
        }
        if (visitor)
            snprintf(visitor_hdr, sizeof g_visitor_header,
                     "X-Goog-Visitor-Id: %s", visitor);
        else
            snprintf(visitor_hdr, sizeof g_visitor_header, "%s",
                     g_visitor_header);
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
        if (visitor_hdr[0]) headers[n++] = visitor_hdr;
    } else {
        headers[n++] = "X-Youtube-Client-Name: " IT_CLIENT_NAME_ID;
        headers[n++] = "X-Youtube-Client-Version: " IT_CLIENT_VERSION;
    }
    if (auth) headers[n++] = auth;
    headers[n] = NULL;

    long status = 0;
    int ret = http_post(url, "application/json", payload, strlen(payload),
                        headers, cancel, out, &status);
    free(payload);
    free(visitor_hdr);
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

/* What the parser had to throw away from the response it just read: a log
 * line is all that tells "the page ended" from "the page did not fit". */
static void log_parse_drops(const char *what) {
    int token = yt_parse_last_dropped_continuation_len();
    int items = yt_parse_last_dropped_items();
    if (token)
        vt_log("parse %s: next-page token of %d chars DROPPED (limit %d)", what,
               token, YT_CONTINUATION_MAX - 1);
    if (items)
        vt_log("parse %s: %d rows DROPPED (YT_LIST_MAX %d)", what, items,
               YT_LIST_MAX);
}

/* One list request. `fields` is the response filter (NULL for none).
 * continuation != 0: this is a "next page" request, and `visitor` is the id
 * its token is bound to (NULL where the surface has none).
 *
 * `surface` is what the CALLER says this request is (YT_SURF_NONE for a call
 * whose pages have no continuation route). It is a parameter of every call and
 * not guessed from the request, so adding a call cannot change paging by
 * accident; each call of a fallback chain names its own, so a page is never
 * labelled with the request that failed before it. stamp_surface() keeps the
 * name only when the request made is that surface's row of the table.
 *
 * On success the page is stamped here, not in the callers: its surface, and,
 * for a surface whose tokens are bound to a visitor id, that id. A first page
 * takes it from ITS OWN response (the token was issued under that); a
 * continuation page carries forward the one it was sent with and never reads
 * a new one, so the whole list keeps a single id. A first page that has no
 * visitorData to take is not pageable (surface NONE): its token would only
 * ever bring back an empty shell.
 *
 * The parser clears `out` first, so a retry of the same task starts clean as
 * well. */
static int it_list_call(const char *endpoint, const char *fields, json_t *body,
                        int with_auth, it_client_t client, yt_surface_t surface,
                        const char *visitor, int continuation,
                        volatile int *cancel, yt_video_list_t *out) {
    http_buf_t buf = {0};
    int ret = it_call(endpoint, fields, body, with_auth, client, visitor,
                      cancel, &buf);
    if (ret == 0) {
        int count = yt_parse_video_list(buf.data, buf.len, out);
        if (count >= 0) {
            out->surface = stamp_surface(surface, endpoint, client, with_auth);
            const it_route_t *route = route_for_surface(out->surface);
            if (route && route->needs_visitor) {
                if (continuation) {
                    snprintf(out->visitor, sizeof out->visitor, "%s",
                             visitor ? visitor : "");
                } else if (!yt_parse_visitor_data(buf.data, out->visitor,
                                                  sizeof out->visitor)) {
                    vt_log("parse %s: no visitorData in the response, so its "
                           "token is unusable; not pageable", endpoint);
                    out->surface = YT_SURF_NONE;
                }
            }
        }
        vt_log("parse %s: %d videos, continuation=%u chars, surface=%d%s",
               endpoint, count, (unsigned)strlen(out->continuation),
               (int)out->surface, continuation ? " (next page)" : "");
        log_parse_drops(endpoint);
        if (count == 0) {
            /* An empty FIRST page is a failure worth reporting. An empty
             * continuation is YouTube's way of saying the token is stale or
             * was sent with another visitor id (HTTP 200, zero rows): not an
             * error text for the user, the list just has no next page. */
            if (continuation)
                vt_log("continue %s: 0 rows, stale or mismatched token", endpoint);
            else
                snprintf(g_last_error, sizeof g_last_error,
                         vt_str(S_ERR_EMPTY_RESPONSE), (unsigned)buf.len);
        }
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
                      IT_CLIENT_ANDROID, NULL, cancel, &buf);
    if (ret == 0) {
        int count = yt_parse_search(buf.data, buf.len, out, out_channels);
        /* Not it_list_call(), so the page is stamped here, by name: the
         * continuation of this response goes back to search as ANDROID,
         * anonymous. */
        if (count >= 0)
            out->surface = stamp_surface(YT_SURF_SEARCH, "search",
                                         IT_CLIENT_ANDROID, 0);
        int channels = out_channels ? out_channels->count : 0;
        vt_log("parse search: %d videos, %d channels, continuation=%u chars",
               count, channels, (unsigned)strlen(out->continuation));
        log_parse_drops("search");
        if (count == 0 && channels == 0)
            snprintf(g_last_error, sizeof g_last_error,
                     vt_str(S_ERR_EMPTY_RESPONSE), (unsigned)buf.len);
        ret = count >= 0 ? 0 : -1;
    }
    http_buf_free(&buf);
    return ret;
}

int yt_continue(const char *continuation, yt_surface_t surface,
                const char *visitor, volatile int *cancel,
                yt_video_list_t *out) {
    const it_route_t *route = route_for_surface(surface);
    if (!route || !continuation || !continuation[0] ||
        (route->needs_visitor && (!visitor || !visitor[0]))) {
        /* No request: a token with no known way back is not worth a guess —
         * the old guess (search) is what broke every other list — and a
         * visitor-bound token without its visitor is a certain empty shell. */
        vt_log("continue: no route for surface %d (token %s, visitor %s), "
               "not sending", (int)surface,
               continuation && continuation[0] ? "set" : "empty",
               visitor && visitor[0] ? "set" : "empty");
        snprintf(g_last_error, sizeof g_last_error, "%s",
                 vt_str(S_UNKNOWN_ERROR));
        return -1;
    }
    return it_list_call(route->endpoint, route->fields,
                        json_pack("{s:s}", "continuation", continuation),
                        route->with_auth, route->client, route->surface,
                        route->needs_visitor ? visitor : NULL, 1, cancel, out);
}

int yt_related(const char *video_id, volatile int *cancel,
               yt_video_list_t *out) {
    /* Related videos: nothing asks for a second page, so no route (NONE). */
    return it_list_call("next", IT_LIST_FIELDS,
                        json_pack("{s:s}", "videoId", video_id), 0,
                        IT_CLIENT_ANDROID, YT_SURF_NONE, NULL, 0, cancel, out);
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
     * googleapis host that flips to LOGIN_REQUIRED. A first page is 58-84
     * rows (randomised per call; 77 seen in TH) and carries a ~4-4.5 KB next-page
     * token (tests/fixtures/browse_home_visionos.json: 76 rows, 4482
     * characters), which is why YT_LIST_MAX and
     * YT_CONTINUATION_MAX are sized the way they are: while they were 60 and
     * 2048 the tail of every page and the token were silently dropped and the
     * Start tab could never load more. This page is the one that is pageable:
     * it_list_call stamps it BROWSE_WEB and takes the visitor id out of its own
     * response (hence IT_WEB_BROWSE_FIELDS, which keeps that field), and
     * yt_continue() sends the token back with that id. The signed-in TV page
     * below is stamped NONE — see the route table.
     *
     * The anonymous call doubles as the fallback for a signed-in session
     * whose TV browse comes back empty, which is what a revoked or
     * reputation-gated token looks like from here. */
    if (yt_oauth_logged_in()) {
        /* NONE, not YT_SURF_BROWSE_TV: that one is reserved until a signed-in
         * capture shows where this feed's own continuation sits. */
        int ret = it_list_call("browse", IT_LIST_FIELDS,
                               json_pack("{s:s}", "browseId", "FEwhat_to_watch"),
                               1, IT_CLIENT_TV, YT_SURF_NONE, NULL, 0, cancel,
                               out);
        if (ret == 0 && out->count > 0) return 0;
        if (cancel && *cancel) return ret;
        vt_log("home: tv feed empty (ret=%d), retrying anonymously", ret);
    }
    return it_list_call("browse", IT_WEB_BROWSE_FIELDS,
                        json_pack("{s:s}", "browseId", "FEwhat_to_watch"), 0,
                        IT_CLIENT_VISIONOS, YT_SURF_BROWSE_WEB, NULL, 0, cancel,
                        out);
}

int yt_subscriptions_feed(volatile int *cancel, yt_video_list_t *out) {
    if (!yt_oauth_logged_in()) return -2;
    /* The TV context matches the OAuth token; the android context is kept
     * as a fallback in case the TV path regresses. */
    int ret = it_list_call("browse", IT_LIST_FIELDS,
                           json_pack("{s:s}", "browseId", "FEsubscriptions"),
                           1, IT_CLIENT_TV, YT_SURF_NONE, NULL, 0, cancel, out);
    /* Sorted explicitly: the stable sort is a no-op when YouTube already
     * returns the feed newest-first, and a real fix when the TV surface
     * groups it by channel or by shelf instead. */
    if (ret == 0 && out->count > 0) {
        yt_sort_newest_first(out);
        return 0;
    }
    vt_log("subs via tv client failed, trying android");
    /* Neither page of this feed is pageable yet (both name NONE; see the
     * route table for why), so the Abos tab shows one page, as it always
     * did. */
    ret = it_list_call("browse", IT_LIST_FIELDS,
                       json_pack("{s:s}", "browseId", "FEsubscriptions"), 1,
                       IT_CLIENT_ANDROID, YT_SURF_NONE, NULL, 0, cancel, out);
    if (ret == 0) yt_sort_newest_first(out);
    return ret;
}

int yt_channel_list(volatile int *cancel, yt_channel_list_t *out) {
    if (!yt_oauth_logged_in()) return -2;
    http_buf_t buf = {0};
    int ret = it_call("browse", IT_LIST_FIELDS,
                      json_pack("{s:s}", "browseId", "FEchannels"), 1,
                      IT_CLIENT_TV, NULL, cancel, &buf);
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
    int ret = it_call("browse", IT_LIST_FIELDS, body, 0, IT_CLIENT_TV, NULL,
                      cancel, &buf);
    if (ret == 0) {
        /* A playlist surface carries no channel header, so channel_title
         * stays empty and list_screen.c keeps the name it already has from
         * the subscriptions list. */
        int count = yt_parse_channel_page(buf.data, buf.len, out,
                                          channel_title, channel_title_cap);
        /* Anonymous TV, as requested above: the uploads token goes back the
         * same way (YT_SURF_CHANNEL_TV). Not it_list_call, so stamped here,
         * by name. */
        if (count >= 0)
            out->surface = stamp_surface(YT_SURF_CHANNEL_TV, "browse",
                                         IT_CLIENT_TV, 0);
        yt_sort_newest_first(out);
        vt_log("parse channel uploads: %d videos, continuation=%u chars, "
               "surface=%d", count, (unsigned)strlen(out->continuation),
               (int)out->surface);
        log_parse_drops("channel uploads");
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
                      NULL, body, 1, IT_CLIENT_TV, NULL, cancel, &buf);
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
        int ret = it_call("player", NULL, body, 0, IT_CLIENT_VISIONOS, NULL,
                          cancel, &buf);
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
    int ret = it_call("player", NULL, body, 0, IT_CLIENT_ANDROID, NULL, cancel,
                      &buf);
    if (ret == 0) ret = yt_parse_player(buf.data, buf.len, out);
    http_buf_free(&buf);
    return ret;
}
