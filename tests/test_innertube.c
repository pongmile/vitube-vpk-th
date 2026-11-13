/* Host-side tests of the request side of src/yt/innertube.c: the real file,
 * #included below (so that its static functions can be called, see
 * test_stamp_is_named), with the real parser and jansson, against a scripted
 * network. Build it WITHOUT innertube.c on the command line:
 *   gcc -Isrc -Iexternal/jansson tests/test_innertube.c src/yt/parse.c <jansson objects>
 * -DINNERTUBE_SRC=\"/path/modified.c\" runs a modified copy (mutation tests).
 *
 * What test_parse.c cannot see: WHERE a continuation token is sent. Every
 * request is recorded (URL, headers, body) and answered from a script, so the
 * tests can say which client, which endpoint, which fields filter, which
 * visitor id and which surface stamp went with which page. The answers are
 * the live captures in tests/fixtures (the pages of the signed-out Start feed
 * as the app now asks for them, the empty shell YouTube sends for a visitor
 * id that does not match the token, the last page of the feed, a channel's
 * uploads, a search) plus a few hand-made ones.
 *
 * Why this exists (issue #10): the Start tab could not load more. Fixing it
 * took four things that all have to stay true together, and none of them is
 * visible in the parser: the token of the Start feed has to go back to the
 * VISIONOS browse request that issued it, with the fields filter that keeps
 * responseContext/visitorData, together with the visitor id of the page that
 * issued it (a token sent with another id is answered with HTTP 200 and no
 * rows at all), and the id has to survive other requests that rewrite the
 * app's global one in the meantime. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "log.h"
#include "net/http.h"
#include "ui/strings.h"
#include "yt/innertube.h"
#include "yt/oauth.h"
#include "yt/parse.h"
#include "yt/region.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

/* ---- what innertube.c links against, faked ----------------------------------- */
static int g_logged_in;
int yt_oauth_logged_in(void) { return g_logged_in; }
const char *yt_oauth_authorization(volatile int *cancel) {
    (void)cancel;
    return "Authorization: Bearer TESTTOKEN";
}
const char *yt_region_hl(void) { return "de"; }
const char *yt_region_gl(void) { return "DE"; }
void vt_log(const char *fmt, ...) { (void)fmt; }
const char *vt_str(VtStringId id) {
    switch (id) {
    case S_ERR_TRANSPORT: return "Connection error %d";
    case S_ERR_HTTP: return "HTTP error %ld";
    case S_ERR_EMPTY_RESPONSE: return "Empty response (%u bytes)";
    case S_UNKNOWN_ERROR: return "Unknown error";
    default: return "text";
    }
}

void http_buf_free(http_buf_t *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }
/* What vita_https_escape (curl_easy_escape) does: everything except
 * A-Za-z0-9 - . _ ~ becomes %XX, so a ',' is %2C and a '/' is %2F. */
char *http_escape(const char *t) {
    char *o = malloc(strlen(t) * 3 + 1), *p = o;
    for (; *t; t++) {
        unsigned char c = (unsigned char)*t;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~')
            *p++ = (char)c;
        else
            p += sprintf(p, "%%%02X", c);
    }
    *p = '\0';
    return o;
}
int http_init(void) { return 0; }
void http_shutdown(void) {}
int http_connected(void) { return 1; }
unsigned http_transport_failures(void) { return 0; }
int http_get(const char *u, const char *const *h, volatile int *c, http_buf_t *o, long *s) {
    (void)u; (void)h; (void)c; (void)o; (void)s;
    return -1;
}

/* The code under test, in this translation unit: the stamping rule and
 * it_list_call() are static, and what they must do for a call nobody has made
 * yet cannot be shown through the calls that exist. Everything it links
 * against is above. */
#ifndef INNERTUBE_SRC
#define INNERTUBE_SRC "yt/innertube.c"
#endif
#include INNERTUBE_SRC

/* ---- the scripted network ---------------------------------------------------------- */
typedef struct {
    char url[600];
    char headers[8][1000];
    int nheaders;
    char *body;
} Req;
#define MAXREQ 16
static Req g_req[MAXREQ];
static int g_nreq;

typedef struct { char *data; size_t len; long status; int ret; } Resp;
static Resp g_resp[MAXREQ];
static int g_nresp, g_next_resp;

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing fixture %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); exit(2); }
    b[n] = '\0';
    fclose(f);
    *len = (size_t)n;
    return b;
}
static void queue_data(char *data, size_t len, long status, int ret) {
    if (g_nresp >= MAXREQ) { fprintf(stderr, "script too long\n"); exit(2); }
    g_resp[g_nresp].data = data;
    g_resp[g_nresp].len = len;
    g_resp[g_nresp].status = status;
    g_resp[g_nresp].ret = ret;
    g_nresp++;
}
static void queue_file(const char *path) { size_t n; char *d = slurp(path, &n); queue_data(d, n, 200, 0); }
static void queue_str(const char *s) { queue_data(strdup(s), strlen(s), 200, 0); }
static void queue_status(long status, const char *body) { queue_data(strdup(body), strlen(body), status, 0); }
static void queue_transport_error(void) { queue_data(NULL, 0, 0, -3); }

static void reset(void) {
    for (int i = 0; i < g_nreq; i++) free(g_req[i].body);
    for (int i = 0; i < g_nresp; i++) free(g_resp[i].data);
    memset(g_req, 0, sizeof g_req);
    memset(g_resp, 0, sizeof g_resp);
    g_nreq = g_nresp = g_next_resp = 0;
}

int http_post(const char *url, const char *ct, const void *body, size_t body_len,
              const char *const *hdrs, volatile int *cancel, http_buf_t *out, long *status) {
    (void)ct; (void)cancel;
    if (g_nreq >= MAXREQ) { fprintf(stderr, "too many requests\n"); exit(2); }
    Req *r = &g_req[g_nreq++];
    snprintf(r->url, sizeof r->url, "%s", url);
    for (const char *const *h = hdrs; h && *h && r->nheaders < 8; h++)
        snprintf(r->headers[r->nheaders++], sizeof r->headers[0], "%s", *h);
    r->body = malloc(body_len + 1);
    memcpy(r->body, body, body_len);
    r->body[body_len] = '\0';
    if (g_next_resp >= g_nresp) { fprintf(stderr, "FAIL %s:%d: unscripted request #%d %s\n", __FILE__, __LINE__, g_nreq, url); exit(3); }
    Resp *s = &g_resp[g_next_resp++];
    if (s->ret < 0) { *status = 0; return s->ret; }
    free(out->data);
    out->data = malloc(s->len + 1);
    memcpy(out->data, s->data, s->len);
    out->data[s->len] = '\0';
    out->len = out->cap = s->len;
    *status = s->status;
    return 0;
}

static const char *header_value(const Req *r, const char *prefix) {
    for (int i = 0; i < r->nheaders; i++)
        if (strncmp(r->headers[i], prefix, strlen(prefix)) == 0) return r->headers[i] + strlen(prefix);
    return NULL;
}
static int has_header(const Req *r, const char *prefix) { return header_value(r, prefix) != NULL; }
static int count_headers(const Req *r, const char *prefix) {
    int n = 0;
    for (int i = 0; i < r->nheaders; i++)
        if (strncmp(r->headers[i], prefix, strlen(prefix)) == 0) n++;
    return n;
}
static const char *client_name(const Req *r) {
    const char *v = header_value(r, "X-Youtube-Client-Name: ");
    return v ? v : "";
}
/* The request body's context.client.clientName and its top-level
 * continuation / videoId, as jansson sees them. */
static void body_info(const Req *r, char *client, size_t cap, char *tok, size_t tcap) {
    json_error_t e;
    json_t *root = json_loads(r->body, 0, &e);
    client[0] = tok[0] = '\0';
    if (!root) return;
    json_t *c = json_object_get(json_object_get(json_object_get(root, "context"), "client"), "clientName");
    if (json_is_string(c)) snprintf(client, cap, "%s", json_string_value(c));
    json_t *t = json_object_get(root, "continuation");
    if (json_is_string(t)) snprintf(tok, tcap, "%s", json_string_value(t));
    json_decref(root);
}

#define WEB_URL "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false&"
#define API_URL "https://youtubei.googleapis.com/youtubei/v1/"
/* The fields filter as http_escape sends it: ',' is %2C and '/' is %2F. */
#define WEB_FIELDS "fields=contents%2CcontinuationContents%2CresponseContext%2FvisitorData"
#define PLAIN_FIELDS "fields=contents%2CcontinuationContents"

/* The one real next page of a browse continuation in the shape YouTube
 * answers with (sectionListContinuation), hand-cut to two rows, and a
 * visitorData that is NOT the one it was sent with. */
static const char *PAGE2_SECTION =
    "{\"responseContext\":{\"visitorData\":\"SOMETHING_ELSE\"},"
    "\"continuationContents\":{\"sectionListContinuation\":{\"contents\":[{\"itemSectionRenderer\":{\"contents\":["
    "{\"compactVideoRenderer\":{\"videoId\":\"AAAAAAAAAA1\",\"title\":{\"runs\":[{\"text\":\"Two one\"}]},"
    "\"shortBylineText\":{\"runs\":[{\"text\":\"Chan\"}]},\"thumbnail\":{\"thumbnails\":[{\"url\":\"https://i.ytimg.com/a.jpg\",\"width\":320}]}}},"
    "{\"compactVideoRenderer\":{\"videoId\":\"AAAAAAAAAA2\",\"title\":{\"runs\":[{\"text\":\"Two two\"}]},"
    "\"shortBylineText\":{\"runs\":[{\"text\":\"Chan\"}]},\"thumbnail\":{\"thumbnails\":[{\"url\":\"https://i.ytimg.com/b.jpg\",\"width\":320}]}}}"
    "]}}],\"continuations\":[{\"nextContinuationData\":{\"continuation\":\"PAGE3TOKEN\"}}]}}}";

/* A page-1 response with the app's new filter would start with
 * responseContext. The older fixtures were captured through the old filter
 * (without it): put the visitor id in front of one, as YouTube now does. */
static void queue_file_with_visitor(const char *path, const char *visitor) {
    size_t n;
    char *d = slurp(path, &n);
    char *o = malloc(n + strlen(visitor) + 64);
    int w = sprintf(o, "{\"responseContext\":{\"visitorData\":\"%s\"},", visitor);
    memcpy(o + w, d + 1, n - 1); /* skip the leading { */
    o[w + n - 1] = '\0';
    queue_data(o, strlen(o), 200, 0);
    free(d);
}

/* ---- the signed-out Start feed: page 1, a player call in between, page 2 --------------- */
static void test_home_anonymous_and_continue(void) {
    static yt_video_list_t page, page2;
    static yt_stream_t st;
    reset();
    g_logged_in = 0;
    queue_file_with_visitor("fixtures/browse_home_visionos.json", "VISITOR_A%3D%3D");
    /* An ANDROID player call lands between the pages and latches another id
     * as the app's global one — what really happens when a video is opened
     * from the list and the user comes back to scroll on. */
    queue_str("{\"responseContext\":{\"visitorData\":\"LATCH_B\"},\"playabilityStatus\":{\"status\":\"OK\"}}");
    queue_str(PAGE2_SECTION);

    int rc = yt_home_feed(NULL, &page);
    CHECK(rc == 0, "home rc %d", rc);
    CHECK(g_nreq == 1, "requests %d", g_nreq);
    CHECK(strcmp(g_req[0].url, WEB_URL WEB_FIELDS) == 0, "page 1 url %s", g_req[0].url);
    CHECK(strcmp(client_name(&g_req[0]), "101") == 0, "page 1 client %s", client_name(&g_req[0]));
    CHECK(!has_header(&g_req[0], "Authorization"), "anonymous page 1 must not send auth");
    CHECK(page.count == 76, "count %d", page.count);
    CHECK(strlen(page.continuation) == 4482, "token length %zu", strlen(page.continuation));
    CHECK(page.surface == YT_SURF_BROWSE_WEB, "surface %d", (int)page.surface);
    CHECK(strcmp(page.visitor, "VISITOR_A%3D%3D") == 0, "page visitor '%s'", page.visitor);
    CHECK(yt_parse_last_dropped_continuation_len() == 0 && yt_parse_last_dropped_items() == 0, "drops");

    rc = yt_resolve_stream("AAAAAAAAAAA", NULL, &st);
    CHECK(rc == 0 && g_nreq == 2, "player rc %d requests %d", rc, g_nreq);

    rc = yt_continue(page.continuation, page.surface, page.visitor, NULL, &page2);
    CHECK(rc == 0, "continue rc %d", rc);
    CHECK(g_nreq == 3, "requests %d", g_nreq);
    const Req *r = &g_req[2];
    CHECK(strcmp(r->url, WEB_URL WEB_FIELDS) == 0, "continuation url %s", r->url);
    CHECK(strcmp(client_name(r), "101") == 0, "continuation client %s", client_name(r));
    CHECK(has_header(r, "Origin: https://www.youtube.com"), "Origin header");
    CHECK(!has_header(r, "Authorization"), "anonymous continuation must not send auth");
    const char *vid = header_value(r, "X-Goog-Visitor-Id: ");
    CHECK(vid && strcmp(vid, "VISITOR_A%3D%3D") == 0,
          "the continuation must carry the page's visitor id, not the one the player call latched: '%s'",
          vid ? vid : "(none)");
    CHECK(count_headers(r, "X-Goog-Visitor-Id") == 1, "exactly one visitor header, got %d",
          count_headers(r, "X-Goog-Visitor-Id"));
    char cn[32], tok[20000];
    body_info(r, cn, sizeof cn, tok, sizeof tok);
    CHECK(strcmp(cn, "VISIONOS") == 0, "body clientName %s", cn);
    CHECK(strcmp(tok, page.continuation) == 0, "the token goes out whole (%zu chars)", strlen(tok));
    CHECK(page2.count == 2 && strcmp(page2.continuation, "PAGE3TOKEN") == 0, "page 2: %d rows, token '%s'",
          page2.count, page2.continuation);
    CHECK(page2.surface == YT_SURF_BROWSE_WEB, "page 2 surface %d", (int)page2.surface);
    CHECK(strcmp(page2.visitor, "VISITOR_A%3D%3D") == 0,
          "a continuation's own visitorData must not replace the list's id, have '%s'", page2.visitor);
}

/* ---- the live captures, page by page ------------------------------------------------------ */
static void test_home_live_pages(void) {
    static yt_video_list_t page, last, shell;
    reset();
    g_logged_in = 0;
    /* Captured live (2026-11-13) with exactly the request this app sends. */
    queue_file("fixtures/browse_home_visionos_filtered_p1.json");
    queue_file("fixtures/browse_home_visionos_last.json");
    queue_file("fixtures/browse_home_visionos_wrongvisitor.json");
    int rc = yt_home_feed(NULL, &page);
    CHECK(rc == 0 && page.count == 59, "live page 1: rc %d, %d rows", rc, page.count);
    CHECK(strlen(page.continuation) == 4048, "live page 1 token %zu", strlen(page.continuation));
    CHECK(page.surface == YT_SURF_BROWSE_WEB, "surface %d", (int)page.surface);
    CHECK(strcmp(page.visitor, "REDACTED_VISITOR_DATA_PAGE1") == 0, "the page's visitor is its own response's: '%s'", page.visitor);
    CHECK(yt_parse_last_dropped_continuation_len() == 0, "token dropped");

    /* The last page of the feed (live: page 5): rows, and only a
     * reloadContinuationData. No next token, and the page is still stamped so
     * the list can tell "ended" from "never had one". */
    rc = yt_continue(page.continuation, page.surface, page.visitor, NULL, &last);
    CHECK(rc == 0 && last.count == 25, "last page: rc %d, %d rows", rc, last.count);
    CHECK(last.continuation[0] == '\0', "the last page has no next page, only a reload: got %zu chars", strlen(last.continuation));
    CHECK(last.surface == YT_SURF_BROWSE_WEB && strcmp(last.visitor, page.visitor) == 0, "last page stamp: surface %d visitor '%s'", (int)last.surface, last.visitor);
    CHECK(yt_last_error()[0] == '\0', "no error text for the end of a feed: '%s'", yt_last_error());

    /* What a mismatched visitor id gets (live): HTTP 200, no rows, no token,
     * and a responseContext of its own whose visitorData is NOT to be adopted.
     * No error either: the caller reads it as "this list has no next page". */
    rc = yt_continue(page.continuation, YT_SURF_BROWSE_WEB, "SOME_OTHER_ID", NULL, &shell);
    CHECK(rc == 0 && shell.count == 0 && shell.continuation[0] == '\0', "empty shell: rc %d, %d rows, token %zu chars", rc, shell.count, strlen(shell.continuation));
    CHECK(yt_last_error()[0] == '\0', "an empty continuation leaves no error text: '%s'", yt_last_error());
    CHECK(shell.surface == YT_SURF_BROWSE_WEB, "an empty page is still stamped with its surface: %d", (int)shell.surface);
    CHECK(strcmp(shell.visitor, "SOME_OTHER_ID") == 0, "an empty page carries the id it was sent with, not its response's: '%s'", shell.visitor);
}

/* ---- the id a page-1 request carried, and the one its response hands back --------------------- */
static void test_visitor_latch_and_page_id(void) {
    static yt_video_list_t page, page2;
    static yt_stream_t st;
    reset();
    g_logged_in = 0;
    queue_str("{\"responseContext\":{\"visitorData\":\"LATCH_X\"},\"playabilityStatus\":{\"status\":\"OK\"}}");
    queue_file_with_visitor("fixtures/browse_home_visionos.json", "PAGE_Y");
    queue_str(PAGE2_SECTION);
    int rc = yt_resolve_stream("AAAAAAAAAAA", NULL, &st);
    CHECK(rc == 0, "player rc %d", rc);
    /* The id any response hands out rides along on the next VISIONOS request
     * (that is what turned the LOGIN_REQUIRED of a fresh process into OK)... */
    rc = yt_home_feed(NULL, &page);
    CHECK(rc == 0 && page.count == 76, "home rc %d count %d", rc, page.count);
    const char *sent = header_value(&g_req[1], "X-Goog-Visitor-Id: ");
    CHECK(sent && strcmp(sent, "LATCH_X") == 0, "page 1 is sent with the latched id: '%s'", sent ? sent : "(none)");
    /* ...but the list keeps the id of the RESPONSE: live (2026-11-13), the id
     * comes back with a fresh timestamp, and either one works with its token. */
    CHECK(strcmp(page.visitor, "PAGE_Y") == 0, "page visitor is the response's: '%s'", page.visitor);
    rc = yt_continue(page.continuation, page.surface, page.visitor, NULL, &page2);
    CHECK(rc == 0 && page2.count == 2, "continue rc %d count %d", rc, page2.count);
    sent = header_value(&g_req[2], "X-Goog-Visitor-Id: ");
    CHECK(sent && strcmp(sent, "PAGE_Y") == 0, "page 2 is sent with the page's id: '%s'", sent ? sent : "(none)");
}

static void test_home_without_visitor(void) {
    static yt_video_list_t page;
    reset();
    g_logged_in = 0;
    queue_file("fixtures/browse_home_visionos.json"); /* captured through the old filter: no responseContext */
    int rc = yt_home_feed(NULL, &page);
    CHECK(rc == 0 && page.count == 76, "rc %d count %d", rc, page.count);
    CHECK(strlen(page.continuation) == 4482, "the parser still finds the token: %zu", strlen(page.continuation));
    CHECK(page.surface == YT_SURF_NONE, "a page without its visitor id is not pageable: surface %d", (int)page.surface);
    CHECK(page.visitor[0] == '\0', "visitor '%s'", page.visitor);
}

static void test_home_signed_in(void) {
    static yt_video_list_t page;
    reset();
    g_logged_in = 1;
    queue_file("fixtures/channel_videos_tv.json"); /* a TV tile response, as the signed-in feed answers */
    int rc = yt_home_feed(NULL, &page);
    CHECK(rc == 0 && page.count > 0, "home rc %d count %d", rc, page.count);
    CHECK(g_nreq == 1, "requests %d", g_nreq);
    CHECK(strcmp(g_req[0].url, API_URL "browse?prettyPrint=false&" PLAIN_FIELDS) == 0, "url %s", g_req[0].url);
    CHECK(strcmp(client_name(&g_req[0]), "7") == 0, "client %s", client_name(&g_req[0]));
    CHECK(has_header(&g_req[0], "Authorization: Bearer"), "the signed-in feed sends the token");
    CHECK(page.surface == YT_SURF_NONE, "an unverified surface must not be pageable: %d", (int)page.surface);

    /* The signed-in feed empty: the anonymous VISIONOS page is tried, and it
     * is THAT page's surface and visitor the list gets. */
    reset();
    g_logged_in = 1;
    queue_str("{\"contents\":{}}");
    queue_file_with_visitor("fixtures/browse_home_visionos.json", "VIS_FALLBACK");
    rc = yt_home_feed(NULL, &page);
    CHECK(rc == 0 && page.count == 76 && g_nreq == 2, "fallback rc %d count %d requests %d", rc, page.count, g_nreq);
    CHECK(strcmp(client_name(&g_req[0]), "7") == 0 && strcmp(client_name(&g_req[1]), "101") == 0,
          "clients %s then %s", client_name(&g_req[0]), client_name(&g_req[1]));
    CHECK(has_header(&g_req[0], "Authorization") && !has_header(&g_req[1], "Authorization"), "auth only on the TV try");
    CHECK(strstr(g_req[1].url, WEB_FIELDS) != NULL, "the fallback asks for the visitor field: %s", g_req[1].url);
    CHECK(page.surface == YT_SURF_BROWSE_WEB, "the surface names the call that filled the page: %d", (int)page.surface);
    CHECK(strcmp(page.visitor, "VIS_FALLBACK") == 0, "visitor '%s'", page.visitor);
}

static void test_subscriptions_not_pageable(void) {
    static yt_video_list_t page;
    reset();
    g_logged_in = 1;
    queue_file("fixtures/channel_videos_tv.json");
    int rc = yt_subscriptions_feed(NULL, &page);
    CHECK(rc == 0 && page.count > 0, "subs rc %d", rc);
    CHECK(page.surface == YT_SURF_NONE, "subs (TV) surface %d: its first continuation continues one shelf, not the feed", (int)page.surface);

    reset();
    g_logged_in = 1;
    queue_str("{\"contents\":{}}");
    queue_file("fixtures/channel_videos_tv.json");
    rc = yt_subscriptions_feed(NULL, &page);
    CHECK(rc == 0 && page.count > 0 && g_nreq == 2, "subs fallback rc %d requests %d", rc, g_nreq);
    CHECK(page.surface == YT_SURF_NONE, "subs (ANDROID fallback) surface %d", (int)page.surface);
    CHECK(strcmp(client_name(&g_req[1]), "3") == 0 && has_header(&g_req[1], "Authorization"), "the fallback is ANDROID with auth");
}

/* ---- search: its old request, unchanged -------------------------------------------------------- */
static void test_search(void) {
    static yt_video_list_t page, page2;
    static yt_channel_list_t ch;
    reset();
    g_logged_in = 1; /* signed in must still not authenticate a search */
    queue_file("fixtures/search_android.json");
    queue_str(PAGE2_SECTION);
    int rc = yt_search("lofi", NULL, &page, &ch);
    CHECK(rc == 0 && page.count >= 10, "search rc %d count %d", rc, page.count);
    CHECK(page.surface == YT_SURF_SEARCH, "surface %d", (int)page.surface);
    CHECK(page.visitor[0] == '\0', "search needs no visitor: '%s'", page.visitor);
    CHECK(strlen(page.continuation) == 586, "token %zu", strlen(page.continuation));
    CHECK(strcmp(g_req[0].url, API_URL "search?prettyPrint=false&" PLAIN_FIELDS) == 0, "url %s", g_req[0].url);
    rc = yt_continue(page.continuation, page.surface, NULL, NULL, &page2);
    CHECK(rc == 0, "continue rc %d", rc);
    const Req *r = &g_req[1];
    CHECK(strcmp(r->url, g_req[0].url) == 0, "the search continuation goes to the same URL: %s", r->url);
    CHECK(strcmp(client_name(r), "3") == 0 && !has_header(r, "Authorization"), "ANDROID, anonymous");
    CHECK(!has_header(r, "X-Goog-Visitor-Id"), "search sends no visitor header");
    char cn[32], tok[9000];
    body_info(r, cn, sizeof cn, tok, sizeof tok);
    CHECK(strcmp(cn, "ANDROID") == 0 && strcmp(tok, page.continuation) == 0, "body: client %s, token %zu chars", cn, strlen(tok));
    CHECK(page2.surface == YT_SURF_SEARCH, "page 2 surface %d", (int)page2.surface);
}

/* ---- a channel's uploads: anonymous TV, even when signed in ---------------------------------------- */
static void test_channel_uploads(void) {
    static yt_video_list_t page, page2;
    char title[80];
    reset();
    g_logged_in = 1;
    queue_file("fixtures/channel_uploads_tv.json");
    queue_file("fixtures/channel_uploads_tv_page2.json");
    int rc = yt_channel_videos("UCabcdefghijklmnopqrstuv", NULL, &page, title, sizeof title);
    CHECK(rc == 0 && page.count == 15, "rc %d count %d", rc, page.count);
    CHECK(has_header(&g_req[0], "X-Youtube-Client-Name: 7") && !has_header(&g_req[0], "Authorization"), "page 1 is anonymous TV");
    CHECK(strcmp(g_req[0].url, API_URL "browse?prettyPrint=false&" PLAIN_FIELDS) == 0,
          "page 1 keeps the plain filter (an unknown field name is an HTTP 400 on the TV client): %s", g_req[0].url);
    CHECK(strstr(g_req[0].body, "VLUUabcdefghijklmnopqrstuv") != NULL, "uploads playlist id in the body: %s", g_req[0].body);
    CHECK(page.surface == YT_SURF_CHANNEL_TV, "surface %d", (int)page.surface);
    CHECK(strlen(page.continuation) == 124, "token %zu", strlen(page.continuation));
    CHECK(page.visitor[0] == '\0', "no visitor for the TV surface: '%s'", page.visitor);
    /* An empty visitor is what a list with a channel token hands over. */
    rc = yt_continue(page.continuation, page.surface, "", NULL, &page2);
    CHECK(rc == 0 && page2.count == 15, "continue rc %d count %d", rc, page2.count);
    const Req *r = &g_req[1];
    CHECK(strcmp(r->url, API_URL "browse?prettyPrint=false&" PLAIN_FIELDS) == 0, "url %s", r->url);
    CHECK(has_header(r, "X-Youtube-Client-Name: 7") && !has_header(r, "Authorization"),
          "the continuation is anonymous TV even though the user is signed in");
    CHECK(!has_header(r, "X-Goog-Visitor-Id"), "no visitor header for TV");
    char cn[32], tok[9000];
    body_info(r, cn, sizeof cn, tok, sizeof tok);
    CHECK(strcmp(cn, "TVHTML5") == 0 && strcmp(tok, page.continuation) == 0, "body client %s", cn);
    CHECK(page2.surface == YT_SURF_CHANNEL_TV && strlen(page2.continuation) == 124, "page 2: surface %d token %zu", (int)page2.surface, strlen(page2.continuation));
}

/* ---- related videos keep their filter ------------------------------------------------------------------ */
static void test_related(void) {
    static yt_video_list_t page;
    reset();
    g_logged_in = 1;
    queue_file("fixtures/next_android.json");
    int rc = yt_related("dQw4w9WgXcQ", NULL, &page);
    CHECK(rc == 0 && page.count > 0, "related rc %d count %d", rc, page.count);
    CHECK(strcmp(g_req[0].url, API_URL "next?prettyPrint=false&" PLAIN_FIELDS) == 0,
          "an unfiltered 'next' answer is ~830 KB: %s", g_req[0].url);
    CHECK(strcmp(client_name(&g_req[0]), "3") == 0 && !has_header(&g_req[0], "Authorization"), "ANDROID, anonymous");
    CHECK(strstr(g_req[0].body, "dQw4w9WgXcQ") != NULL, "videoId in the body");
    CHECK(page.surface == YT_SURF_NONE, "nothing asks for a second page of related videos: surface %d", (int)page.surface);
}

/* ---- yt_continue says no without sending anything --------------------------------------------------------- */
static void test_no_route(void) {
    static yt_video_list_t out;
    reset();
    g_logged_in = 0;
    CHECK(yt_continue("TOK", YT_SURF_NONE, "V", NULL, &out) == -1, "NONE");
    CHECK(yt_continue("TOK", YT_SURF_BROWSE_TV, "V", NULL, &out) == -1, "reserved TV");
    CHECK(yt_continue("TOK", YT_SURF_BROWSE_ANDROID, "V", NULL, &out) == -1, "reserved ANDROID");
    CHECK(yt_continue("TOK", (yt_surface_t)99, "V", NULL, &out) == -1, "out-of-range surface");
    CHECK(yt_continue("TOK", (yt_surface_t)-1, "V", NULL, &out) == -1, "negative surface");
    CHECK(yt_continue("", YT_SURF_SEARCH, NULL, NULL, &out) == -1, "empty token");
    CHECK(yt_continue(NULL, YT_SURF_SEARCH, NULL, NULL, &out) == -1, "null token");
    CHECK(yt_continue("TOK", YT_SURF_BROWSE_WEB, NULL, NULL, &out) == -1, "BROWSE_WEB without a visitor (NULL)");
    CHECK(yt_continue("TOK", YT_SURF_BROWSE_WEB, "", NULL, &out) == -1, "BROWSE_WEB without a visitor (empty)");
    CHECK(g_nreq == 0, "no request may be sent for any of these, got %d", g_nreq);
    CHECK(yt_last_error()[0] != '\0', "an error text is set");
}

static void test_failures(void) {
    static yt_video_list_t out;
    reset();
    g_logged_in = 0;
    queue_status(400, "{\"error\":{\"message\":\"bad\"}}");
    CHECK(yt_continue("TOK", YT_SURF_BROWSE_WEB, "V", NULL, &out) < 0, "HTTP 400");
    CHECK(strstr(yt_last_error(), "400") != NULL, "error text '%s'", yt_last_error());
    reset();
    queue_transport_error();
    CHECK(yt_continue("TOK", YT_SURF_BROWSE_WEB, "V", NULL, &out) < 0, "transport error");
    reset();
    queue_str("not json");
    CHECK(yt_continue("TOK", YT_SURF_BROWSE_WEB, "V", NULL, &out) < 0, "malformed body");
    reset();
    queue_str("{\"truncated\":");
    CHECK(yt_continue("TOK", YT_SURF_SEARCH, NULL, NULL, &out) < 0, "truncated body");

    /* An empty FIRST page is reported, as it always was. */
    reset();
    queue_str("{\"contents\":{}}");
    int rc = yt_home_feed(NULL, &out);
    CHECK(rc == 0 && out.count == 0 && yt_last_error()[0] != '\0', "an empty first page keeps its error text: rc %d '%s'", rc, yt_last_error());
}

/* ---- sizes: the biggest token and visitor that fit go out whole ----------------------------------------------- */
static void test_limits(void) {
    static yt_video_list_t out;
    static char tok[YT_CONTINUATION_MAX], vis[YT_VISITOR_MAX];
    /* Token alphabet of the real ones: base64url plus percent-escapes. */
    for (int i = 0; i < YT_CONTINUATION_MAX - 1; i++) tok[i] = "ABCxyz019-_%3D"[i % 14];
    tok[YT_CONTINUATION_MAX - 1] = '\0';
    for (int i = 0; i < YT_VISITOR_MAX - 1; i++) vis[i] = "CgtEps6O7PTxe8-_"[i % 16];
    vis[YT_VISITOR_MAX - 1] = '\0';
    reset();
    queue_str(PAGE2_SECTION);
    int rc = yt_continue(tok, YT_SURF_BROWSE_WEB, vis, NULL, &out);
    CHECK(rc == 0 && out.count == 2, "max token, max visitor: rc %d count %d", rc, out.count);
    char cn[32];
    static char back[YT_CONTINUATION_MAX + 16];
    body_info(&g_req[0], cn, sizeof cn, back, sizeof back);
    CHECK(strcmp(back, tok) == 0, "the longest token goes out whole (%zu of %zu chars)", strlen(back), strlen(tok));
    const char *sent = header_value(&g_req[0], "X-Goog-Visitor-Id: ");
    CHECK(sent && strcmp(sent, vis) == 0, "the longest visitor id goes out whole (%zu of %zu chars)", sent ? strlen(sent) : 0, strlen(vis));
    CHECK(strcmp(out.visitor, vis) == 0, "and is carried forward whole");
}

/* ---- an oversize token is not stored, and says so ---------------------------------------------------------------- */
static void test_dropped_logged(void) {
    static yt_video_list_t out;
    reset();
    g_logged_in = 0;
    size_t n = YT_CONTINUATION_MAX + 500;
    char *big = malloc(n + 600);
    int w = snprintf(big, n + 600,
        "{\"responseContext\":{\"visitorData\":\"V\"},\"contents\":[{\"compactVideoRenderer\":{\"videoId\":\"AAAAAAAAAAA\","
        "\"title\":{\"simpleText\":\"t\"}}}],\"continuations\":[{\"nextContinuationData\":{\"continuation\":\"");
    memset(big + w, 'T', n);
    w += (int)n;
    snprintf(big + w, 100, "\"}}]}");
    queue_str(big);
    free(big);
    int rc = yt_home_feed(NULL, &out);
    CHECK(rc == 0 && out.count == 1, "rc %d count %d", rc, out.count);
    CHECK(out.continuation[0] == '\0', "an oversize token must not be stored");
    CHECK(yt_parse_last_dropped_continuation_len() == (int)n, "dropped length %d", yt_parse_last_dropped_continuation_len());
}

/* ---- the stamp is named by the call, never guessed from the request ------------------------- */

/* One request shape and the surface whose route it is (NONE: no route). */
typedef struct {
    const char *endpoint;
    it_client_t client;
    int with_auth;
    yt_surface_t route;
    const char *what;
} Shape;

static void test_stamp_is_named(void) {
    /* The routes that exist, written out here and not read from g_routes:
     * search, the signed-out Start feed and a channel's uploads. */
    static const Shape shapes[] = {
        { "search", IT_CLIENT_ANDROID,  0, YT_SURF_SEARCH,     "search, ANDROID, anonymous" },
        { "browse", IT_CLIENT_VISIONOS, 0, YT_SURF_BROWSE_WEB, "browse, VISIONOS, anonymous" },
        { "browse", IT_CLIENT_TV,       0, YT_SURF_CHANNEL_TV, "browse, TV, anonymous" },
        { "browse", IT_CLIENT_TV,       1, YT_SURF_NONE,       "browse, TV, OAuth" },
        { "browse", IT_CLIENT_ANDROID,  1, YT_SURF_NONE,       "browse, ANDROID, OAuth" },
        { "next",   IT_CLIENT_ANDROID,  0, YT_SURF_NONE,       "next, ANDROID, anonymous" },
        { "browse", IT_CLIENT_ANDROID,  0, YT_SURF_NONE,       "browse, ANDROID, anonymous" },
        { "search", IT_CLIENT_TV,       0, YT_SURF_NONE,       "search, TV, anonymous" },
    };
    static const yt_surface_t named[] = { YT_SURF_NONE, YT_SURF_SEARCH, YT_SURF_BROWSE_WEB,
                                          YT_SURF_CHANNEL_TV, YT_SURF_BROWSE_TV, YT_SURF_BROWSE_ANDROID };
    /* Every name against every request shape: the name survives only where the
     * request is that surface's own route. Nothing is ever stamped because of
     * what the request looks like. */
    for (size_t s = 0; s < sizeof named / sizeof *named; s++)
        for (size_t q = 0; q < sizeof shapes / sizeof *shapes; q++) {
            const Shape *sh = &shapes[q];
            yt_surface_t want = (named[s] != YT_SURF_NONE && named[s] == sh->route) ? named[s] : YT_SURF_NONE;
            yt_surface_t got = stamp_surface(named[s], sh->endpoint, sh->client, sh->with_auth);
            CHECK(got == want, "named %d for [%s]: stamped %d, want %d", (int)named[s], sh->what, (int)got, (int)want);
        }

    /* The case the old inference got wrong in waiting: an anonymous TV browse
     * made for some other purpose (it has the request shape of a channel's
     * uploads) that names no surface. It was stamped CHANNEL_TV by its look
     * alone and its token offered for paging; now it is NONE. The page does
     * carry a token, so this is the case where being NONE is what keeps the
     * list from offering a route nobody verified. */
    static yt_video_list_t out;
    reset();
    g_logged_in = 0;
    queue_file("fixtures/channel_uploads_tv.json");
    int rc = it_list_call("browse", IT_LIST_FIELDS, json_pack("{s:s}", "browseId", "VLUUunlisted"), 0,
                          IT_CLIENT_TV, YT_SURF_NONE, NULL, 0, NULL, &out);
    CHECK(rc == 0 && out.count > 0, "setup: rc %d, %d rows", rc, out.count);
    CHECK(out.continuation[0] != '\0', "setup: this page carries a token");
    CHECK(strcmp(g_req[0].url, API_URL "browse?prettyPrint=false&" PLAIN_FIELDS) == 0 &&
          strcmp(client_name(&g_req[0]), "7") == 0 && !has_header(&g_req[0], "Authorization"),
          "setup: the request has the shape of a channel's uploads: %s / client %s", g_req[0].url, client_name(&g_req[0]));
    CHECK(out.surface == YT_SURF_NONE, "an unlisted call is stamped NONE, got %d", (int)out.surface);

    /* ...and naming the surface is what makes the same request pageable. */
    reset();
    queue_file("fixtures/channel_uploads_tv.json");
    rc = it_list_call("browse", IT_LIST_FIELDS, json_pack("{s:s}", "browseId", "VLUUlisted"), 0,
                      IT_CLIENT_TV, YT_SURF_CHANNEL_TV, NULL, 0, NULL, &out);
    CHECK(rc == 0 && out.surface == YT_SURF_CHANNEL_TV, "a call that names it: rc %d, stamped %d", rc, (int)out.surface);

    /* A name for a request that is not its route: not pageable, and no visitor
     * id taken for a route that does not exist. */
    reset();
    queue_file_with_visitor("fixtures/browse_home_visionos.json", "VISITOR_X");
    rc = it_list_call("browse", IT_WEB_BROWSE_FIELDS, json_pack("{s:s}", "browseId", "FEwhat_to_watch"), 0,
                      IT_CLIENT_VISIONOS, YT_SURF_CHANNEL_TV, NULL, 0, NULL, &out);
    CHECK(rc == 0 && out.count == 76 && out.surface == YT_SURF_NONE && out.visitor[0] == '\0',
          "VISIONOS browse naming the TV route: rc %d, %d rows, stamped %d, visitor '%s'", rc, out.count, (int)out.surface, out.visitor);
    reset();
    queue_file_with_visitor("fixtures/browse_home_visionos.json", "VISITOR_X");
    rc = it_list_call("browse", IT_WEB_BROWSE_FIELDS, json_pack("{s:s}", "browseId", "FEwhat_to_watch"), 0,
                      IT_CLIENT_VISIONOS, YT_SURF_NONE, NULL, 0, NULL, &out);
    CHECK(rc == 0 && out.surface == YT_SURF_NONE && out.visitor[0] == '\0',
          "VISIONOS browse naming nothing: stamped %d, visitor '%s'", (int)out.surface, out.visitor);
    reset();
    queue_file_with_visitor("fixtures/browse_home_visionos.json", "VISITOR_X");
    rc = it_list_call("browse", IT_WEB_BROWSE_FIELDS, json_pack("{s:s}", "browseId", "FEwhat_to_watch"), 0,
                      IT_CLIENT_VISIONOS, YT_SURF_BROWSE_WEB, NULL, 0, NULL, &out);
    CHECK(rc == 0 && out.surface == YT_SURF_BROWSE_WEB && strcmp(out.visitor, "VISITOR_X") == 0,
          "VISIONOS browse naming its route: stamped %d, visitor '%s'", (int)out.surface, out.visitor);
}

int main(void) {
    /* Statics inside innertube.c (the latched visitor id) outlive each test:
     * every test names the ids it relies on. */
    test_home_anonymous_and_continue();
    test_home_live_pages();
    test_visitor_latch_and_page_id();
    test_home_without_visitor();
    test_home_signed_in();
    test_subscriptions_not_pageable();
    test_search();
    test_channel_uploads();
    test_related();
    test_no_route();
    test_failures();
    test_limits();
    test_dropped_logged();
    test_stamp_is_named();
    reset();
    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL INNERTUBE TESTS PASSED\n");
    return 0;
}
