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
     * as compactVideoRenderer and carries no continuation, so the whole feed
     * is this one page. Every row is fully populated here; a regression that
     * drops the channel name or duration would still leave video_id intact,
     * which is why those are checked individually. */
    buf = slurp("fixtures/browse_home_visionos.json", &len);
    n = yt_parse_video_list(buf, len, &list);
    free(buf);
    printf("home (anonymous, visionos): %d videos, continuation=%zu chars\n",
           n, strlen(list.continuation));
    CHECK(n >= 50, "anonymous home expected >=50 videos, got %d", n);
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

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL PARSER TESTS PASSED\n");
    return 0;
}
