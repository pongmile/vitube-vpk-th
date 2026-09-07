/* ViTube — HLS playlist handling for the decoder.
 *
 * Two shapes of YouTube HLS arrive here, and the difference between them is
 * the manifest's, not this file's:
 *
 *  - On demand: 720p comes as YouTube's DEMUXED HLS (the VISIONOS player
 *    response). Video and audio are separate playlists, one factory and one
 *    cursor each, and a playlist is complete the moment it is fetched
 *    (#EXT-X-ENDLIST) — it never has to be looked at again.
 *
 *  - Live: the manifest in the ANDROID player response the app already has.
 *    Its variants are MUXED — one MPEG-TS carrying both streams — and its
 *    playlist is a 30-second window with no #EXT-X-ENDLIST, so the segments
 *    it names change as the stream runs. Both decoder cursors read that one
 *    playlist through ONE factory, which is what keeps each segment being
 *    downloaded once and served twice, and the playlist itself is re-fetched
 *    as it ages. That reload is the whole difference between a live stream
 *    that plays and one that stops after a few seconds.
 *
 * Either way the playlist URIs are already signed (no `n` parameter, so no
 * JavaScript challenge to solve) and the segments are plain MPEG-TS with
 * H.264 and AAC, which the hardware decoder handles.
 */
#ifndef VITUBE_MEDIA_HLS_H
#define VITUBE_MEDIA_HLS_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "media/vita_decoder.h"

/* Fetches an HLS master playlist. *text_out receives a malloc'd,
 * NUL-terminated copy the caller frees. Uses a short-lived client of its own,
 * so it is safe to call from a worker thread. Returns 0 on success. */
int vt_hls_fetch_master(const char *url, volatile int *cancel_flag,
                        char **text_out);

/* Picks the best variant that the hardware decoder can play from an HLS
 * master playlist: highest resolution up to max_height, H.264 video only,
 * plus the audio rendition that variant refers to. Returns 0 on success and
 * fills video_url, or -1 when the playlist offers nothing suitable.
 *
 * max_fps (0 = no limit) demotes variants above that frame rate below every
 * variant at or under it, whatever their resolution. NOT because the decoder
 * cannot keep up — measured on hardware 2026-09-01, an on-demand 720p60 plays
 * at 59.7 fps — but because of what a high frame rate costs on the wire: a
 * live 720p60 runs 4.4 Mbit/s against 1.6 for the 480p30 beside it, and the
 * console\'s link is the binding constraint on the live path, where one
 * connection carries both streams. A high-rate variant is still taken when
 * there is no alternative at all.
 *
 * audio_url comes back EMPTY when the chosen variant is muxed (it names no
 * AUDIO group, which is how YouTube serves live): there is no second playlist
 * to open, because the one video_url names carries both streams. */
int vt_hls_pick(const char *master, int max_height, int max_fps,
                char *video_url, size_t video_cap,
                char *audio_url, size_t audio_cap);

/* Segments kept in memory. The ring has to span the DISTANCE between the two
 * cursors of a muxed source, not just hold one segment each: video and audio
 * are demuxed by separate threads that drift apart whenever either one has a
 * bad moment, and the instant that drift exceeds the ring, they stop hitting
 * each other's segments and the stream gets pulled twice over a Wi-Fi link
 * that has no room for that — which starves video further, which widens the
 * drift. Four slots spanned eight seconds with the two-second segments a live
 * window uses, and that was not enough to break the loop (measured on real
 * hardware 2026-09-01: 20 served against 18 downloaded, where 2:1 is the
 * whole point). Sixteen spans half a minute. The byte budget below still
 * caps what this actually costs. */
#define VT_HLS_SEGMENT_SLOTS 16

typedef struct VtHlsSegmentSlot {
    char *url;      /* the segment held here, NULL when the slot is unused */
    char *data;
    size_t size;
    int state;      /* hls_cache_state_t, private to hls.c */
    int serves;     /* how many cursors have consumed this one. >0 means it
                     * only stays in the ring for the OTHER cursor of a muxed
                     * source and no longer counts as read-ahead; once every
                     * open cursor has taken it, it is dead weight and the
                     * first thing eviction reaches for (see slots_trim) */
    unsigned stamp; /* last use, for eviction among equals */
} VtHlsSegmentSlot;

/* The playlist as fetched, refcounted so a live reload can install a new one
 * while a cursor is still parsing the old. Private to hls.c. */
struct VtHlsPlaylist;

typedef struct VtHlsStreamFactory {
    char url[4096];                 /* the media playlist for this source */
    char decoder_url[4096];         /* same, with the scheme the demuxer sees */
    struct VtHlsPlaylist *playlist; /* the current snapshot */
    /* The snapshot every cursor of the CURRENT decoder session reads, pinned
     * when the first of them opens. A session's cursors are opened one after
     * the other, seconds apart, and a live window moves in between — reading
     * whatever was current at each open would have them compute different
     * live edges and start playing the same stream segments apart. */
    struct VtHlsPlaylist *session_playlist;
    int open_cursors;
    struct VitaHttpsClient *client; /* worker 0's connection (and init's) */
    struct VitaHttpsClient *client2;/* worker 1's, live sources only */
    volatile int *cancel_flag;
    VtDecoderStreamFactory factory; /* hand .factory to the decoder */

    /* Set while the playlist carries no #EXT-X-ENDLIST: it describes a window
     * that moves, so it has to be re-fetched to stay usable, and positions in
     * it mean nothing (see vt_hls_segment_start_ms). */
    int live;
    unsigned refresh_after_us;      /* how stale a live copy may get */
    uint64_t playlist_time_us;      /* when the current copy arrived */

    /* Segment workers (see hls.c): after init each worker thread is the sole
     * user of its own client. A worker serves demanded segments and fetches
     * upcoming ones ahead of time, so the demux thread finds the next segment
     * already in memory instead of stalling playback on a synchronous HTTPS
     * fetch at every segment boundary. Segments are addressed by their
     * URL rather than by their position, because a live window renumbers
     * itself on every reload while the URLs stay what they were.
     *
     * A live source runs TWO workers, VOD one. Live segments are short and
     * never edge-cached, so every request pays a full time-to-first-byte
     * before the transfer starts — serially, that latency came straight out
     * of throughput (measured on hardware 2026-09-01: one connection moved
     * 187 KB/s where the same console's VOD path moves ~500 KB/s, and the
     * stream needed 306). Two connections overlap one segment's TTFB with
     * the previous segment's transfer, exactly like the two per-playlist
     * connections VOD has always had. */
    struct VtHlsWorkerSlot {
        struct VtHlsStreamFactory *factory;
        struct VitaHttpsClient *client; /* borrowed, owned by the factory */
        pthread_t thread;
        int started;
    } workers[2];
    pthread_mutex_t worker_lock;
    pthread_cond_t worker_wake;
    int worker_started;             /* lock/cond live, >=1 worker running */
    int worker_stop;
    int want_playlist;              /* a cursor is waiting on a fresh copy */
    int reload_in_flight;           /* a worker is fetching the playlist */
    char *want_url;                 /* demanded by a cursor, NULL = none */
    char *next_url;                 /* speculative prefetch target */
    char *last_url;                 /* last served, to re-aim the prefetch */
    VtHlsSegmentSlot slots[VT_HLS_SEGMENT_SLOTS];
    unsigned stamp_counter;
    /* Decoder sessions opened on this factory (a seek or a recovery reopens
     * the same factory); the first one keeps the init-time read-ahead seed,
     * later ones start their speculation afresh (see cursor_attach). */
    unsigned sessions;
    /* Consecutive failed live-playlist reloads. Past a small threshold the
     * demuxer is refused the stale copy so the outage becomes an error the
     * player can recover from instead of a frozen picture. */
    unsigned reload_failures;
    /* The segment URL a cursor most recently gave up on, and when. The two
     * cursors read the same segments a beat apart, so the second gets the
     * same verdict at once: both lose the SAME segment and stay aligned,
     * instead of each skipping a different one on its own timeout. */
    char *abandoned_url;
    uint64_t abandoned_at_us;
    /* Demanded segments that failed in a row (reset by a served one). The
     * player reads it: a source whose segments all fail leaves the demuxer
     * skipping through the playlist with a frozen picture and no error, for
     * as long as the playlist is — this is what turns that into an error
     * within seconds. */
    unsigned consecutive_failures;
    /* Diagnostics, logged periodically: served counts what the cursors asked
     * for, downloads what actually went over the network. */
    unsigned served, downloads, reloads;
    unsigned last_logged_serves;
    uint64_t bytes;
} VtHlsStreamFactory;

/* Creates the HTTPS client, fetches the media playlist and rewrites its URIs
 * for the decoder. The factory must outlive the whole playback including all
 * seeks (every seek reopens the cursors). Returns 0 on success. */
int vt_hls_stream_factory_init(VtHlsStreamFactory *factory, const char *url,
                               volatile int *cancel_flag);
void vt_hls_stream_factory_term(VtHlsStreamFactory *factory);

/* Start of the segment that contains position_ms, in milliseconds — the point
 * a backward seek into this playlist actually resumes at. Returns position_ms
 * unchanged when the playlist cannot answer: no segment covers it, or the
 * playlist is live and so has no fixed origin to count from. */
uint64_t vt_hls_segment_start_ms(const VtHlsStreamFactory *factory,
                                 uint64_t position_ms);

#endif
