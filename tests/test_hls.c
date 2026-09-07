/* Host-side tests for the HLS source (src/media/hls.c) against a scripted
 * server (tests/stubs/ replaces vita-https and the two psp2 headers).
 *
 * The live half of this is what needs the harness. A live playlist is a
 * window that moves: it names a handful of segments, drops the oldest and
 * gains a new one every few seconds, and never says it has ended. Everything
 * that follows from that — re-fetching it before FFmpeg asks again, keeping a
 * segment addressable after the window renumbered itself, downloading a muxed
 * segment once and handing it to both cursors — is invisible on a build
 * machine and expensive to debug on a console. Here the server is a variable
 * and the clock is a variable, so all of it is a plain assertion. */
#include "media/hls.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "vita_https.h"

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

/* --- the platform the module thinks it is running on --------------------- */

/* Driven by hand: a reload becomes due because the test says so, not because
 * it sat through two and a half seconds. */
static _Atomic uint64_t g_now_us = 1000000;

uint64_t sceKernelGetProcessTimeWide(void) { return g_now_us; }
SceUID sceKernelGetThreadId(void) { return 1; }
int sceKernelChangeThreadPriority(SceUID thid, int priority) {
    (void)thid; (void)priority; return 0;
}
int sceKernelDelayThread(uint32_t usec) { usleep(usec); return 0; }

static int g_verbose;
void vt_log(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list args;
    va_start(args, fmt);
    fputs("    log: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

/* --- the server ---------------------------------------------------------- */

#define PLAYLIST_URL "https://live.example/hls_playlist/index.m3u8"
#define SEQ_BASE 1000
#define WINDOW 6

static pthread_mutex_t g_server_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_head_seq = SEQ_BASE + 40; /* newest segment produced */
static int g_window = WINDOW;          /* how many segments a window names */
static int g_ended;                            /* emit #EXT-X-ENDLIST */
static int g_playlist_fetches;
static int g_seg_fetches[4096];
/* Concurrency watermark: how many fetches were on the wire at once. The
 * segment handler sleeps briefly while "transferring", so two workers that
 * really run in parallel are caught overlapping here. */
static int g_inflight, g_inflight_max;

static int seg_fetches(int seq) {
    pthread_mutex_lock(&g_server_lock);
    int n = g_seg_fetches[seq - SEQ_BASE];
    pthread_mutex_unlock(&g_server_lock);
    return n;
}

/* The window as it stands right now, in the shape YouTube serves it. */
static char *build_playlist(void) {
    int first = g_head_seq - g_window + 1;
    size_t cap = 4096;
    char *text = malloc(cap);
    int len = snprintf(text, cap,
                       "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:5\n"
                       "#EXT-X-MEDIA-SEQUENCE:%d\n", first);
    for (int seq = first; seq <= g_head_seq; seq++)
        len += snprintf(text + len, cap - (size_t)len,
                        "#EXTINF:5.0,\nhttps://seg.example/sq/%d/seg.ts\n", seq);
    if (g_ended) snprintf(text + len, cap - (size_t)len, "#EXT-X-ENDLIST\n");
    return text;
}

VitaHttpsClient *vita_https_client_create(const VitaHttpsClientConfig *config) {
    (void)config;
    return (VitaHttpsClient *)&g_server_lock; /* any non-NULL handle will do */
}

void vita_https_client_destroy(VitaHttpsClient *client) { (void)client; }

int vita_https_perform(VitaHttpsClient *client, const VitaHttpsRequest *request,
                       VitaHttpsResponse *response) {
    (void)client;
    response->status_code = 200;
    if (strcmp(request->url, PLAYLIST_URL) == 0) {
        pthread_mutex_lock(&g_server_lock);
        g_playlist_fetches++;
        char *body = build_playlist();
        pthread_mutex_unlock(&g_server_lock);
        request->write(body, strlen(body), request->write_opaque);
        free(body);
        return 0;
    }
    int seq = -1;
    if (sscanf(request->url, "https://seg.example/sq/%d/seg.ts", &seq) == 1) {
        pthread_mutex_lock(&g_server_lock);
        /* Past the head is what a live edge answers before the encoder has
         * caught up; everything already produced stays available. */
        int available = seq <= g_head_seq && seq >= SEQ_BASE;
        if (available) {
            g_seg_fetches[seq - SEQ_BASE]++;
            if (++g_inflight > g_inflight_max) g_inflight_max = g_inflight;
        }
        pthread_mutex_unlock(&g_server_lock);
        if (!available) {
            response->status_code = 404;
            return -4;
        }
        usleep(40000); /* a segment takes time on the wire; see g_inflight
                        * and test_readahead_alive's serve-latency bound */
        char body[64];
        int n = snprintf(body, sizeof body, "SEGMENT-%d-PAYLOAD", seq);
        request->write(body, (size_t)n, request->write_opaque);
        pthread_mutex_lock(&g_server_lock);
        g_inflight--;
        pthread_mutex_unlock(&g_server_lock);
        return 0;
    }
    response->status_code = 404;
    return -4;
}

/* --- helpers ------------------------------------------------------------- */

/* One fetch through a cursor, as the demuxer's io_open would make it. The
 * returned buffer is the caller's, exactly as it is for FFmpeg. */
static char *fetch_through(VtDecoderStreamHandle *handle, const char *url,
                           size_t *len_out) {
    unsigned char *data = NULL;
    size_t size = 0;
    if (handle->fetch(handle->opaque, url, &data, &size) < 0) return NULL;
    char *text = malloc(size + 1);
    memcpy(text, data, size);
    text[size] = '\0';
    free(data);
    if (len_out) *len_out = size;
    return text;
}

/* Real wall clock, unlike the virtual one the module ages its playlist
 * against: used to assert that a cursor call never sits on the network. */
static uint64_t wall_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static void seg_url(char *buf, size_t cap, int seq) {
    /* file:, not https:: the demuxer only ever sees the rewritten scheme. */
    snprintf(buf, cap, "file://seg.example/sq/%d/seg.ts", seq);
}

/* --- masters ------------------------------------------------------------- */

/* What YouTube's live manifest looks like: muxed variants, no audio group. */
static const char MASTER_MUXED[] =
    "#EXTM3U\n"
    "#EXT-X-INDEPENDENT-SEGMENTS\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=546239,CODECS=\"mp4a.40.5,avc1.4D4015\","
        "RESOLUTION=426x240,FRAME-RATE=30,CLOSED-CAPTIONS=NONE\n"
    "https://live.example/240/index.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2969452,CODECS=\"mp4a.40.2,avc1.4D401F\","
        "RESOLUTION=1280x720,FRAME-RATE=30,CLOSED-CAPTIONS=NONE\n"
    "https://live.example/720/index.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=5420722,CODECS=\"mp4a.40.2,avc1.640028\","
        "RESOLUTION=1920x1080,FRAME-RATE=30,CLOSED-CAPTIONS=NONE\n"
    "https://live.example/1080/index.m3u8\n";

/* A live news channel: 720p only exists at 60 fps, and the best 30 fps
 * variant is 480p. Copied from Sky News' real master (2026-09-01). */
static const char MASTER_HIGH_FPS[] =
    "#EXTM3U\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1209862,CODECS=\"mp4a.40.2,avc1.4D401E\","
        "RESOLUTION=640x360,FRAME-RATE=30,CLOSED-CAPTIONS=NONE\n"
    "https://live.example/360/index.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1568726,CODECS=\"mp4a.40.2,avc1.4D401F\","
        "RESOLUTION=854x480,FRAME-RATE=30,CLOSED-CAPTIONS=NONE\n"
    "https://live.example/480/index.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=4370178,CODECS=\"mp4a.40.2,avc1.4D4020\","
        "RESOLUTION=1280x720,FRAME-RATE=60,CLOSED-CAPTIONS=NONE\n"
    "https://live.example/720/index.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=7171631,CODECS=\"mp4a.40.2,avc1.64002A\","
        "RESOLUTION=1920x1080,FRAME-RATE=60,CLOSED-CAPTIONS=NONE\n"
    "https://live.example/1080/index.m3u8\n";

/* Every variant above the cap: the cap must not turn into "unplayable". */
static const char MASTER_ONLY_FAST[] =
    "#EXTM3U\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=4370178,CODECS=\"mp4a.40.2,avc1.4D4020\","
        "RESOLUTION=1280x720,FRAME-RATE=60,CLOSED-CAPTIONS=NONE\n"
    "https://live.example/720/index.m3u8\n";

/* And what the on-demand one looks like: separate audio renditions. */
static const char MASTER_DEMUXED[] =
    "#EXTM3U\n"
    "#EXT-X-MEDIA:URI=\"https://vod.example/audio-en/index.m3u8\",TYPE=AUDIO,"
        "GROUP-ID=\"234\",NAME=\"American English - dubbed-auto\",DEFAULT=NO\n"
    "#EXT-X-MEDIA:URI=\"https://vod.example/audio-de/index.m3u8\",TYPE=AUDIO,"
        "GROUP-ID=\"234\",NAME=\"Deutsch (Deutschland) - original\",DEFAULT=NO\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2969452,CODECS=\"avc1.4D401F,mp4a.40.2\","
        "RESOLUTION=1280x720,FRAME-RATE=30,AUDIO=\"234\"\n"
    "https://vod.example/720/index.m3u8\n";

static void test_pick(void) {
    char video[4096], audio[4096];

    CHECK(vt_hls_pick(MASTER_MUXED, 720, 0, video, sizeof video, audio,
                      sizeof audio) == 0,
          "muxed master should yield a variant");
    CHECK(strcmp(video, "https://live.example/720/index.m3u8") == 0,
          "muxed: picked '%s', wanted the 720p variant", video);
    CHECK(audio[0] == '\0',
          "muxed: expected no separate audio playlist, got '%s'", audio);

    /* The frame-rate cap outranks resolution: a live news channel offers
     * 720p60 and 480p30 and nothing in between, and this decoder holds 720p60
     * together only by dropping frames. */
    CHECK(vt_hls_pick(MASTER_HIGH_FPS, 720, 30, video, sizeof video, audio,
                      sizeof audio) == 0,
          "high-fps master should still yield a variant");
    CHECK(strcmp(video, "https://live.example/480/index.m3u8") == 0,
          "capped at 30 fps: picked '%s', wanted the 480p30 variant", video);
    CHECK(vt_hls_pick(MASTER_HIGH_FPS, 720, 0, video, sizeof video, audio,
                      sizeof audio) == 0 &&
          strcmp(video, "https://live.example/720/index.m3u8") == 0,
          "without a cap the same master must still give 720p60, got '%s'",
          video);
    /* Nothing under the cap at all: take the fast one rather than nothing. */
    CHECK(vt_hls_pick(MASTER_ONLY_FAST, 720, 30, video, sizeof video, audio,
                      sizeof audio) == 0 &&
          strcmp(video, "https://live.example/720/index.m3u8") == 0,
          "a master with only 60 fps variants must still play, got '%s'",
          video);

    CHECK(vt_hls_pick(MASTER_DEMUXED, 720, 0, video, sizeof video, audio,
                      sizeof audio) == 0,
          "demuxed master should yield a variant");
    CHECK(strcmp(video, "https://vod.example/720/index.m3u8") == 0,
          "demuxed: picked '%s'", video);
    CHECK(strcmp(audio, "https://vod.example/audio-de/index.m3u8") == 0,
          "demuxed: expected the original-language rendition, got '%s'", audio);
    printf("pick: muxed -> one playlist, demuxed -> two\n");
}

/* --- the live source ----------------------------------------------------- */

static void test_live(void) {
    volatile int cancel = 0;
    VtHlsStreamFactory factory;
    int failures_before_init = failures;
    CHECK(vt_hls_stream_factory_init(&factory, PLAYLIST_URL, &cancel) == 0,
          "live factory init failed");
    if (failures > failures_before_init) return;
    pthread_mutex_lock(&factory.worker_lock);
    int live_flag = factory.live;
    pthread_mutex_unlock(&factory.worker_lock);
    CHECK(live_flag == 1, "a playlist without #EXT-X-ENDLIST is live");

    /* Read-ahead aims where the demuxer will actually join — near the end of
     * the window, not at its start. Nobody ever asks for the oldest segment
     * of a live window, and fetching it would be a wasted megabyte. */
    usleep(200000);
    CHECK(seg_fetches(g_head_seq - WINDOW + 1) == 0,
          "read-ahead fetched the oldest segment of the window");
    int ahead = 0;
    for (int seq = g_head_seq - 3; seq <= g_head_seq; seq++)
        ahead += seg_fetches(seq);
    CHECK(ahead >= 1, "read-ahead fetched nothing near the live edge");

    /* Live runs on two connections: segments are short and never edge-cached,
     * so serially every fetch pays its full first-byte latency out of
     * throughput — the measured difference between 187 KB/s and a stream that
     * needs 306. The read-ahead chain must therefore actually overlap. */
    CHECK(factory.workers[1].started == 1,
          "a live source should run a second fetch worker");
    int seen_parallel = 0;
    for (int i = 0; i < 100 && !seen_parallel; i++) {
        pthread_mutex_lock(&g_server_lock);
        seen_parallel = g_inflight_max >= 2;
        pthread_mutex_unlock(&g_server_lock);
        if (!seen_parallel) usleep(20000);
    }
    CHECK(seen_parallel,
          "the two workers never had fetches in flight at the same time");

    VtDecoderStreamHandle video, audio;
    CHECK(factory.factory.open(factory.factory.opaque, &video) == 0,
          "opening the video cursor failed");
    CHECK(factory.factory.open(factory.factory.opaque, &audio) == 0,
          "opening the audio cursor failed (a muxed source opens twice)");

    /* Both cursors read the same muxed playlist, and each reads it whole. */
    char buffer[8192];
    int read_bytes = video.read(video.opaque, buffer, sizeof buffer);
    CHECK(read_bytes > 0 && (int)video.size == read_bytes,
          "cursor read %d of %lld playlist bytes", read_bytes,
          (long long)video.size);
    CHECK(memcmp(buffer, "#EXTM3U", 7) == 0, "cursor did not read a playlist");

    /* One download, two consumers: this is the whole reason a muxed source
     * gets ONE factory instead of one per cursor. */
    int shared = g_head_seq - 2;
    char url[256];
    seg_url(url, sizeof url, shared);
    char *from_video = fetch_through(&video, url, NULL);
    char *from_audio = fetch_through(&audio, url, NULL);
    CHECK(from_video && from_audio, "a cursor could not fetch segment %d",
          shared);
    CHECK(from_video && from_audio && strcmp(from_video, from_audio) == 0,
          "the two cursors got different bytes for the same segment");
    CHECK(seg_fetches(shared) == 1,
          "segment %d was downloaded %d times, expected once", shared,
          seg_fetches(shared));
    free(from_video);
    free(from_audio);

    /* A segment the window has already dropped is still fetchable: the two
     * cursors reload on their own schedules, so one of them is regularly
     * holding a snapshot the other has moved past. */
    int stale = SEQ_BASE;
    seg_url(url, sizeof url, stale);
    char *old_segment = fetch_through(&video, url, NULL);
    CHECK(old_segment != NULL,
          "a segment outside the current window has to stay fetchable");
    free(old_segment);

    /* No reload spin: the clock is static, so nothing is stale, and the
     * read-ahead chain is parked at the window's end. A build that lets the
     * chain request reloads spins here — reload, re-seed, walk back to the
     * unchanged edge, request again — burning the connection the segments
     * need. One serve-tail hint is legitimate; a stream of fetches is the
     * spin. */
    int spin_before = g_playlist_fetches;
    usleep(400000);
    int spin_grew = g_playlist_fetches - spin_before;
    CHECK(spin_grew <= 1,
          "reload spin: %d playlist fetches while nothing was stale and "
          "nothing was consumed", spin_grew);

    /* The reload. Two things have to hold, and the second is what live
     * playback lives or dies on:
     *   - the window that comes back eventually names segments produced after
     *     playback started, and
     *   - no call into a cursor ever waits on the network to get it. A demux
     *     thread parked on an HTTPS round trip — queued behind whatever
     *     segment download is in flight — is seconds of stall every few
     *     seconds, which is what "live plays, but stutters" was. The worker
     *     therefore refreshes the window on its own, before anyone asks. */
    int before = g_playlist_fetches;
    char *fresh = fetch_through(&video, factory.decoder_url, NULL);
    CHECK(fresh && strstr(fresh, "#EXTM3U") == fresh,
          "the playlist URL must serve the playlist");
    CHECK(g_playlist_fetches == before,
          "a copy this fresh should not have been re-fetched");
    free(fresh);

    pthread_mutex_lock(&g_server_lock);
    g_head_seq += 3; /* the stream ran on */
    int new_head = g_head_seq;
    pthread_mutex_unlock(&g_server_lock);
    g_now_us += 6000000; /* ... and the copy in hand is now stale */

    char expected[64];
    snprintf(expected, sizeof expected, "/sq/%d/", new_head);
    int named = 0;
    uint64_t slowest_us = 0;
    for (int attempt = 0; attempt < 100 && !named; attempt++) {
        uint64_t started = wall_us();
        char *served = fetch_through(&video, factory.decoder_url, NULL);
        uint64_t took = wall_us() - started;
        if (took > slowest_us) slowest_us = took;
        if (served && strstr(served, expected)) named = 1;
        free(served);
        if (!named) usleep(20000);
    }
    CHECK(named,
          "the reloaded window never named segment %d — this is the failure "
          "that stopped live playback after a few seconds", new_head);
    CHECK(g_playlist_fetches > before, "no reload was actually performed");
    CHECK(slowest_us < 50000,
          "a cursor waited %llu us on a playlist — it must never wait on the "
          "network, that wait IS the stutter",
          (unsigned long long)slowest_us);

    /* And the segments it named are servable, which is what keeps playback
     * going past the window it started in. */
    seg_url(url, sizeof url, new_head);
    char *edge = fetch_through(&audio, url, NULL);
    CHECK(edge != NULL, "the newly announced segment %d could not be fetched",
          new_head);
    free(edge);

    /* A stream that ends publishes its last segment in one refresh and
     * #EXT-X-ENDLIST in the next — a window that brings nothing new but that
     * one line. It used to be discarded as "nothing new", so the source
     * stayed live forever, the demuxer never saw the end and the player
     * never reached EOF. */
    pthread_mutex_lock(&g_server_lock);
    g_ended = 1;
    pthread_mutex_unlock(&g_server_lock);
    g_now_us += 6000000;
    int ended_seen = 0;
    for (int attempt = 0; attempt < 100 && !ended_seen; attempt++) {
        char *served = fetch_through(&video, factory.decoder_url, NULL);
        if (served && strstr(served, "#EXT-X-ENDLIST")) ended_seen = 1;
        free(served);
        if (!ended_seen) usleep(20000);
    }
    CHECK(ended_seen,
          "an ENDLIST-only window never reached the served playlist");
    CHECK(factory.live == 0,
          "the source still reports live after #EXT-X-ENDLIST arrived");
    pthread_mutex_lock(&g_server_lock);
    g_ended = 0;
    pthread_mutex_unlock(&g_server_lock);

    printf("live: reload names sq %d, one download per segment, %d playlist "
           "fetches total, end seen\n", new_head, g_playlist_fetches);

    video.close(video.opaque);
    audio.close(audio.opaque);
    vt_hls_stream_factory_term(&factory);
}

/* --- where the two cursors join ------------------------------------------ */

/* Reads a cursor's whole playlist and works out the segment libavformat would
 * start playing from it: EXT-X-MEDIA-SEQUENCE plus live_start_index, whose
 * default -3 puts the join three segments back from the edge. */
static int join_sequence(VtDecoderStreamHandle *cursor) {
    char text[16384];
    int len = cursor->read(cursor->opaque, text, sizeof text - 1);
    if (len <= 0) return -1;
    text[len] = '\0';
    const char *tag = strstr(text, "#EXT-X-MEDIA-SEQUENCE:");
    if (!tag) return -1;
    int start_seq = atoi(tag + strlen("#EXT-X-MEDIA-SEQUENCE:"));
    int count = 0;
    for (const char *p = text; p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (*p && *p != '#' && *p != '\n' && *p != '\r') count++;
    int back = count - 1 - 3;
    return start_seq + (back > 0 ? back : 0);
}

/* The decoder opens its video cursor, probes it — which costs a segment
 * download, so seconds — and only then opens its audio cursor. For a muxed
 * source both come off ONE factory, and if the live window has moved on in
 * between, the second cursor reads a playlist whose edge sits a segment
 * later than the first one's. Both then start playing, several seconds
 * apart, from the same stream: audio runs ahead, and video spends the rest
 * of the session dropping frames trying to catch the audio clock. That is
 * not a subtle desync, it is what "plays, but stutters catastrophically"
 * looks like. */
static void test_cursor_alignment(void) {
    pthread_mutex_lock(&g_server_lock);
    g_ended = 0;
    g_head_seq += 100; /* a fresh stretch of stream for this case */
    pthread_mutex_unlock(&g_server_lock);

    volatile int cancel = 0;
    VtHlsStreamFactory factory;
    int failures_before_init = failures;
    CHECK(vt_hls_stream_factory_init(&factory, PLAYLIST_URL, &cancel) == 0,
          "live factory init failed");
    if (failures > failures_before_init) return;

    VtDecoderStreamHandle video, audio;
    CHECK(factory.factory.open(factory.factory.opaque, &video) == 0,
          "opening the video cursor failed");
    int video_join = join_sequence(&video);

    /* The probe takes its time and the stream does not wait. */
    pthread_mutex_lock(&g_server_lock);
    g_head_seq += 1;
    pthread_mutex_unlock(&g_server_lock);
    g_now_us += 6000000;
    for (int i = 0; i < 100; i++) { /* let the worker pick the new window up */
        usleep(20000);
        pthread_mutex_lock(&factory.worker_lock);
        int fresh = factory.playlist != NULL;
        pthread_mutex_unlock(&factory.worker_lock);
        if (fresh && g_playlist_fetches > 1) break;
    }

    CHECK(factory.factory.open(factory.factory.opaque, &audio) == 0,
          "opening the audio cursor failed");
    int audio_join = join_sequence(&audio);

    printf("alignment: video joins at sq %d, audio at sq %d\n", video_join,
           audio_join);
    CHECK(video_join > 0 && video_join == audio_join,
          "the two cursors join %d segments apart — every one of those is ~5 s "
          "of audio running ahead of video", audio_join - video_join);

    video.close(video.opaque);
    audio.close(audio.opaque);
    vt_hls_stream_factory_term(&factory);
}

/* --- read-ahead under steady consumption ---------------------------------- */

/* The failure mode the console showed at the live edge: playback consumes
 * segment after segment while the window slides, and the read-ahead silently
 * dies — either because consumed-but-kept ring slots satisfied its depth
 * check forever, or because a chain pointed at an already-cached segment was
 * dropped instead of walked forward. Every segment then becomes an on-demand
 * fetch with the demux thread stalled through its full latency: exactly
 * "plays for a moment, then stutters forever". This drives a steady playback
 * pattern and asserts every post-warmup segment is served from memory. */
static void test_readahead_alive(void) {
    pthread_mutex_lock(&g_server_lock);
    g_ended = 0;
    g_head_seq += 200; /* a fresh stretch of stream */
    int join = g_head_seq - WINDOW + 1 + (WINDOW > 5 ? WINDOW - 5 : 0);
    pthread_mutex_unlock(&g_server_lock);

    volatile int cancel = 0;
    VtHlsStreamFactory factory;
    int failures_before_init = failures;
    CHECK(vt_hls_stream_factory_init(&factory, PLAYLIST_URL, &cancel) == 0,
          "live factory init failed");
    if (failures > failures_before_init) return;

    VtDecoderStreamHandle video, audio;
    CHECK(factory.factory.open(factory.factory.opaque, &video) == 0,
          "video cursor open failed");
    CHECK(factory.factory.open(factory.factory.opaque, &audio) == 0,
          "audio cursor open failed");

    uint64_t worst_late_us = 0;
    for (int step = 0; step < 10; step++) {
        int seq = join + step;
        /* The stream runs on and the copy in hand ages, exactly as live. */
        pthread_mutex_lock(&g_server_lock);
        g_head_seq++;
        pthread_mutex_unlock(&g_server_lock);
        g_now_us += 5000000;
        usleep(150000); /* worker time: reload + prefetch (40 ms per fetch) */

        if (step >= 2) {
            /* The structural claim: while playback sits at `seq`, the ring
             * must already hold an unconsumed segment beyond it. */
            pthread_mutex_lock(&factory.worker_lock);
            int fresh = 0;
            for (int i = 0; i < VT_HLS_SEGMENT_SLOTS; i++)
                if (factory.slots[i].url && factory.slots[i].serves == 0 &&
                    factory.slots[i].state != 0 /* != EMPTY */)
                    fresh++;
            pthread_mutex_unlock(&factory.worker_lock);
            CHECK(fresh >= 1,
                  "step %d: read-ahead is dead — nothing unconsumed in the "
                  "ring while playback needs segment %d", step, seq);
        }

        char url[256];
        seg_url(url, sizeof url, seq);
        uint64_t t0 = wall_us();
        char *for_video = fetch_through(&video, url, NULL);
        char *for_audio = fetch_through(&audio, url, NULL);
        uint64_t took = wall_us() - t0;
        CHECK(for_video && for_audio, "segment %d was not served", seq);
        free(for_video);
        free(for_audio);
        if (step >= 2 && took > worst_late_us) worst_late_us = took;
    }
    /* Both cursors, from memory: far under one stub fetch (40 ms). A dead
     * read-ahead pays 40 ms + polling per segment and fails this by 3x. */
    CHECK(worst_late_us < 30000,
          "a post-warmup segment took %llu us to serve — that is an on-demand "
          "network fetch with the demuxer stalled, not a read-ahead hit",
          (unsigned long long)worst_late_us);
    printf("readahead: alive across a sliding window, worst serve %llu us\n",
           (unsigned long long)worst_late_us);

    video.close(video.opaque);
    audio.close(audio.opaque);
    vt_hls_stream_factory_term(&factory);
}

/* --- the tiny window ------------------------------------------------------ */

/* Some channels serve a window of THREE segments. Joining one lands on its
 * oldest segment, and a real console needs seconds to probe — on a raw
 * window that segment has expired before playback starts, libavformat skips
 * ahead on ONE of the two cursors, and audio runs a fixed two seconds in
 * front of video forever (console log 2026-09-01 21:08, twice, only on
 * 3-segment windows). The accumulated playlist makes expiry impossible:
 * whatever a cursor is standing on stays in the served list. */
static void test_tiny_window(void) {
    pthread_mutex_lock(&g_server_lock);
    g_ended = 0;
    g_window = 3;
    g_head_seq += 300;
    int first_seq = g_head_seq - 3 + 1; /* join lands here (count-5 clamps) */
    pthread_mutex_unlock(&g_server_lock);

    volatile int cancel = 0;
    VtHlsStreamFactory factory;
    int failures_before_init = failures;
    CHECK(vt_hls_stream_factory_init(&factory, PLAYLIST_URL, &cancel) == 0,
          "tiny-window factory init failed");
    if (failures > failures_before_init) { g_window = WINDOW; return; }

    /* The probe takes seconds and the stream does not wait: slide the window
     * fully past the join point before anything is consumed. */
    for (int i = 0; i < 4; i++) {
        pthread_mutex_lock(&g_server_lock);
        g_head_seq++;
        pthread_mutex_unlock(&g_server_lock);
        g_now_us += 5000000;
        usleep(120000);
    }

    VtDecoderStreamHandle video, audio;
    CHECK(factory.factory.open(factory.factory.opaque, &video) == 0,
          "video cursor open failed");
    CHECK(factory.factory.open(factory.factory.opaque, &audio) == 0,
          "audio cursor open failed");

    /* The served playlist must still NAME the join segment — that is the
     * whole point: nothing a cursor stands on may ever expire. */
    char expected[64];
    snprintf(expected, sizeof expected, "/sq/%d/", first_seq);
    char *served = fetch_through(&video, factory.decoder_url, NULL);
    CHECK(served && strstr(served, expected),
          "segment %d expired from the served playlist — this is the exact "
          "expiry that split audio from video on tiny windows", first_seq);
    char seqline[48];
    snprintf(seqline, sizeof seqline, "#EXT-X-MEDIA-SEQUENCE:%d", first_seq);
    CHECK(served && strstr(served, seqline),
          "MEDIA-SEQUENCE moved — the demuxer would renumber and skip");
    free(served);

    /* And playing from the join point, straight through what the raw window
     * long dropped, works for both cursors. */
    for (int seq = first_seq; seq < first_seq + 6; seq++) {
        char url[256];
        seg_url(url, sizeof url, seq);
        char *a = fetch_through(&video, url, NULL);
        char *b = fetch_through(&audio, url, NULL);
        CHECK(a && b, "segment %d was not served from behind the window",
              seq);
        free(a);
        free(b);
    }
    printf("tiny window: nothing expires, playback from %d fine\n", first_seq);

    video.close(video.opaque);
    audio.close(audio.opaque);
    vt_hls_stream_factory_term(&factory);
    pthread_mutex_lock(&g_server_lock);
    g_window = WINDOW;
    pthread_mutex_unlock(&g_server_lock);
}

/* The accumulated list is bounded: past the cap the front is dropped and the
 * header counters advance by exactly what left.
 *
 * The cap itself is a compile-time constant inside hls.c and deliberately NOT
 * read here — an earlier version of this test hard-coded a cycle count that
 * only reached the cap when the suite was built with -DHLS_ACC_MAX_SEGMENTS
 * turned down, so building it the obvious way produced a red suite for a
 * perfectly good library. It now feeds the window until a trim is observed,
 * whatever the cap is set to. */
static void test_trim(void) {
    pthread_mutex_lock(&g_server_lock);
    g_ended = 0;
    g_head_seq += 400;
    int first_seq = g_head_seq - WINDOW + 1;
    pthread_mutex_unlock(&g_server_lock);

    volatile int cancel = 0;
    VtHlsStreamFactory factory;
    int failures_before_init = failures;
    CHECK(vt_hls_stream_factory_init(&factory, PLAYLIST_URL, &cancel) == 0,
          "trim factory init failed");
    if (failures > failures_before_init) return;

    VtDecoderStreamHandle cursor;
    CHECK(factory.factory.open(factory.factory.opaque, &cursor) == 0,
          "cursor open failed");

    char gone[64];
    snprintf(gone, sizeof gone, "/sq/%d/", first_seq);
    char *served = NULL;
    int trimmed = 0;
    /* Generous bound: enough cycles to pass any sane cap, and the loop stops
     * the moment the first entry actually falls off. */
    for (int i = 0; i < 400 && !trimmed; i++) {
        pthread_mutex_lock(&g_server_lock);
        g_head_seq++;
        pthread_mutex_unlock(&g_server_lock);
        g_now_us += 5000000;
        usleep(20000);
        free(served);
        served = fetch_through(&cursor, factory.decoder_url, NULL);
        if (served && strstr(served, gone) == NULL) trimmed = 1;
    }
    CHECK(trimmed, "the accumulated list never trimmed its front");

    if (served && trimmed) {
        char newest[64];
        pthread_mutex_lock(&g_server_lock);
        snprintf(newest, sizeof newest, "/sq/%d/", g_head_seq);
        int head = g_head_seq;
        pthread_mutex_unlock(&g_server_lock);
        CHECK(strstr(served, newest) != NULL,
              "the newest segment is missing after a trim");
        const char *ms = strstr(served, "#EXT-X-MEDIA-SEQUENCE:");
        int seq = ms ? atoi(ms + 22) : -1;
        int uris = 0;
        for (const char *p = served; p;
             p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
            if (*p && *p != '#' && *p != '\n' && *p != '\r') uris++;
        CHECK(seq > first_seq,
              "MEDIA-SEQUENCE did not advance with the trim (still %d)", seq);
        /* The numbering has to keep describing the entries that are actually
         * listed, or the demuxer renumbers and skips. */
        CHECK(seq + uris - 1 == head,
              "sequence %d + %d entries - 1 != head %d — the numbering no "
              "longer matches the listed segments", seq, uris, head);
        printf("trim: front dropped, MEDIA-SEQUENCE %d, %d entries\n", seq,
               uris);
    }
    free(served);

    cursor.close(cursor.opaque);
    vt_hls_stream_factory_term(&factory);
}

/* --- the on-demand source ------------------------------------------------ */

static void test_complete(void) {
    pthread_mutex_lock(&g_server_lock);
    g_ended = 1;
    pthread_mutex_unlock(&g_server_lock);

    volatile int cancel = 0;
    VtHlsStreamFactory factory;
    int failures_before_init = failures;
    CHECK(vt_hls_stream_factory_init(&factory, PLAYLIST_URL, &cancel) == 0,
          "on-demand factory init failed");
    if (failures > failures_before_init) return;
    pthread_mutex_lock(&factory.worker_lock);
    int live_flag = factory.live;
    pthread_mutex_unlock(&factory.worker_lock);
    CHECK(live_flag == 0, "#EXT-X-ENDLIST means the playlist is complete");
    CHECK(factory.workers[1].started == 0,
          "VOD moves fine on one connection per playlist — a second worker "
          "here would double every video's socket count for nothing");

    VtDecoderStreamHandle cursor;
    CHECK(factory.factory.open(factory.factory.opaque, &cursor) == 0,
          "opening a cursor failed");

    /* A finished playlist is never re-fetched, however old the copy gets:
     * there is nothing new for a reload to find. */
    int before = g_playlist_fetches;
    g_now_us += 60000000;
    char *served = fetch_through(&cursor, factory.decoder_url, NULL);
    CHECK(served != NULL, "the playlist URL must still serve the playlist");
    CHECK(g_playlist_fetches == before,
          "a complete playlist was re-fetched %d times",
          g_playlist_fetches - before);
    free(served);

    /* Seeking still resolves to segment boundaries here — 5 s segments, so
     * 12 s into the stream is the start of the third one. */
    CHECK(vt_hls_segment_start_ms(&factory, 12000) == 10000,
          "expected 12 s to resume at 10 s, got %llu",
          (unsigned long long)vt_hls_segment_start_ms(&factory, 12000));

    printf("complete: no reloads, seek snaps to 10000 ms\n");

    cursor.close(cursor.opaque);
    vt_hls_stream_factory_term(&factory);
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    test_pick();
    test_live();
    test_cursor_alignment();
    test_readahead_alive();
    test_tiny_window();
    test_trim();
    test_complete();
    if (failures) {
        fprintf(stderr, "%d HLS CHECK(S) FAILED\n", failures);
        return 1;
    }
    printf("ALL HLS TESTS PASSED\n");
    return 0;
}
