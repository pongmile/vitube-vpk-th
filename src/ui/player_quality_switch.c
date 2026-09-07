/* ViTube — opening a source and switching quality mid-playback.
 * See player_quality_switch.h for the three-stage design. */
#include "ui/player_quality_switch.h"

#include <stdio.h>

#include <psp2/kernel/processmgr.h>

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "ui/strings.h"
#include "ui/ui.h"
#include "yt/innertube.h"

/* --- a playable source -------------------------------------------------- */

int player_source_init(PlayerSource *source, const char *url, int is_hls,
                       int max_fps, volatile int *cancel) {
    if (!source || !url) return -1;
    memset(source, 0, sizeof *source);
    if (!is_hls)
        return vt_remote_stream_factory_init(&source->remote, url, cancel);

    char *master = NULL;
    if (vt_hls_fetch_master(url, cancel, &master) < 0) {
        vt_log("hls: master playlist fetch failed");
        return -1;
    }
    /* Two URLs of playlist length: heap, not stack — this runs on the
     * prepare worker thread. */
    char *video_url = malloc(YT_URL_MAX), *audio_url = malloc(YT_URL_MAX);
    int ret = -1;
    if (video_url && audio_url)
        ret = vt_hls_pick(master, 720, max_fps, video_url, YT_URL_MAX,
                          audio_url, YT_URL_MAX);
    free(master);
    int muxed = ret == 0 && !audio_url[0];
    if (ret == 0) {
        ret = vt_hls_stream_factory_init(&source->hls_video, video_url, cancel);
        /* No audio URL means the variant is muxed — one playlist carrying
         * both streams, which is how YouTube serves live. There is no second
         * factory to open; both decoder cursors read this one. */
        if (ret == 0 && !muxed) {
            ret = vt_hls_stream_factory_init(&source->hls_audio, audio_url,
                                             cancel);
            if (ret < 0) vt_hls_stream_factory_term(&source->hls_video);
        }
    }
    free(video_url);
    free(audio_url);
    if (ret < 0) return -1;
    source->is_hls = 1;
    source->hls_muxed = muxed;
    return 0;
}

void player_source_term(PlayerSource *source) {
    if (!source) return;
    if (source->is_hls) {
        vt_hls_stream_factory_term(&source->hls_video);
        if (!source->hls_muxed) vt_hls_stream_factory_term(&source->hls_audio);
    } else {
        vt_remote_stream_factory_term(&source->remote);
    }
    source->is_hls = 0;
    source->hls_muxed = 0;
}

void player_source_apply(const PlayerSource *source,
                         VtDecoderPlayerConfig *config) {
    if (!source || !config) return;
    if (source->is_hls) {
        config->stream = source->hls_video.factory;
        /* Muxed: leaving stream_audio zeroed is what makes the decoder open
         * this same factory a second time for its audio cursor — and that
         * shared factory is what keeps each segment downloaded once and
         * handed to both. */
        if (!source->hls_muxed)
            config->stream_audio = source->hls_audio.factory;
        /* Both cursors have to resume at the same picture, and for HLS that
         * picture is a segment boundary whichever position is asked for.
         * See vt_hls_segment_start_ms. */
        if (config->start_position_ms > 0)
            config->start_position_ms =
                vt_hls_segment_start_ms(&source->hls_video,
                                        config->start_position_ms);
        /* Exact positioning is not on offer here at all: an HLS segment
         * carries exactly one keyframe, at its start, so the only picture the
         * video cursor can begin on is the one the position above already
         * names. Asking for an exact resume would make read_video_packet
         * discard that keyframe (its timestamp is not strictly past the
         * request once rounding is involved) and wait for the NEXT segment's,
         * putting video a full segment ahead of audio instead. This also
         * carries into vt_decoder_seek, which reuses the config it was opened
         * with. */
        config->approximate_seek = 1;
    } else {
        config->stream = source->remote.factory;
    }
}

void player_source_repoint_cancel(PlayerSource *source, volatile int *cancel) {
    if (!source) return;
    if (source->is_hls) {
        source->hls_video.cancel_flag = cancel;
        if (!source->hls_muxed) source->hls_audio.cancel_flag = cancel;
    } else {
        source->remote.cancel_flag = cancel;
    }
}

/* --- opening a source --------------------------------------------------- */

/* 720p resolves through the app's own HLS path and nothing else: no server,
 * no account, and it works wherever the Vita is, because YouTube ties the
 * stream URLs to the IP that asked and here that is the Vita. When HLS says
 * no (age-gated or otherwise account-gated video), there is no fallback —
 * playback stays on the direct format the caller already has.
 * Returns 0 ok, -1 failed, -2 cancelled. */
static int resolve_hd(const char *video_id, volatile int *cancel, char *url,
                      size_t url_cap, int *is_hls, char *reason,
                      size_t reason_cap) {
    *is_hls = 0;
    if (reason_cap) reason[0] = '\0';
    if (yt_hls_manifest(video_id, cancel, url, url_cap) == 0) {
        *is_hls = 1;
        return 0;
    }
    if (cancel && *cancel) return -2;
    if (reason_cap) snprintf(reason, reason_cap, "%s", yt_last_error());
    return -1;
}

typedef struct {
    const char *video_id;
    volatile int *cancel;
    int is_hls;
    char url[YT_URL_MAX];
    char reason[160];
} HdTask;

static int hd_task(void *opaque) {
    HdTask *task = opaque;
    return resolve_hd(task->video_id, task->cancel, task->url,
                      sizeof task->url, &task->is_hls, task->reason,
                      sizeof task->reason);
}

typedef struct {
    VtDecoderPlayer *player;
    VtDecoderPlayerConfig config;
    /* Source init runs inside the task too: for HLS that is two or three
     * synchronous fetches (master, media playlists), which used to happen on
     * the main thread BEFORE the spinner — a frozen frame with no Circle for
     * as long as their timeouts allow. */
    const char *url;
    int is_hls;
    int max_fps;
    volatile int *cancel;
    PlayerSource *source_out;
    int source_ready;
} OpenTask;

static int open_task(void *opaque) {
    OpenTask *task = opaque;
    if (!task->source_ready) {
        if (player_source_init(task->source_out, task->url, task->is_hls,
                               task->max_fps, task->cancel) < 0) {
            vt_log("open %.80s: source init failed", task->url);
            return -1;
        }
        task->source_ready = 1;
        player_source_apply(task->source_out, &task->config);
    }
    return vt_decoder_open(task->player, &task->config);
}

/* Blocking and silent: runs vt_decoder_open() directly on the calling
 * thread, no spinner, no redraw of any kind. Used only by
 * commit_prepared_switch's recovery path (reopening the old source after
 * the new one failed to open) — by then the old source has already been
 * destroyed, so vita2d simply keeps showing whatever frame it last swapped
 * to for the handful of frames this takes, instead of a "Puffern ..."
 * interruption. Not used for the very first open, where nothing has been
 * drawn yet and a spinner is the right feedback. */
static int open_from_url_silent(const char *url, int is_hls, int exp_w,
                                int exp_h, int exp_fps, uint64_t start_ms,
                                int volume, int start_paused,
                                volatile int *cancel,
                                PlayerSource *source_out,
                                VtDecoderPlayer **player_out) {
    /* Recovery path, direct formats only — no variant to pick. */
    if (player_source_init(source_out, url, is_hls, 0, cancel) < 0) {
        vt_log("open %.80s: source init failed", url);
        return -1;
    }
    VtDecoderPlayer *player = vt_decoder_create();
    if (!player) {
        vt_log("open %.80s: vt_decoder_create failed", url);
        player_source_term(source_out);
        return -1;
    }
    VtDecoderPlayerConfig config = {
        .expected_width = (uint32_t)(exp_w > 0 ? exp_w : 640),
        .expected_height = (uint32_t)(exp_h > 0 ? exp_h : 360),
        .expected_fps = exp_fps > 0 ? exp_fps : 30,
        .start_position_ms = start_ms,
        .volume_percent = volume,
        .cancel_flag = cancel,
        /* This helper is only ever used to resume mid-playback (the
         * quality-switch recovery path), never a user-initiated seek — see
         * commit_prepared_switch's config for why approximate_seek fits. */
        .approximate_seek = 1,
        .start_paused = start_paused,
    };
    player_source_apply(source_out, &config);
    int ret = vt_decoder_open(player, &config);
    if (ret < 0 || (cancel && *cancel)) {
        if (ret < 0)
            vt_log("open %.80s: vt_decoder_open failed ret=%d "
                   "expected=%dx%d@%dfps", url, ret, exp_w, exp_h, exp_fps);
        vt_decoder_destroy(player);
        player_source_term(source_out);
        return ret < 0 ? ret : -1;
    }
    *player_out = player;
    return 0;
}

/* Blocking: shows a spinner via vt_ui_run_task while opening. Used only for
 * the very first open, before anything has been drawn — there's nothing
 * else on screen to keep showing, so a spinner is the right feedback there
 * (unlike a quality switch — see open_from_url_silent above). */
static int open_from_url_blocking(const char *url, int is_hls, int max_fps,
                                  int exp_w, int exp_h, int exp_fps,
                                  uint64_t start_ms, int volume,
                                  int start_paused, volatile int *cancel,
                                  PlayerSource *source_out,
                                  VtDecoderPlayer **player_out) {
    VtDecoderPlayer *player = vt_decoder_create();
    if (!player) {
        vt_log("open %.80s: vt_decoder_create failed", url);
        player_source_term(source_out);
        return -1;
    }
    OpenTask open = {
        player,
        {
            .expected_width = (uint32_t)(exp_w > 0 ? exp_w : 640),
            .expected_height = (uint32_t)(exp_h > 0 ? exp_h : 360),
            .expected_fps = exp_fps > 0 ? exp_fps : 30,
            .start_position_ms = start_ms,
            .volume_percent = volume,
            .cancel_flag = cancel,
            /* With start_ms 0 (the normal first open) this has no effect;
             * with a mid-video resume (the HLS-to-direct fallback) the
             * exact alternative would discard-scan from the keyframe to
             * the target over the network first. */
            .approximate_seek = 1,
            .start_paused = start_paused,
        },
        url, is_hls, max_fps, cancel, source_out, 0,
    };
    int ret = vt_ui_run_task(vt_str(S_BUFFERING), open_task, &open, cancel);
    if (ret < 0 || (cancel && *cancel)) {
        if (ret < 0 && open.source_ready)
            vt_log("open %.80s: vt_decoder_open failed ret=%d "
                   "expected=%dx%d@%dfps", url, ret, exp_w, exp_h, exp_fps);
        vt_decoder_destroy(player);
        if (open.source_ready) player_source_term(source_out);
        return ret < 0 ? ret : -1;
    }
    *player_out = player;
    return 0;
}

/* Blocking open of a direct progressive format at a position — the runtime
 * fallback when HLS playback fails mid-video (player_screen.c). Shows the
 * spinner: by the time this runs the HLS decoder is already dead, so there
 * is nothing else to keep on screen. */
int open_direct_blocking_at(const yt_stream_t *stream, int fmt_index,
                            uint64_t start_ms, int volume, int start_paused,
                            volatile int *cancel,
                            PlayerSource *source_out,
                            VtDecoderPlayer **player_out) {
    if (!stream || fmt_index < 0 || fmt_index >= stream->format_count)
        return -1;
    const yt_format_t *f = &stream->formats[fmt_index];
    return open_from_url_blocking(f->url, 0, 0, f->width, f->height, f->fps,
                                  start_ms, volume, start_paused, cancel,
                                  source_out, player_out);
}

int open_source_blocking(const char *video_id, int want_hd,
                         const yt_stream_t *stream, int fmt_index,
                         int volume, int start_paused,
                         volatile int *cancel,
                         PlayerSource *source_out,
                         VtDecoderPlayer **player_out) {
    /* Live needs no resolve stage at all: there is no progressive format to
     * choose between, and the manifest came with the player response the
     * caller already has. Going through resolve_hd instead would ask YouTube
     * a second time and come back with the on-demand manifest — an hour-long
     * window whose playlist alone is most of a megabyte. */
    if (stream && stream->is_live && stream->hls_manifest[0])
        return open_from_url_blocking(stream->hls_manifest, 1, 30, 1280, 720,
                                      30, 0, volume, start_paused, cancel,
                                      source_out, player_out);
    if (want_hd) {
        HdTask task = { video_id, cancel, 0, "", "" };
        int r = vt_ui_run_task(vt_str(S_PREPARING_HD), hd_task, &task,
                               cancel);
        if (cancel && *cancel) return -2;
        if (r < 0 || !task.url[0]) {
            /* With no progressive format either, the caller shows the one
             * message that matters (why the video cannot play); a technical
             * "HD unavailable" on top of it only confused. */
            if (stream && stream->format_count > 0)
                vt_ui_toast(vt_str(S_HD_UNAVAILABLE),
                            task.reason[0] ? task.reason
                                           : vt_str(S_NO_HD_STREAM),
                            150);
            return -1;
        }
        return open_from_url_blocking(task.url, task.is_hls, 0, 1280, 720, 30,
                                      0, volume, start_paused, cancel,
                                      source_out, player_out);
    }
    const yt_format_t *f = &stream->formats[fmt_index];
    return open_from_url_blocking(f->url, 0, 0, f->width, f->height, f->fps, 0,
                                  volume, start_paused, cancel, source_out,
                                  player_out);
}

/* --- stage 1: HD resolve -------------------------------------------------- */

void *hd_resolve_worker(void *arg) {
    HdResolveJob *job = arg;
    char reason[160] = "";
    int is_hls = 0;
    int r = resolve_hd(job->video_id, &job->cancel, job->url, sizeof job->url,
                       &is_hls, reason, sizeof reason);
    job->is_hls = is_hls;
    if (r == 0) {
        job->result = 0;
    } else if (r == -2) {
        job->result = -2;
    } else {
        job->result = -1;
        snprintf(job->hd_reason, sizeof job->hd_reason, "%s",
                 reason[0] ? reason : vt_str(S_YOUTUBE_UNREACHABLE));
    }
    __sync_synchronize();
    job->done = 1;
    return NULL;
}

/* --- stage 2: prepare -----------------------------------------------------
 * Opens the HTTPS connection and allocates the decoder object for `url` —
 * everything that does NOT touch SceAudiodec — on a background thread,
 * while the current source keeps playing normally (present/render_complete
 * every frame, exactly like the HD resolve stage; this is why the actual
 * video stays live right up until the final commit, not just during the
 * server-side HD resolve). The one remaining step, vt_decoder_open()
 * itself, has to wait for commit_prepared_switch, since that's the part
 * the hardware only allows one of at a time. */

void *prepare_worker(void *arg) {
    PrepareJob *job = arg;
    if (player_source_init(job->source_out, job->url, job->is_hls,
                           job->max_fps, &job->cancel) < 0) {
        job->result = -1;
    } else {
        job->player = vt_decoder_create();
        if (job->player) {
            job->result = 0;
        } else {
            player_source_term(job->source_out);
            job->result = -1;
        }
    }
    __sync_synchronize();
    job->done = 1;
    return NULL;
}

/* --- stage 3: commit ------------------------------------------------------
 * Destroys the current player (the ONLY point where the old and new source
 * must not coexist — the single-decoder-instance hardware limit) and opens
 * the ALREADY-PREPARED one from a finished PrepareJob. Since the connection
 * was already established in the background while the old source kept
 * playing, this is just the local vt_decoder_open() call — no network wait
 * left, so the screen (frozen for these few frames, since nothing can be
 * presented while no decoder is open at all) is only briefly out of sync
 * with real time, not visibly "buffering". */
int commit_prepared_switch(VtDecoderPlayer **player, int *cur,
                           PlayerSource *sources, int *is_hd,
                           int *quality, const yt_stream_t *stream,
                           PrepareJob *prep, int ns, int new_is_hd,
                           int new_quality_idx, int volume,
                           volatile int *cancel) {
    VtDecoderPlayerStatus before;
    vt_decoder_get_status(*player, &before);
    uint64_t resume_ms = before.position_ms;
    int was_paused = before.paused;
    int old_is_hd = *is_hd, old_quality = *quality;

    /* Logged before touching anything, so a crash inside destroy or open
     * itself still leaves this line on disk (vt_log opens/writes/closes
     * the file synchronously per call) — the timing summary below only
     * gets written once both steps finish. */
    vt_log("commit: switching ns=%d is_hd=%d quality=%d resume_ms=%llu "
          "old_is_hd=%d old_quality=%d",
          ns, new_is_hd, new_quality_idx, (unsigned long long)resume_ms,
          old_is_hd, old_quality);

    uint64_t t_destroy0 = sceKernelGetProcessTimeWide();
    vt_decoder_destroy(*player);
    *player = NULL;
    player_source_term(&sources[*cur]);
    uint64_t t_destroy1 = sceKernelGetProcessTimeWide();

    /* prepare_worker initialized sources[ns] with &prep->cancel as the
     * stream-level cancel flag (so Circle could cancel stage 2 in
     * isolation). prep is the caller's stack-local PrepareJob, reused by
     * value for every subsequent switch attempt — once this stream commits
     * and becomes the live player, its RangeStream cursors (opened right
     * below, and again on every future seek) must no longer be checking
     * that address: the NEXT switch attempt reinitializes it, and cancelling
     * that later attempt (or even just its own failure path) would falsely
     * signal cancellation to THIS already-committed, still-playing stream,
     * corrupting or aborting a background prefetch thread out from under
     * live playback. Repoint it at the whole-playback-lifetime cancel
     * variable — the same one the very first open already uses. */
    player_source_repoint_cancel(&sources[ns], cancel);

    VtDecoderPlayerConfig config = {
        .expected_width = (uint32_t)(prep->exp_w > 0 ? prep->exp_w : 640),
        .expected_height = (uint32_t)(prep->exp_h > 0 ? prep->exp_h : 360),
        .expected_fps = prep->exp_fps > 0 ? prep->exp_fps : 30,
        .start_position_ms = resume_ms,
        .volume_percent = volume,
        .cancel_flag = cancel,
        /* This resume is "keep playing at roughly the same spot", not a
         * user-initiated seek — a fast approximate resume (up to one GOP
         * interval earlier, quickly caught up) is the right tradeoff here,
         * unlike Left/Right seeking where exact positioning matters more.
         * See VtDecoderPlayerConfig's doc comment for the full mechanism. */
        .approximate_seek = 1,
        /* In the config, not set_paused() after the open: the audio thread
         * is live the moment open returns, and the card write below sits in
         * between — one to three audible grains from a paused player. */
        .start_paused = was_paused,
    };
    player_source_apply(&sources[ns], &config);
    int ret = vt_decoder_open(prep->player, &config);
    uint64_t t_open1 = sceKernelGetProcessTimeWide();
    /* Diagnostics for the ONE unavoidably frozen step (see
     * player_quality_switch.h) — measuring where its time actually goes
     * (tearing down the old hardware session vs. opening the new one —
     * header-parse-over-the-already-open-connection plus genuine hardware
     * decoder init). */
    vt_log("commit: destroy=%llu ms open=%llu ms (ret=%d)",
          (unsigned long long)((t_destroy1 - t_destroy0) / 1000),
          (unsigned long long)((t_open1 - t_destroy1) / 1000), ret);
    if (ret == 0) {
        *player = prep->player;
        *cur = ns;
        *is_hd = new_is_hd;
        *quality = new_quality_idx;
        /* Deliberately does NOT touch the stored default. This used to be the
         * only way to express a preference at all, so writing it here was the
         * whole mechanism; since the Mehr tab owns that setting, a pick made
         * for one video would silently rewrite the system-wide one. It also
         * leaked the direct_good_enough exception outward: a Short that
         * bypasses HLS starts with is_hd = 0, and any pick from its menu then
         * stored "HD off" for everything. */
        /* What the new session actually negotiated: a resolution or frame
         * rate other than the one asked for is the first thing to check
         * when a switch opens slowly or stalls right after — this was a
         * silent gap since the player_screen.c rewrite dropped the
         * equivalent log this async design replaced. */
        VtDecoderPlayerStatus opened;
        vt_decoder_get_status(*player, &opened);
        vt_log("commit: opened backend=%s negotiated=%ux%u@%dfps",
              vt_decoder_backend_name(*player), opened.width, opened.height,
              opened.fps);
        return 1;
    }
    vt_log("quality swap: final open failed (ret=%d), attempting recovery",
          ret);
    vt_decoder_destroy(prep->player);
    player_source_term(&sources[ns]);
    /* The picker is only offered for videos with progressive formats, so
     * formats[old_quality] names a direct URL whether the old source was HD
     * (HLS) or not — reopening it costs the same single connection this
     * recovery has always paid for the direct case. Giving up when the old
     * source was HD ended the video over a transient failure for nothing. */
    if (!stream || old_quality < 0 || old_quality >= stream->format_count)
        return -1;
    const yt_format_t *old_fmt = &stream->formats[old_quality];
    *cancel = 0;
    VtDecoderPlayer *nplayer = NULL;
    ret = open_from_url_silent(old_fmt->url, 0, old_fmt->width,
                               old_fmt->height, old_fmt->fps, resume_ms,
                               volume, was_paused, cancel, &sources[ns],
                               &nplayer);
    if (ret < 0) return -1;
    *player = nplayer;
    *cur = ns;
    /* The recovered source is the direct format, whatever played before;
     * quality already names it. */
    *is_hd = 0;
    return 0;
}
