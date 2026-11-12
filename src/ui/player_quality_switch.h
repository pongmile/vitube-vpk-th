/* ViTube — opening a source and switching quality mid-playback.
 *
 * The Vita's hardware AAC decoder is a single system-wide resource: opening
 * a second VtDecoderPlayer while one is already open fails hard with
 * SCE_AUDIODEC_ERROR_ALREADY_INITIALIZED (confirmed on real hardware —
 * vt_decoder_open ret=-2139160573/0x807F0003 every time, even though the
 * server-side resolve had already succeeded). So only ONE decoder can ever
 * be open at a time — but that only applies to vt_decoder_open() itself.
 * Everything that leads up to it (opening the HTTPS connection, allocating
 * the decoder object) touches no shared hardware state at all and is
 * perfectly safe to do while the old source is still alive and playing.
 *
 * So a switch runs in up to three stages, each handing off to the next:
 *  1. (HD only) resolve the HLS manifest from YouTube — a pure network
 *     operation (HdResolveJob / hd_resolve_worker).
 *  2. Open the connection + allocate the decoder for the resolved (or, for
 *     a direct format, already-known) URL — also pure network/allocation,
 *     no decoder touched (PrepareJob / prepare_worker).
 *  3. commit_prepared_switch: destroy the old decoder, open the new
 *     (already-connected) one. This is the one unavoidably exclusive step
 *     — but by the time it runs, there is no network wait left in it at
 *     all, so it is now genuinely brief rather than "brief in theory".
 *
 * The old source keeps playing normally (present/render_complete every
 * frame in the caller's main loop) through stages 1 and 2 without any
 * special casing — nothing touches the current player until stage 3.
 * Stage 3 itself can't present anything (no decoder is open during it), so
 * the screen simply keeps showing whatever it last swapped to for the
 * handful of frames it takes — no spinner, no "Puffern ..." interruption.
 */
#ifndef VITUBE_UI_PLAYER_QUALITY_SWITCH_H
#define VITUBE_UI_PLAYER_QUALITY_SWITCH_H

#include <stdint.h>

#include "media/hls.h"
#include "media/local_stream.h"
#include "media/remote_stream.h"
#include "media/vita_decoder.h"
#include "yt/model.h"

/* One playable source, in any of the three shapes this app can open:
 *  - a progressive MP4 over an HTTPS range stream (YouTube's direct 360p
 *    formats, and the HD server's muxed file),
 *  - YouTube's HLS, which is the app's own route to 720p and to live. On
 *    demand that is two playlists, one per media type, each fetched by its
 *    own cursor; live is one muxed playlist that both cursors read (see
 *    hls_muxed, and media/hls.h for why the two differ), or
 *  - a finished download: an MP4 on the memory card (media/local_stream.h).
 *    No network is involved at all, so none of the HLS or quality-switch
 *    machinery applies to it — it is never prepared in the background and has
 *    no stream-level cancel flag to repoint.
 * The decoder captures the factory addresses, so a PlayerSource must live in
 * stable storage for the whole playback — never a stack temporary. */
typedef struct {
    int is_hls;
    int is_local;
    /* The chosen HLS variant carries both streams in one playlist, so
     * hls_audio is unused and hls_video serves both decoder cursors. */
    int hls_muxed;
    VtRemoteStreamFactory remote;
    VtHlsStreamFactory hls_video;
    VtHlsStreamFactory hls_audio;
    VtLocalStreamFactory local;
} PlayerSource;

/* url is a media URL, or an HLS master playlist URL when is_hls. max_fps
 * (0 = no limit) caps the frame rate of the HLS variant picked — see
 * vt_hls_pick; live streams pass 30 because their 720p is usually 60. */
int player_source_init(PlayerSource *source, const char *url, int is_hls,
                       int max_fps, volatile int *cancel);
/* A downloaded file on the memory card. Returns 0, -1 for a path that does not
 * fit; whether the file is any good is for the decoder's open to say. */
int player_source_init_local(PlayerSource *source, const char *path);
void player_source_term(PlayerSource *source);
/* Fills in the stream fields of a decoder config from this source. */
void player_source_apply(const PlayerSource *source,
                         VtDecoderPlayerConfig *config);
/* Repoints the stream-level cancel flag (see commit_prepared_switch). */
void player_source_repoint_cancel(PlayerSource *source, volatile int *cancel);

/* Resolve+open the chosen source (blocking form, only for the initial open,
 * before anything has been drawn yet — a spinner is the right feedback there).
 * want_hd resolves 720p via HLS; otherwise formats[fmt_index] is opened.
 * Returns 0 ok, <0 fail, -2 cancelled. */
/* Blocking open of stream->formats[fmt_index] at start_ms — the runtime
 * fallback when HLS playback fails mid-video. Returns 0 ok, <0 fail. */
int open_direct_blocking_at(const yt_stream_t *stream, int fmt_index,
                            uint64_t start_ms, int volume, int start_paused,
                            volatile int *cancel,
                            PlayerSource *source_out,
                            VtDecoderPlayer **player_out);

/* Blocking open of a downloaded file, with the spinner (it is the first open of
 * the playback, before anything has been drawn). No resolve, no network: the
 * file is read straight off the card. Returns 0 ok, <0 fail (the file would not
 * open or the decoder refused it), and also <0 with *cancel set when Circle
 * ended the wait. */
int open_local_blocking(const char *path, int volume, int start_paused,
                        volatile int *cancel, PlayerSource *source_out,
                        VtDecoderPlayer **player_out);

/* volume/start_paused are the viewer's current settings, so a reopen (live
 * recovery) keeps them instead of coming back at 100 % and playing. */
int open_source_blocking(const char *video_id, int want_hd,
                         const yt_stream_t *stream, int fmt_index,
                         int volume, int start_paused,
                         volatile int *cancel,
                         PlayerSource *source_out,
                         VtDecoderPlayer **player_out);

/* --- stage 1: HD resolve (network only, server-side) --------------------- */

typedef struct {
    const char *video_id;
    volatile int cancel;
    volatile int done;
    int result;              /* 0 ok (url holds it), -1 fail, -2 cancelled */
    int is_hls;              /* url is an HLS master playlist, not a file */
    char url[YT_URL_MAX];
    char hd_reason[160];
} HdResolveJob;

void *hd_resolve_worker(void *arg);

/* --- stage 2: prepare (network + allocation only, no decoder touched) ---- */

typedef struct {
    const char *url;
    int exp_w, exp_h, exp_fps;
    int is_hls;                        /* url is an HLS master playlist */
    int max_fps;                       /* variant frame-rate cap, 0 = none */
    PlayerSource *source_out;          /* the free sources[] slot */
    volatile int cancel;
    volatile int done;
    int result;               /* 0 ok, -1 fail (job->cancel tells cancelled) */
    VtDecoderPlayer *player;  /* allocated, not yet opened, on success */
} PrepareJob;

void *prepare_worker(void *arg);

/* --- stage 3: commit (the one exclusive step) ----------------------------
 * Destroys the current player (the ONLY point where the old and new source
 * must not coexist) and opens the ALREADY-PREPARED one from a finished
 * PrepareJob. If even this fails, attempts to recover by reopening the
 * direct format at the current quality index (resumed position) rather than
 * leaving the user with nothing — for an old HD source too, since the picker
 * is only offered when a progressive format exists; *is_hd then reads 0.
 * Returns 1 switched, 0 recovered on a direct source, -1 total failure
 * (*player is NULL, both remotes[] slots safely torn down). */
int commit_prepared_switch(VtDecoderPlayer **player, int *cur,
                           PlayerSource *sources, int *is_hd,
                           int *quality, const yt_stream_t *stream,
                           PrepareJob *prep, int ns, int new_is_hd,
                           int new_quality_idx, int volume,
                           volatile int *cancel);

#endif
