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

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL PARSER TESTS PASSED\n");
    return 0;
}
