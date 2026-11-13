/* Host-side parser tests against live-captured InnerTube fixtures. */
#include "../src/yt/parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing fixture %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    fread(buf, 1, (size_t)n, f);
    buf[n] = '\0';
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

/* ---- later pages of the signed-out Start feed -----------------------------------
 * Captured live from the same VISIONOS surface, one page after the other, each
 * with the token of the one before. The token GROWS with every page it is
 * passed on, which is what YT_CONTINUATION_MAX has to carry: 4482 -> 7498 ->
 * 10788 -> 12892 characters. A page 2 holds its rows in a shelfRenderer's
 * verticalListRenderer and its next token in
 * continuationContents.sectionListContinuation.continuations[] — before a
 * sibling reloadContinuationData. */
static int page_of(const char *path, yt_video_list_t *out) {
    size_t len;
    char *buf = slurp(path, &len);
    int n = yt_parse_video_list(buf, len, out);
    free(buf);
    return n;
}

static void check_home_pages(void) {
    static yt_video_list_t page;
    int n = page_of("fixtures/browse_home_visionos_page2.json", &page);
    printf("home page 2: %d videos, continuation=%zu chars\n", n,
           strlen(page.continuation));
    CHECK(n == 62, "page 2 expected 62 rows, got %d", n);
    CHECK(strlen(page.continuation) == 7498,
          "page 2 expected its 7498-char token (not the 7488-char reload), got %zu",
          strlen(page.continuation));
    CHECK(yt_parse_last_dropped_continuation_len() == 0, "page 2 token dropped");
    for (int i = 0; i < n; i++)
        CHECK(strlen(page.items[i].video_id) == 11 && page.items[i].title[0],
              "page 2 item %d incomplete", i);

    n = page_of("fixtures/browse_home_visionos_page3.json", &page);
    printf("home page 3: %d videos, continuation=%zu chars\n", n,
           strlen(page.continuation));
    CHECK(n == 62, "page 3 expected 62 rows, got %d", n);
    CHECK(strlen(page.continuation) == 10788,
          "page 3 expected its 10788-char token, got %zu (dropped: %d)",
          strlen(page.continuation), yt_parse_last_dropped_continuation_len());

    n = page_of("fixtures/browse_home_visionos_page4.json", &page);
    printf("home page 4: %d videos, continuation=%zu chars\n", n,
           strlen(page.continuation));
    CHECK(n == 32, "page 4 expected 32 rows, got %d", n);
    CHECK(strlen(page.continuation) == 12892,
          "page 4 expected its 12892-char token, got %zu (dropped: %d)",
          strlen(page.continuation), yt_parse_last_dropped_continuation_len());

    /* The headroom the limit is sized for. The tokens captured here are the DE
     * ones (about 2.3 KB more a page); the same feed in TH grows by about
     * 3.3 KB a page (both measured live on 2026-11-13). The token that page 4
     * carries plus three more pages of that faster growth must still fit, or
     * a long scroll would end silently on a token that no longer does. The
     * limit is the constant itself, not a copy of its value, so changing it
     * either way is judged here. */
    {
        const size_t growth_th = 3300, more_pages = 3;
        size_t p4 = strlen(page.continuation);
        CHECK(p4 + more_pages * growth_th < (size_t)YT_CONTINUATION_MAX,
              "YT_CONTINUATION_MAX %d does not carry page 4's %zu-char token "
              "and %zu more pages of %zu chars each", YT_CONTINUATION_MAX, p4,
              more_pages, growth_th);
    }

    /* The same page 2 without the app's fields filter: same rows, same token;
     * the filter only removes things the parser never reads. */
    n = page_of("fixtures/browse_home_visionos_page2_nofields.json", &page);
    CHECK(n == 62 && strlen(page.continuation) == 7498,
          "unfiltered page 2: %d rows, %zu-char token", n,
          strlen(page.continuation));

    /* What YouTube answers a token sent with the wrong visitor id (or none):
     * HTTP 200 and a shell with no rows and nothing to continue. */
    n = page_of("fixtures/browse_home_visionos_page2_empty.json", &page);
    printf("home page 2 (empty shell): %d videos, continuation=%zu chars\n", n,
           strlen(page.continuation));
    CHECK(n == 0, "the empty shell has no rows, got %d", n);
    CHECK(page.continuation[0] == '\0',
          "the empty shell carries only a reloadContinuationData, which is "
          "not a next page; got a %zu-char token", strlen(page.continuation));
}

/* A channel's uploads (browse VLUU... on the anonymous TV client): 15 tiles
 * and a 124-character token under playlistVideoListRenderer.continuations[];
 * the next page is continuationContents.playlistVideoListContinuation. */
static void check_channel_uploads(void) {
    static yt_video_list_t page;
    char title[80];
    size_t len;
    char *buf = slurp("fixtures/channel_uploads_tv.json", &len);
    int n = yt_parse_channel_page(buf, len, &page, title, sizeof title);
    free(buf);
    printf("channel uploads: %d videos, continuation=%zu chars\n", n,
           strlen(page.continuation));
    CHECK(n == 15, "uploads page 1 expected 15 tiles, got %d", n);
    CHECK(strlen(page.continuation) == 124,
          "uploads page 1 expected a 124-char token, got %zu",
          strlen(page.continuation));
    for (int i = 0; i < n; i++)
        CHECK(strlen(page.items[i].video_id) == 11 && page.items[i].title[0],
              "uploads item %d incomplete", i);

    n = page_of("fixtures/channel_uploads_tv_page2.json", &page);
    printf("channel uploads page 2: %d videos, continuation=%zu chars\n", n,
           strlen(page.continuation));
    CHECK(n == 15, "uploads page 2 expected 15 tiles, got %d", n);
    CHECK(strlen(page.continuation) == 124,
          "uploads page 2 expected a 124-char token, got %zu",
          strlen(page.continuation));
}

/* ---- token selection and the drop counters, on hand-built responses ----------- */
static int parse_str(const char *json, yt_video_list_t *out) {
    return yt_parse_video_list(json, strlen(json), out);
}

static void check_tokens_synthetic(void) {
    static yt_video_list_t page;
    const char *V = "{\"compactVideoRenderer\":{\"videoId\":\"AAAAAAAAAAA\","
                    "\"title\":{\"simpleText\":\"t\"}}}";
    char json[4096];

    /* The next page, then a reload: the first wins, the reload never does. */
    parse_str("{\"c\":{\"continuations\":[{\"nextContinuationData\":"
              "{\"continuation\":\"NEXT\"}},{\"reloadContinuationData\":"
              "{\"continuation\":\"RELOAD\"}}]}}", &page);
    CHECK(strcmp(page.continuation, "NEXT") == 0, "got '%s'", page.continuation);
    /* A reload on its own is not a next page. */
    parse_str("{\"c\":{\"continuations\":[{\"reloadContinuationData\":"
              "{\"continuation\":\"RELOAD\"}}]}}", &page);
    CHECK(page.continuation[0] == '\0', "reload taken as next page: '%s'",
          page.continuation);

    /* The web shape: continuationItemRenderer inside an
     * appendContinuationItemsAction, after the rows. */
    snprintf(json, sizeof json,
             "{\"onResponseReceivedActions\":[{\"appendContinuationItemsAction\":"
             "{\"continuationItems\":[%s,{\"continuationItemRenderer\":"
             "{\"continuationEndpoint\":{\"continuationCommand\":"
             "{\"token\":\"WEBTOKEN\"}}}}]}}]}", V);
    int n = parse_str(json, &page);
    CHECK(n == 1 && strcmp(page.continuation, "WEBTOKEN") == 0,
          "web shape: %d rows, token '%s'", n, page.continuation);

    /* The TV shape: tiles in a grid continuation. */
    parse_str("{\"continuationContents\":{\"gridContinuation\":{\"items\":[],"
              "\"continuations\":[{\"nextContinuationData\":"
              "{\"continuation\":\"GRIDTOKEN\"}}]}}}", &page);
    CHECK(strcmp(page.continuation, "GRIDTOKEN") == 0, "got '%s'",
          page.continuation);

    /* The size limit is exact: the longest token that fits is
     * YT_CONTINUATION_MAX-1 characters; one more is dropped, whole, and the
     * drop is on record. */
    for (int extra = 0; extra <= 1; extra++) {
        size_t tlen = (size_t)YT_CONTINUATION_MAX - 1 + (size_t)extra;
        char *big = malloc(tlen + 200);
        int w = snprintf(big, tlen + 200, "{\"c\":{\"continuations\":["
                         "{\"nextContinuationData\":{\"continuation\":\"");
        memset(big + w, 'T', tlen);
        snprintf(big + w + tlen, 100, "\"}},{\"nextContinuationData\":"
                 "{\"continuation\":\"LATER\"}}]}}");
        parse_str(big, &page);
        free(big);
        if (extra == 0) {
            CHECK(strlen(page.continuation) == tlen && page.continuation[tlen - 1] == 'T',
                  "a token of %zu chars fits and is kept whole, got %zu", tlen,
                  strlen(page.continuation));
            CHECK(yt_parse_last_dropped_continuation_len() == 0, "false drop");
        } else {
            /* Not truncated, and not replaced by the later token either:
             * that one continues something else. */
            CHECK(page.continuation[0] == '\0',
                  "an oversize token must leave none, got %zu chars '%.12s'",
                  strlen(page.continuation), page.continuation);
            CHECK(yt_parse_last_dropped_continuation_len() == (int)tlen,
                  "drop not recorded: %d", yt_parse_last_dropped_continuation_len());
        }
    }
    /* ...and the record is per parse, not sticky. */
    parse_str("{}", &page);
    CHECK(yt_parse_last_dropped_continuation_len() == 0 &&
          yt_parse_last_dropped_items() == 0, "drop counters not reset");

    /* Rows beyond YT_LIST_MAX are counted, and the first YT_LIST_MAX kept. */
    {
        size_t cap = (size_t)(YT_LIST_MAX + 3) * 90 + 64;
        char *rows = malloc(cap);
        size_t used = (size_t)snprintf(rows, cap, "{\"items\":[");
        for (int i = 0; i < YT_LIST_MAX + 3; i++)
            used += (size_t)snprintf(rows + used, cap - used,
                "%s{\"compactVideoRenderer\":{\"videoId\":\"V%010d\","
                "\"title\":{\"simpleText\":\"t\"}}}", i ? "," : "", i);
        snprintf(rows + used, cap - used, "]}");
        n = parse_str(rows, &page);
        free(rows);
        CHECK(n == YT_LIST_MAX, "expected the list to fill to %d, got %d",
              YT_LIST_MAX, n);
        CHECK(yt_parse_last_dropped_items() == 3, "expected 3 dropped rows, got %d",
              yt_parse_last_dropped_items());
    }
    CHECK(YT_SURF_NONE == 0, "a zeroed list must mean 'no surface'");
}

/* ---- the visitor id a continuation has to be sent with ------------------------- */
static void check_visitor(void) {
    char out[YT_VISITOR_MAX];
    size_t len;
    /* Only the unfiltered capture still has a responseContext. */
    char *buf = slurp("fixtures/browse_home_visionos_page2_nofields.json", &len);
    int n = yt_parse_visitor_data(buf, out, sizeof out);
    free(buf);
    CHECK(n == (int)strlen("REDACTED_VISITOR_DATA") &&
          strcmp(out, "REDACTED_VISITOR_DATA") == 0,
          "visitorData from the unfiltered page 2: %d '%s'", n, out);
    buf = slurp("fixtures/browse_home_visionos_page2.json", &len);
    n = yt_parse_visitor_data(buf, out, sizeof out);
    free(buf);
    CHECK(n == 0 && out[0] == '\0',
          "the filtered capture has no responseContext, got %d '%s'", n, out);

    CHECK(yt_parse_visitor_data("{\"responseContext\":{\"visitorData\":\"Cgt-x_9%3D%3D\"},"
                                "\"contents\":{}}", out, sizeof out) == 13 &&
          strcmp(out, "Cgt-x_9%3D%3D") == 0, "plain value: '%s'", out);
    /* First occurrence in document order. */
    CHECK(yt_parse_visitor_data("{\"visitorData\":\"FIRST\",\"x\":{\"visitorData\":\"SECOND\"}}",
                                out, sizeof out) == 5 && strcmp(out, "FIRST") == 0,
          "first wins: '%s'", out);
    /* Refused: absent, empty, escaped, unterminated, too long for the buffer. */
    CHECK(yt_parse_visitor_data("{}", out, sizeof out) == 0 && !out[0], "absent");
    CHECK(yt_parse_visitor_data("{\"visitorData\":\"\"}", out, sizeof out) == 0 && !out[0], "empty");
    CHECK(yt_parse_visitor_data("{\"visitorData\":\"ab\\u003dcd\"}", out, sizeof out) == 0 && !out[0], "escaped");
    CHECK(yt_parse_visitor_data("{\"visitorData\":\"abc", out, sizeof out) == 0 && !out[0], "unterminated");
    char small[4];
    CHECK(yt_parse_visitor_data("{\"visitorData\":\"abcd\"}", small, sizeof small) == 0 && !small[0], "too long");
    CHECK(yt_parse_visitor_data("{\"visitorData\":\"abc\"}", small, sizeof small) == 3 &&
          strcmp(small, "abc") == 0, "exact fit");
    CHECK(yt_parse_visitor_data(NULL, out, sizeof out) == 0 && !out[0], "NULL json");
}

/* ---- hostile and borderline input --------------------------------------------------- */
static char *repeat_rows(const char *key, int n, const char *between_rows_extra) {
    /* {"items":[ {<key>:{...}}, ... ]} with n rows. The rows are
     * compactVideoRenderers or tileRenderers, as `key` says. */
    size_t cap = (size_t)n * 260 + 512 + (between_rows_extra ? strlen(between_rows_extra) : 0);
    char *s = malloc(cap);
    size_t used = (size_t)snprintf(s, cap, "{\"items\":[");
    for (int i = 0; i < n; i++) {
        if (strcmp(key, "tileRenderer") == 0)
            used += (size_t)snprintf(s + used, cap - used,
                "%s{\"tileRenderer\":{\"onSelectCommand\":{\"watchEndpoint\":{\"videoId\":\"V%010d\"}},"
                "\"metadata\":{\"tileMetadataRenderer\":{\"title\":{\"simpleText\":\"t\"}}}}}", i ? "," : "", i);
        else
            used += (size_t)snprintf(s + used, cap - used,
                "%s{\"compactVideoRenderer\":{\"videoId\":\"V%010d\",\"title\":{\"simpleText\":\"t\"}}}",
                i ? "," : "", i);
    }
    snprintf(s + used, cap - used, "]%s}", between_rows_extra ? between_rows_extra : "");
    return s;
}

static void check_row_limit_exact(void) {
    static yt_video_list_t page;
    const char *keys[] = { "compactVideoRenderer", "tileRenderer" };
    for (int k = 0; k < 2; k++) {
        for (int extra = -1; extra <= 1; extra++) {
            int n = YT_LIST_MAX + extra;
            char *js = repeat_rows(keys[k], n, NULL);
            int got = parse_str(js, &page);
            free(js);
            CHECK(got == (n > YT_LIST_MAX ? YT_LIST_MAX : n),
                  "%s: %d rows in, %d out (cap %d)", keys[k], n, got, YT_LIST_MAX);
            CHECK(yt_parse_last_dropped_items() == (n > YT_LIST_MAX ? n - YT_LIST_MAX : 0),
                  "%s: %d rows in, %d counted as dropped", keys[k], n, yt_parse_last_dropped_items());
        }
    }
}

/* A row past the cap is counted and not walked into: nothing inside it can be
 * taken for the page's own token, even when it comes before the real one in
 * document order. (A whole row of a TV tile or a video renderer carries its
 * own continuation-shaped command objects in real responses.) */
static void check_token_inside_dropped_row(void) {
    static yt_video_list_t page;
    const char *keys[] = { "compactVideoRenderer", "tileRenderer" };
    for (int k = 0; k < 2; k++) {
        /* YT_LIST_MAX rows to fill the list, one more carrying a nested
         * nextContinuationData, then the page's real token. */
        size_t cap = (size_t)(YT_LIST_MAX + 1) * 260 + 1024;
        char *js = malloc(cap);
        char *rows = repeat_rows(keys[k], YT_LIST_MAX, NULL);
        /* cut the closing "]}" of repeat_rows and append the extra row */
        rows[strlen(rows) - 2] = '\0';
        snprintf(js, cap,
            "%s,{\"%s\":{\"nextContinuationData\":{\"continuation\":\"NESTED\"},"
            "\"videoId\":\"Z%010d\",\"onSelectCommand\":{\"watchEndpoint\":{\"videoId\":\"Z%010d\"}},"
            "\"title\":{\"simpleText\":\"t\"}}}],"
            "\"continuations\":[{\"nextContinuationData\":{\"continuation\":\"REAL\"}}]}",
            rows, keys[k], 0, 0);
        free(rows);
        int n = parse_str(js, &page);
        free(js);
        CHECK(n == YT_LIST_MAX, "%s: %d rows", keys[k], n);
        CHECK(yt_parse_last_dropped_items() == 1, "%s: dropped %d", keys[k], yt_parse_last_dropped_items());
        CHECK(strcmp(page.continuation, "REAL") == 0,
              "%s: a token inside a dropped row must not be the page's: '%s'", keys[k], page.continuation);
    }
}

/* The parse entry points reset the drop record, whichever one ran last. */
static void check_drop_record_reset(void) {
    static yt_video_list_t page;
    static yt_channel_list_t chans;
    size_t tlen = (size_t)YT_CONTINUATION_MAX + 10;
    char *big = malloc(tlen + 200);
    int w = snprintf(big, tlen + 200, "{\"c\":{\"continuations\":[{\"nextContinuationData\":{\"continuation\":\"");
    memset(big + w, 'T', tlen);
    snprintf(big + w + tlen, 100, "\"}}]}}");
    const char *empty = "{}";
    char title[40];
    for (int which = 0; which < 4; which++) {
        parse_str(big, &page);
        CHECK(yt_parse_last_dropped_continuation_len() == (int)tlen, "setup: %d", yt_parse_last_dropped_continuation_len());
        switch (which) {
        case 0: yt_parse_video_list(empty, strlen(empty), &page); break;
        case 1: yt_parse_search(empty, strlen(empty), &page, &chans); break;
        case 2: yt_parse_channel_page(empty, strlen(empty), &page, title, sizeof title); break;
        default: yt_parse_channel_list(empty, strlen(empty), &chans); break;
        }
        CHECK(yt_parse_last_dropped_continuation_len() == 0,
              "entry point %d did not reset the dropped-token record: %d", which, yt_parse_last_dropped_continuation_len());
    }
    /* ...and each of them records its own drop. */
    yt_parse_search(big, strlen(big), &page, &chans);
    CHECK(yt_parse_last_dropped_continuation_len() == (int)tlen, "search: %d", yt_parse_last_dropped_continuation_len());
    yt_parse_channel_page(big, strlen(big), &page, title, sizeof title);
    CHECK(yt_parse_last_dropped_continuation_len() == (int)tlen, "channel page: %d", yt_parse_last_dropped_continuation_len());
    yt_parse_channel_list(big, strlen(big), &chans);
    CHECK(yt_parse_last_dropped_continuation_len() == (int)tlen, "channel list: %d", yt_parse_last_dropped_continuation_len());
    free(big);
}

/* The subscribed-channels list takes the same token rules as the video lists:
 * the next page, not a reload; whole or not at all. */
static void check_channel_list_token(void) {
    static yt_channel_list_t chans;
    const char *ch = "{\"tileRenderer\":{\"contentType\":\"TILE_CONTENT_TYPE_CHANNEL\","
                     "\"onSelectCommand\":{\"browseEndpoint\":{\"browseId\":\"UCabcdefghijklmnopqrstuv\"}},"
                     "\"metadata\":{\"tileMetadataRenderer\":{\"title\":{\"simpleText\":\"Chan\"}}}}}";
    char js[2048];
    snprintf(js, sizeof js, "{\"items\":[%s],\"continuations\":[{\"nextContinuationData\":{\"continuation\":\"CHNEXT\"}},"
             "{\"reloadContinuationData\":{\"continuation\":\"CHRELOAD\"}}]}", ch);
    yt_parse_channel_list(js, strlen(js), &chans);
    CHECK(strcmp(chans.continuation, "CHNEXT") == 0, "channel list token '%s'", chans.continuation);
    snprintf(js, sizeof js, "{\"items\":[%s],\"continuations\":[{\"reloadContinuationData\":{\"continuation\":\"CHRELOAD\"}}]}", ch);
    yt_parse_channel_list(js, strlen(js), &chans);
    CHECK(chans.continuation[0] == '\0', "a reload is not a next page: '%s'", chans.continuation);
    snprintf(js, sizeof js, "{\"items\":[%s],\"x\":{\"continuationCommand\":{\"token\":\"CHCMD\"}}}", ch);
    yt_parse_channel_list(js, strlen(js), &chans);
    CHECK(strcmp(chans.continuation, "CHCMD") == 0, "continuationCommand token '%s'", chans.continuation);
}

/* yt_parse_visitor_data on input it was never meant for. */
static void check_visitor_hostile(void) {
    char out[YT_VISITOR_MAX];
    char small[8];
    /* No quotes at all, a key without a value, a value that never closes. */
    CHECK(yt_parse_visitor_data("", out, sizeof out) == 0 && !out[0], "empty string");
    CHECK(yt_parse_visitor_data("visitorData", out, sizeof out) == 0 && !out[0], "bare word");
    CHECK(yt_parse_visitor_data("\"visitorData\"", out, sizeof out) == 0 && !out[0], "key only");
    CHECK(yt_parse_visitor_data("\"visitorData\":", out, sizeof out) == 0 && !out[0], "key and colon only");
    CHECK(yt_parse_visitor_data("\"visitorData\":\"", out, sizeof out) == 0 && !out[0], "opening quote only");
    CHECK(yt_parse_visitor_data("\"visitorData\":null", out, sizeof out) == 0 && !out[0], "null value");
    CHECK(yt_parse_visitor_data("\"visitorData\":12", out, sizeof out) == 0 && !out[0], "number value");
    CHECK(yt_parse_visitor_data("\"visitorData\": \"abc\"", out, sizeof out) == 0 && !out[0],
          "a space after the colon is not the shape YouTube sends (compact JSON): refused, not guessed");
    /* Every truncation of a good response is either refused or an exact prefix-free answer: never a partial id. */
    const char *good = "{\"responseContext\":{\"visitorData\":\"CgtAbC-_%3D\",\"x\":1}}";
    size_t glen = strlen(good);
    for (size_t cut = 0; cut <= glen; cut++) {
        char part[128];
        memcpy(part, good, cut);
        part[cut] = '\0';
        int n = yt_parse_visitor_data(part, out, sizeof out);
        if (n != 0)
            CHECK(strcmp(out, "CgtAbC-_%3D") == 0, "truncated at %zu gave the partial id '%s'", cut, out);
        else
            CHECK(out[0] == '\0', "refused at %zu but out is '%s'", cut, out);
    }
    CHECK(yt_parse_visitor_data(good, out, sizeof out) == 11, "the good one is 11 chars");
    /* A value as long as the buffer can hold, and one more. */
    {
        size_t cap = YT_VISITOR_MAX;
        char *js = malloc(cap + 64);
        for (int extra = 0; extra <= 1; extra++) {
            size_t vlen = cap - 1 + (size_t)extra;
            int w = sprintf(js, "{\"visitorData\":\"");
            memset(js + w, 'A', vlen);
            strcpy(js + w + vlen, "\"}");
            int n = yt_parse_visitor_data(js, out, cap);
            if (extra == 0)
                CHECK(n == (int)vlen && strlen(out) == vlen, "a %zu-char id fits YT_VISITOR_MAX: got %d", vlen, n);
            else
                CHECK(n == 0 && out[0] == '\0', "a %zu-char id does not fit: got %d", vlen, n);
        }
        free(js);
    }
    /* cap 0 and 1: nothing is written past what was offered. */
    memset(small, 'x', sizeof small);
    CHECK(yt_parse_visitor_data("{\"visitorData\":\"a\"}", small, 0) == 0 && small[0] == 'x', "cap 0 leaves out alone");
    CHECK(yt_parse_visitor_data("{\"visitorData\":\"a\"}", small, 1) == 0 && small[0] == '\0', "cap 1 holds no id");
    CHECK(yt_parse_visitor_data("{\"visitorData\":\"a\"}", NULL, 8) == 0, "NULL out");
    /* Escapes: refused whole, wherever they sit. */
    CHECK(yt_parse_visitor_data("{\"visitorData\":\"ab\\\"cd\"}", out, sizeof out) == 0 && !out[0], "escaped quote");
    CHECK(yt_parse_visitor_data("{\"visitorData\":\"ab\\\\cd\"}", out, sizeof out) == 0 && !out[0], "escaped backslash");
    CHECK(yt_parse_visitor_data("{\"visitorData\":\"\\u0041\"}", out, sizeof out) == 0 && !out[0], "unicode escape");
    CHECK(yt_parse_visitor_data("{\"visitorData\":\"abc\\", out, sizeof out) == 0 && !out[0], "backslash at the end");
    /* An embedded NUL ends the C string: an unterminated value, refused. */
    {
        const char raw[] = "{\"visitorData\":\"ab\0cd\"}";
        CHECK(yt_parse_visitor_data(raw, out, sizeof out) == 0 && !out[0], "embedded NUL");
    }
    /* The key inside another string: JSON escapes the quotes there, so what
     * a title can say never has the shape of the field. The real one, later,
     * is found. */
    CHECK(yt_parse_visitor_data("{\"contents\":[{\"title\":{\"simpleText\":\"x \\\"visitorData\\\":\\\"EVIL\\\" y\"}}],"
                                "\"responseContext\":{\"visitorData\":\"GOOD\"}}", out, sizeof out) == 4 &&
          strcmp(out, "GOOD") == 0, "visitorData quoted inside a title: '%s'", out);
    /* The word without the quote after it is no key. */
    CHECK(yt_parse_visitor_data("{\"t\":\"my visitorData:\\\"x\\\"\"}", out, sizeof out) == 0 && !out[0], "the word in a title");
    /* An earlier non-string field of that name stops the scan: the search is
     * for the string shape, nothing else is looked at. */
    CHECK(yt_parse_visitor_data("{\"visitorData\":{\"x\":1},\"responseContext\":{\"visitorData\":\"GOOD\"}}", out, sizeof out) == 4 &&
          strcmp(out, "GOOD") == 0, "an object-valued visitorData is skipped: '%s'", out);
}

/* The parsers on truncated and garbage JSON: an error or an answer, never a
 * crash (the sanitizer run is what makes this a test), and never a token
 * from a response that was cut. */
static void check_parser_garbage(void) {
    static yt_video_list_t page;
    static yt_channel_list_t chans;
    char title[40];
    size_t len;
    char *buf = slurp("fixtures/channel_uploads_tv.json", &len);
    int parsed = 0, refused = 0;
    for (size_t cut = 0; cut < len; cut += (cut < 3000 ? 7 : 811)) {
        int n = yt_parse_video_list(buf, cut, &page);
        if (n < 0) refused++; else parsed++;
        CHECK(n >= 0 ? page.count == n : page.count == 0 && page.continuation[0] == '\0',
              "cut at %zu: %d rows, count %d token '%.8s'", cut, n, page.count, page.continuation);
        yt_parse_search(buf, cut, &page, &chans);
        yt_parse_channel_page(buf, cut, &page, title, sizeof title);
        yt_parse_channel_list(buf, cut, &chans);
    }
    CHECK(refused > 100, "a cut response is refused: %d of %d", refused, refused + parsed);
    /* The complete one still parses to what it always did. */
    CHECK(yt_parse_video_list(buf, len, &page) >= 0 && page.count == 15, "whole response: %d rows", page.count);
    free(buf);

    const char *junk[] = { "", "\"", "}", "{", "[", "null", "true", "12", "\"x\"", "[[[[", "{\"a\":", "{\"a\":}",
                           "{\"contents\":[{\"compactVideoRenderer\":null}]}",
                           "{\"contents\":[{\"compactVideoRenderer\":{\"videoId\":5,\"title\":7}}]}",
                           "{\"continuations\":[{\"nextContinuationData\":null}]}",
                           "{\"continuations\":[{\"nextContinuationData\":{\"continuation\":5}}]}",
                           "{\"continuations\":[{\"nextContinuationData\":{\"continuation\":null}}]}",
                           "{\"continuationCommand\":{\"token\":[]}}",
                           "\xff\xfe\x00\x01" };
    for (size_t i = 0; i < sizeof junk / sizeof *junk; i++) {
        size_t jl = i == sizeof junk / sizeof *junk - 1 ? 4 : strlen(junk[i]);
        int n = yt_parse_video_list(junk[i], jl, &page);
        CHECK(n <= 0 && page.count == 0 && page.continuation[0] == '\0', "junk %zu: %d rows, token '%s'", i, n, page.continuation);
        yt_parse_search(junk[i], jl, &page, &chans);
        yt_parse_channel_page(junk[i], jl, &page, title, sizeof title);
        yt_parse_channel_list(junk[i], jl, &chans);
    }
    /* No body at all: an HTTP 200 with a zero-length answer leaves the buffer
     * pointer NULL. */
    CHECK(yt_parse_video_list(NULL, 0, &page) == -1, "NULL, 0");
    /* Nesting deeper than the JSON library allows is refused, not recursed into. */
    {
        size_t depth = 200000;
        char *deep = malloc(depth + 1);
        memset(deep, '[', depth);
        deep[depth] = '\0';
        CHECK(yt_parse_video_list(deep, depth, &page) == -1, "200000 levels of [");
        free(deep);
    }
}

/* A page that is nothing but repeats of rows the list already has parses to
 * the same rows: de-duplicating is the list's job (list_screen.c), so the
 * parser must hand over every row, in order. */
static void check_duplicates_pass_through(void) {
    static yt_video_list_t page;
    const char *row = "{\"compactVideoRenderer\":{\"videoId\":\"AAAAAAAAAAA\",\"title\":{\"simpleText\":\"t\"}}}";
    char js[1024];
    snprintf(js, sizeof js, "{\"items\":[%s,%s,%s]}", row, row, row);
    int n = parse_str(js, &page);
    CHECK(n == 3, "three identical rows parse to %d", n);
}

int main(void) {
    size_t len;
    char *buf;
    yt_video_list_t list;

    buf = slurp("fixtures/search_android.json", &len);
    int n = yt_parse_video_list(buf, len, &list);
    free(buf);
    printf("search: %d videos, continuation=%zu chars\n", n, strlen(list.continuation));
    CHECK(n >= 10, "search expected >=10 videos, got %d", n);
    CHECK(list.continuation[0], "search expected a continuation token");
    for (int i = 0; i < n; i++) {
        CHECK(strlen(list.items[i].video_id) == 11, "item %d bad id '%s'", i, list.items[i].video_id);
        CHECK(list.items[i].title[0], "item %d empty title", i);
        CHECK(list.items[i].thumb_url[0], "item %d no thumbnail", i);
    }
    printf("  [0] %s | %s | %s | %s | dur=%s\n    thumb=%.60s\n",
           list.items[0].video_id, list.items[0].title, list.items[0].channel,
           list.items[0].views, list.items[0].duration, list.items[0].thumb_url);

    buf = slurp("fixtures/next_android.json", &len);
    n = yt_parse_video_list(buf, len, &list);
    free(buf);
    printf("next/related: %d videos\n", n);
    CHECK(n >= 5, "related expected >=5 videos, got %d", n);

    yt_stream_t st;
    buf = slurp("fixtures/player_android.json", &len);
    int rc = yt_parse_player(buf, len, &st);
    free(buf);
    CHECK(rc == 0, "player parse failed");
    CHECK(strcmp(st.playability, "OK") == 0, "playability %s", st.playability);
    CHECK(st.format_count >= 1, "expected at least one progressive format");
    yt_format_t *best = &st.formats[st.selected];
    printf("player: rc=%d playability=%s formats=%d best itag=%d %dx%d len=%ds\n"
           "  title=%s\n  url=%.70s...\n",
           rc, st.playability, st.format_count, best->itag, best->width,
           best->height, st.length_seconds, st.title, best->url);
    CHECK(best->itag == 18, "expected best itag 18, got %d", best->itag);
    CHECK(strstr(best->url, "https://") == best->url, "bad stream url");
    CHECK(st.length_seconds > 0, "no length");
    CHECK(st.is_live == 0, "an ordinary video is not live");
    CHECK(st.hls_manifest[0] == '\0',
          "an ordinary video resolves its HD manifest separately; carrying "
          "this client's one would take the player down the live path");
    /* formats must be sorted descending by height */
    for (int i = 1; i < st.format_count; i++)
        CHECK(st.formats[i - 1].height >= st.formats[i].height,
             "formats not sorted descending at %d", i);

    /* A Short. Portrait, so its progressive format is 360x640 — YouTube's
     * own qualityLabel calls that 360p. player_screen.c used to decide
     * "the direct format is already as good as HLS" by comparing `height`
     * against a threshold, and 640 cleared every bar, so Shorts played 360p
     * no matter what the quality setting said. The assertions below pin the
     * exact shape that fooled it, rather than the resolution of the day. */
    buf = slurp("fixtures/player_short_android.json", &len);
    yt_stream_t sh;
    int short_rc = yt_parse_player(buf, len, &sh);
    free(buf);
    const yt_format_t *sf = &sh.formats[sh.selected];
    printf("short: rc=%d formats=%d best itag=%d %dx%d\n", short_rc,
           sh.format_count, sf->itag, sf->width, sf->height);
    CHECK(short_rc == 0 && sh.format_count > 0, "short did not resolve");
    CHECK(sf->width > 0 && sf->width < sf->height,
          "expected a portrait format, got %dx%d", sf->width, sf->height);
    CHECK(sf->height > 360 && sf->width <= 360,
          "this fixture only guards the height-vs-short-side trap if its "
          "height clears 360 while its short side does not; got %dx%d",
          sf->width, sf->height);

    /* A live stream. It has no progressive format at all — the manifest in
     * this same response is the only way to play it, which is why the parser
     * keeps it and why the player stops asking the HD preference first. */
    buf = slurp("fixtures/player_live_android.json", &len);
    yt_stream_t live;
    int live_rc = yt_parse_player(buf, len, &live);
    free(buf);
    printf("live: rc=%d playability=%s is_live=%d formats=%d len=%ds "
           "manifest=%zu chars\n",
           live_rc, live.playability, live.is_live, live.format_count,
           live.length_seconds, strlen(live.hls_manifest));
    CHECK(live_rc == 0, "live player response did not parse");
    CHECK(strcmp(live.playability, "OK") == 0, "live stream not playable: %s",
          live.playability);
    CHECK(live.is_live == 1, "videoDetails.isLive was not picked up");
    CHECK(live.format_count == 0,
          "expected no progressive format for a live stream, got %d",
          live.format_count);
    CHECK(live.length_seconds == 0, "a live stream has no length yet");
    CHECK(strstr(live.hls_manifest, "https://") == live.hls_manifest &&
          strstr(live.hls_manifest, ".m3u8"),
          "expected an HLS manifest URL, got '%.60s'", live.hls_manifest);
    /* Muxed and short-windowed: the two properties the live player path is
     * built on (media/hls.h). The on-demand manifest is neither, which is why
     * it is resolved from a different client. */
    CHECK(strstr(live.hls_manifest, "/demuxed/1/") == NULL,
          "this client's live manifest is supposed to be muxed");
    CHECK(strstr(live.hls_manifest, "/playlist_duration/30/") != NULL,
          "expected the 30-second window; a longer one makes every reload "
          "cost most of a megabyte");

    /* The anonymous home feed, captured from the VISIONOS client — the
     * surface a signed-out user now lands on (see yt_home_feed). It arrives
     * as compactVideoRenderer: 76 of them (a page is 66-84 rows, randomised
     * per call) and a 4482-character next-page token, beside a
     * reloadContinuationData that is NOT the next page. Both numbers once
     * went wrong without a sound: YT_LIST_MAX of 60 cut the last 16 rows of
     * every page, and YT_CONTINUATION_MAX of 2048 dropped the token, so Start
     * could never load more. Every row is fully populated here; a regression
     * that drops the channel name or duration would still leave video_id
     * intact, which is why those are checked individually. */
    buf = slurp("fixtures/browse_home_visionos.json", &len);
    list.surface = YT_SURF_SEARCH; /* the parser must clear it, not inherit it */
    n = yt_parse_video_list(buf, len, &list);
    free(buf);
    printf("home (anonymous, visionos): %d videos, continuation=%zu chars\n",
           n, strlen(list.continuation));
    CHECK(n == 76, "anonymous home expected all 76 rows, got %d", n);
    CHECK(strlen(list.continuation) == 4482,
          "anonymous home expected the 4482-char next-page token, got %zu "
          "chars (the 8304-char reloadContinuationData must never win)",
          strlen(list.continuation));
    CHECK(yt_parse_last_dropped_continuation_len() == 0,
          "token dropped: %d chars", yt_parse_last_dropped_continuation_len());
    CHECK(yt_parse_last_dropped_items() == 0, "%d rows dropped",
          yt_parse_last_dropped_items());
    CHECK(list.surface == YT_SURF_NONE,
          "the parser does not know the request; innertube.c stamps the surface");
    CHECK(list.visitor[0] == '\0', "the parser does not fill the visitor id");
    for (int i = 0; i < n; i++) {
        CHECK(strlen(list.items[i].video_id) == 11, "home item %d bad id '%s'",
             i, list.items[i].video_id);
        CHECK(list.items[i].title[0], "home item %d has no title", i);
        CHECK(list.items[i].thumb_url[0], "home item %d has no thumb_url", i);
        CHECK(list.items[i].channel[0], "home item %d has no channel", i);
        CHECK(list.items[i].duration[0], "home item %d has no duration", i);
    }

    buf = slurp("fixtures/channel_videos_tv.json", &len);
    char channel_title[80];
    n = yt_parse_channel_page(buf, len, &list, channel_title, sizeof channel_title);
    free(buf);
    printf("channel page: %d videos, title=%s\n", n, channel_title);
    CHECK(n >= 10, "channel page expected >=10 videos, got %d", n);
    CHECK(channel_title[0], "channel page missing title");
    for (int i = 0; i < n; i++) {
        CHECK(strlen(list.items[i].video_id) == 11, "channel item %d bad id '%s'",
             i, list.items[i].video_id);
        CHECK(list.items[i].thumb_url[0], "channel item %d (tileRenderer) has no thumb_url", i);
    }

    check_home_pages();
    check_channel_uploads();
    check_tokens_synthetic();
    check_visitor();
    check_visitor_hostile();
    check_row_limit_exact();
    check_token_inside_dropped_row();
    check_drop_record_reset();
    check_channel_list_token();
    check_parser_garbage();
    check_duplicates_pass_through();

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL PARSER TESTS PASSED\n");
    return 0;
}
