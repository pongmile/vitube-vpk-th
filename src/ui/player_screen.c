/* ViTube — fullscreen video player (720p via HLS first, direct fallback; a
 * finished download plays from the memory card with no network at all). */
#include "ui/player_screen.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#include "app.h"
#include "dl/dl.h"
#include "log.h"
#include "media/local_stream.h"
#include "media/remote_stream.h"
#include "media/vita_decoder.h"
#include "net/http.h"
#include "store.h"
#include "ui/player_hud.h"
#include "ui/player_quality_switch.h"
#include "ui/player_touch.h"
#include "ui/touch.h"
#include "ui/strings.h"
#include "ui/ui.h"
#include "yt/hd_helper.h"
#include "yt/innertube.h"

#define HUD_HIDE_US 3500000ULL
/* Stall detection (see progress_wall_us in player_screen_run). */
#define STALL_SHOW_US (1500ULL * 1000ULL)
#define STALL_ERROR_US (30ULL * 1000000ULL)
/* A live stream paused longer than this is resumed by a fresh open. */
#define LIVE_PAUSE_REOPEN_US (60ULL * 1000000ULL)
#define SEEK_STEP_MS 10000ULL
/* A downloaded file whose decoder session dies before playback has got this
 * far (and before the viewer has seeked) is taken to be a bad file; later
 * than that it is a playback error like any other. The engine checks a file
 * when it finishes, so reaching here means the card or the container lied, and
 * a download that is merely unlucky with the hardware decoder mid-video must
 * not be thrown away and fetched again for it. */
#define OFFLINE_BAD_START_MS 10000ULL
/* Minimum time the "Wechsle auf ..." label stays up, so a switch that
 * finishes in a couple of frames still registers as having happened. It runs
 * for at least this long and for as long as the switch is actually in flight,
 * whichever is more.
 *
 * It used to be a flat second regardless, on the reasoning that HD's
 * server-side resolve could take long enough for a banner to become a
 * nuisance over the video. That server detour is gone — HD resolves YouTube's
 * own HLS now — and the flat second left the one switch that does take a
 * moment announcing itself for a fraction of it and then going quiet. */
#define SWITCH_LABEL_SHOW_US 1000000ULL
/* How often playback health reaches the log. It ran once a SECOND through the
 * stutter investigations — which is how the live bugs were found, and also a
 * thousand synchronous card writes per session, issued from the loop that
 * drives presentation, for a stream that is behaving. Every line now carries
 * the deltas for its own window, so a bad half-minute still names itself
 * (shown far below 30/s, drops or underruns moving) at a fiftieth of the
 * traffic. Turn it back down while chasing something. */
#define STATUS_LOG_INTERVAL_US (30ULL * 1000000ULL)

/* --- background tasks (run on a worker thread via vt_ui_run_task) --------- */

typedef struct {
    const char *video_id;
    volatile int *cancel;
    yt_stream_t stream;
} ResolveTask;

static int resolve_task(void *opaque) {
    ResolveTask *task = opaque;
    return yt_resolve_stream(task->video_id, task->cancel, &task->stream);
}

typedef struct {
    VtDecoderPlayer *player;
    uint64_t position_ms;
} SeekTask;

static int seek_task(void *opaque) {
    SeekTask *task = opaque;
    return vt_decoder_seek(task->player, task->position_ms);
}

/* Every blocking modal (spinner, picker) runs its own frames: afterwards the
 * pre-modal `previous` would make a still-held button a fresh press, a finger
 * still down a tap — and a drag on the progress track that the modal
 * interrupted must not survive it, or the next touch anywhere is a seek. */
static void reset_input(SceCtrlData *previous, VtPlayerGesture *gesture) {
    sceCtrlPeekBufferPositive(0, previous, 1);
    ui_touch_reset();
    memset(gesture, 0, sizeof *gesture);
}

/* --- main entry --------------------------------------------------------- */

/* How one run of the player ended, as far as player_screen_run needs to know. */
typedef enum {
    PLAY_ENDED,     /* played, was cancelled, or failed and said so itself */
    PLAY_LOCAL_BAD  /* the downloaded file would not open, or the decoder gave
                     * up on it in its first seconds; nothing has been shown */
} PlayResult;

/* One run of the player, in one of two modes.
 *
 * Online (local_path NULL) is what the player always was: resolve the video,
 * open 720p HLS or a direct format, and keep both the picker and the
 * fallbacks between them.
 *
 * Offline (local_path is a finished download) is the same loop with the
 * network taken out of it. There is nothing to resolve, nothing to switch
 * between and nothing to fall back to, so the state those depend on —
 * `resolve`, is_hd, quality_pickable — is left at "no choice" rather than
 * having each use guarded: with no formats and no HLS source, the Triangle
 * menu stays shut and every HLS branch of the error ladder is unreachable.
 * What offline does need to know for itself is `offline`: the source to open,
 * the badge, and what a failure in the first seconds means. */
static PlayResult play_video(const yt_video_t *video, const char *local_path) {
    static volatile int cancel;
    cancel = 0;
    const int offline = local_path != NULL;

    /* 1. resolve direct progressive formats (fast; also gives metadata and
     *    the fallback path if HD is unavailable). Nothing to ask for a file
     *    that is already here: `resolve` stays zeroed (no formats, not live). */
    ResolveTask resolve = { video->video_id, &cancel, {0} };
    int ret = 0;
    int is_hd = 0;
    int is_live = 0;
    if (!offline) {
        ret = vt_ui_run_task(vt_str(S_LOADING_VIDEO), resolve_task, &resolve,
                             &cancel);
        if (cancel) return PLAY_ENDED;
        is_hd = yt_hd_pref_get();
        /* A live stream is HLS or nothing — YouTube publishes no progressive
         * format for one — so the HD preference has no say here. Left to it,
         * "HD off" would turn every live stream into "not playable". Which
         * manifest gets opened is open_source_blocking's business; what
         * matters from here on is that the transport bar must not offer to
         * seek. */
        is_live = resolve.stream.is_live;
        if (is_live) is_hd = 1;
        if (ret < 0) {
            vt_ui_toast(vt_str(S_VIDEO_UNPLAYABLE),
                        resolve.stream.reason[0] ? resolve.stream.reason
                            : vt_str(S_NETWORK_ERROR), 150);
            return PLAY_ENDED;
        }
        /* A video with zero progressive formats can still be playable via the
         * HD server path; only give up outright when there is nothing left to
         * try. */
        if (resolve.stream.format_count == 0 && !is_hd) {
            const char *reason = resolve.stream.reason[0]
                               ? resolve.stream.reason
                               : resolve.stream.playability[0]
                               ? resolve.stream.playability
                               : vt_str(S_ERR_NO_STREAM_DATA);
            vt_ui_toast(vt_str(S_VIDEO_UNPLAYABLE), reason, 150);
            return PLAY_ENDED;
        }
    }

    /* 2. open. Two stable stream slots so a later quality switch can open
     * the new source without first destroying the one that is playing (the
     * decoder captures the factory's address, so these must not be stack
     * temporaries that go out of scope). */
    PlayerSource sources[2];
    int cur = 0;
    /* One hardware-session reopen per rough position after a decode error
     * (see the status.error branch), tracked per source type so that an
     * HLS-to-direct fallback hands the direct source its own attempt;
     * UINT64_MAX = none spent yet. */
    uint64_t hls_error_retry_pos = UINT64_MAX;
    uint64_t direct_error_retry_pos = UINT64_MAX;
    VtDecoderPlayer *player = NULL;
    int quality = resolve.stream.selected; /* direct format index (0 if none) */
    /* switch_label is a transient toast, not a persistent status line — it
     * shows for ~1s after a switch is triggered, then hides again even
     * though stages 1-2 (see player_quality_switch.h) may still be running
     * for a while longer in the background; the source itself keeps
     * playing throughout regardless of whether this is visible. */
    char switch_label[48] = "";
    uint64_t switch_label_deadline = 0;
    /* Whether this switch gets the moving indicator. Decided when it is
     * triggered rather than from how long it ends up taking, so the banner
     * keeps one size for its whole life instead of growing partway through.
     * Only the HD path has a stage long enough to animate — it resolves a
     * manifest before it can prepare anything; a direct format already has
     * its URL and is committed before a block could travel. If a direct
     * switch ever drags on a bad connection, this is the line to revisit. */
    int switch_animated = 0;
    /* Stage 1 (HD only): server-side resolve. */
    pthread_t hd_thread;
    int hd_resolving = 0;
    HdResolveJob hd_job;
    /* Stage 2: connection + decoder-object prep, for either an HD URL just
     * resolved by stage 1 or a direct format (which skips stage 1
     * entirely, its URL already being known). */
    pthread_t prep_thread;
    int preparing = 0;
    PrepareJob prep_job;
    int prep_ns = 0, prep_new_is_hd = 0, prep_new_quality = 0;

    cancel = 0;

    /* Some videos hand back a direct progressive format that is already
     * everything the HLS detour would give, and resolving 720p for those
     * gains nothing. The bar is what HLS actually delivers, 720p, rather
     * than merely "better than 360p" — measured against the short side,
     * which for the landscape videos this check is about is the height.
     * (Portrait is handled outright below, not by this bar.) */
    int direct_short_side = 0;
    int is_portrait = 0;
    if (resolve.stream.format_count > 0) {
        const yt_format_t *f = &resolve.stream.formats[quality];
        is_portrait = f->width > 0 && f->width < f->height;
        direct_short_side = is_portrait ? f->width : f->height;
    }
    int direct_good_enough = direct_short_side >= 720;

    /* A Short never takes the HLS detour at all. vt_hls_pick caps by HEIGHT,
     * and a portrait video's height is its long side — so "720p HD" for a
     * Short resolves to a variant no bigger than the progressive format the
     * app already holds (~360x640), just served in segments: nothing gained,
     * plus segment-snapped seeks and a quality switch that can wobble between
     * two names for the same picture. The progressive formats arrive sorted
     * best-first, so the default IS the highest native resolution — and on a
     * 544-pixel-tall screen even 640 tall is already more than can be shown.
     * The Triangle picker stays closed for the same reason: every entry it
     * could offer is the same resolution wearing different labels. */
    if (is_portrait) is_hd = 0;
    int quality_pickable = !offline && !is_live && !is_portrait &&
                           resolve.stream.format_count > 0;

    if (offline) {
        /* The file is already on the card: open it and play. No spinner
         * beyond the open's own, no HD attempt, no direct fallback — if it
         * will not open there is nothing else to try HERE, and
         * player_screen_run decides whether the network can take over. */
        vt_log("%s: offline, playing %s", video->video_id, local_path);
        ret = open_local_blocking(local_path, 100, 0, &cancel, &sources[cur],
                                  &player);
        if (cancel) return PLAY_ENDED;
        if (ret < 0 || !player) {
            vt_log("%s: downloaded file would not open (ret=%d)",
                   video->video_id, ret);
            return PLAY_LOCAL_BAD;
        }
    } else {
        /* 720p FIRST: HD via HLS is the quality this player is meant to show,
         * so it is the one worth blocking the first frame on — the old
         * start-at-360p-then-upgrade flow meant every video visibly began
         * blurry and spent its first seconds on a quality switch. The direct
         * progressive format is the FALLBACK, used when the HLS resolve or
         * open fails (age gate, missing manifest, network) — and again
         * mid-video if HLS playback later dies (see the status.error branch).
         * A manual pick in the Triangle menu applies to this video only; the
         * default this reads lives in the Mehr tab. */
        /* YouTube already said why this one cannot play (age gate, private,
         * region, premiere) and offers no format: asking the HLS route as well
         * only produced a technical "HD unavailable" on top of the real reason.
         * A live stream is exempt — its progressive format count is always 0. */
        if (!is_live && resolve.stream.format_count == 0 &&
            resolve.stream.playability[0] &&
            strcmp(resolve.stream.playability, "OK") != 0) {
            vt_log("%s: not playable (%s), no formats", video->video_id,
                   resolve.stream.playability);
            vt_ui_toast(vt_str(S_VIDEO_UNPLAYABLE),
                        resolve.stream.reason[0] ? resolve.stream.reason
                                                 : resolve.stream.playability,
                        150);
            return PLAY_ENDED;
        }
        ret = -1;
        if (is_hd && !direct_good_enough) {
            ret = open_source_blocking(video->video_id, 1, &resolve.stream,
                                       quality, 100, 0, &cancel, &sources[cur],
                                       &player);
            if (cancel) return PLAY_ENDED;
            if (ret < 0 && resolve.stream.format_count > 0)
                vt_ui_toast(vt_str(S_HD_UNAVAILABLE),
                            vt_str(S_STARTING_LOWER_QUALITY), 100);
        } else {
            is_hd = 0;
        }
        if (ret < 0) {
            if (resolve.stream.format_count == 0) {
                /* HLS was the only option (a live stream, or a video without a
                 * progressive format) and it failed; nothing left to try. Say so
                 * — a silent return to the list reads as a crash. */
                vt_log("%s: hls-only open failed ret=%d (live=%d), giving up",
                       video->video_id, ret, is_live);
                vt_ui_toast(vt_str(S_VIDEO_UNPLAYABLE),
                            resolve.stream.reason[0] ? resolve.stream.reason
                                                     : vt_str(S_NETWORK_ERROR),
                            150);
                return PLAY_ENDED;
            }
            is_hd = 0;
            ret = open_source_blocking(video->video_id, 0, &resolve.stream,
                                       quality, 100, 0, &cancel, &sources[cur],
                                       &player);
            if (cancel) return PLAY_ENDED;
        }
        if (ret < 0 || !player) {
            /* Previously a failed open fell straight into the playback loop
             * with a NULL player: a black screen that only Circle could leave. */
            vt_ui_toast(vt_str(S_VIDEO_UNPLAYABLE),
                        resolve.stream.reason[0] ? resolve.stream.reason
                                                 : vt_str(S_NETWORK_ERROR), 150);
            return PLAY_ENDED;
        }
    }

    /* 3. playback loop */
    SceCtrlData ctrl, previous = {0};
    sceCtrlPeekBufferPositive(0, &previous, 1);
    ui_touch_reset();
    uint64_t hud_deadline = sceKernelGetProcessTimeWide() + HUD_HIDE_US;
    int hud_visible = 1;
    VtPlayerGesture gesture = {0};
    int fill_screen = 0;
    int volume = 100;
    int exit_code = 0;
    /* Offline only: still in the stretch where a failure means the FILE is
     * bad (see OFFLINE_BAD_START_MS), and the verdict once it has failed. */
    int local_at_start = offline;
    int bad_local = 0;
    /* A live stream's position_ms is the encoder's absolute MPEG-TS
     * timestamp (3:17:15 on the first frame in the shipped device log), not
     * anything a viewer can read as time. The HUD shows the time since the
     * join instead; re-latched by the live recovery, which opens afresh. */
    uint64_t live_join_ms = 0;
    /* Stall detection: the wall time at which position_ms last moved. With
     * the ring empty and the clock standing still the picture is frozen —
     * show that it is buffering, and after a long stall on an HLS source
     * take the recovery ladder instead of waiting on the network. */
    uint64_t progress_wall_us = sceKernelGetProcessTimeWide();
    uint64_t progress_pos_ms = 0;
    int stalled = 0;
    int stall_error = 0;
    /* A live stream paused for long lets the served window move past the
     * segments the demuxer stands on; the two cursors then rejoin at
     * different places. Resume by reopening at the edge instead. */
    uint64_t paused_since_us = 0;
    int live_resume = 0;

    /* Playback health; see STATUS_LOG_INTERVAL_US. The counters are
     * cumulative, so the previous line's values are what the deltas are
     * measured against — the first line's deltas are simply "since the
     * stream opened". */
    uint64_t next_status_log = 0;
    unsigned last_shown = 0, last_dropped = 0, last_underruns = 0;

    for (;;) {
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        unsigned pressed = ctrl.buttons & ~previous.buttons;
        previous = ctrl;
        UiTouchEvent touch;
        unsigned touch_flags = ui_touch_poll(&touch);
        uint64_t now = sceKernelGetProcessTimeWide();
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

        /* Poll stage 1 (HD resolve): never blocks, never stalls the
         * currently-playing source (pure network, no decoder touched). On
         * success, hand off to stage 2 (prepare) rather than committing
         * directly — the connection can still be opened, and the decoder
         * object allocated, while the old source keeps playing. */
        if (hd_resolving && hd_job.done) {
            pthread_join(hd_thread, NULL);
            hd_resolving = 0;
            if (hd_job.result == 0) {
                prep_job = (PrepareJob){
                    .url = hd_job.url, .exp_w = 1280, .exp_h = 720,
                    .exp_fps = 30, .is_hls = hd_job.is_hls,
                    .source_out = &sources[1 - cur],
                    .cancel = 0, .done = 0, .result = -1, .player = NULL,
                };
                prep_ns = 1 - cur;
                prep_new_is_hd = 1;
                prep_new_quality = quality;
                if (pthread_create(&prep_thread, NULL, prepare_worker,
                                   &prep_job) == 0) {
                    preparing = 1;
                } else {
                    vt_ui_toast(vt_str(S_HD_STREAM_FAILED), NULL, 120);
                }
            } else if (hd_job.result != -2) {
                vt_ui_toast(vt_str(S_HD_UNAVAILABLE),
                           hd_job.hd_reason[0] ? hd_job.hd_reason
                                               : vt_str(S_YOUTUBE_UNREACHABLE),
                           120);
            }
            reset_input(&previous, &gesture);
            continue;
        }

        /* Poll stage 2 (prepare): also never blocks and never stalls the
         * currently-playing source — same reasoning as stage 1, just for
         * the connection/allocation step instead of the server resolve.
         * On success, commit_prepared_switch does the one genuinely
         * exclusive step (destroy old, open new); that's the only part of
         * the whole switch briefly frozen now. */
        if (preparing && prep_job.done) {
            pthread_join(prep_thread, NULL);
            preparing = 0;
            if (prep_job.result == 0) {
                /* One deliberate frame before the only part of a switch that
                 * cannot draw. commit_prepared_switch destroys the running
                 * decoder and opens the prepared one without returning to
                 * this loop — for those few hundred milliseconds nothing can
                 * be presented, since there is no decoder to present from.
                 * Without this the screen freezes on a stale video frame and
                 * reads as a crash; with it, it freezes on a screen that says
                 * what is happening. The indicator stands still through it,
                 * which is the honest trade: moving it would mean tearing the
                 * hardware session down from a worker thread, next to a
                 * render thread mid-frame. */
                vita2d_start_drawing();
                vita2d_clear_screen();
                vt_decoder_present(player, fill_screen);
                draw_switch_overlay(switch_label, now, switch_animated);
                vita2d_end_drawing();
                vita2d_wait_rendering_done();
                vt_decoder_render_complete(player);
                vita2d_swap_buffers();
                int swap = commit_prepared_switch(&player, &cur, sources,
                                                  &is_hd, &quality,
                                                  &resolve.stream, &prep_job,
                                                  prep_ns, prep_new_is_hd,
                                                  prep_new_quality, volume,
                                                  &cancel);
                if (swap < 0) {
                    exit_code = -1;
                    break;
                }
                /* The source the user just picked is a new stream, so the
                 * one-reopen-per-position budget the old one spent must not
                 * be charged to it. Only for a genuine swap: the swap == 0
                 * recovery reopened the OLD source, whose budget stands. */
                if (swap > 0) {
                    direct_error_retry_pos = UINT64_MAX;
                    hls_error_retry_pos = UINT64_MAX;
                }
                if (swap == 0)
                    vt_ui_toast(vt_str(prep_new_is_hd
                                          ? S_HD_STREAM_FAILED
                                          : S_QUALITY_SWITCH_FAILED),
                               NULL, 120);
            } else if (!prep_job.cancel) {
                vt_ui_toast(vt_str(prep_new_is_hd ? S_HD_STREAM_FAILED
                                                  : S_QUALITY_SWITCH_FAILED),
                           NULL, 120);
            }
            reset_input(&previous, &gesture);
            hud_visible = 1;
            hud_deadline = sceKernelGetProcessTimeWide() + HUD_HIDE_US;
            continue;
        }

        VtDecoderPlayerStatus status;
        vt_decoder_get_status(player, &status);
        if (is_live && !live_join_ms && status.position_ms)
            live_join_ms = status.position_ms;
        if (local_at_start && status.opened && !status.error &&
            status.position_ms >= OFFLINE_BAD_START_MS)
            local_at_start = 0;

        if (!status.error && !status.eof && now >= next_status_log) {
            next_status_log = now + STATUS_LOG_INTERVAL_US;
            vt_log("status pos=%u ready=%d/%d shown=%u(+%u) dropped=%u(+%u) "
                   "au=%u(+%u)/%ums aj=%u paused=%d",
                   (unsigned)status.position_ms, status.ready_frames,
                   status.frame_capacity,
                   status.frames_shown, status.frames_shown - last_shown,
                   status.frames_dropped,
                   status.frames_dropped - last_dropped,
                   status.audio_underruns,
                   status.audio_underruns - last_underruns,
                   status.audio_worst_prep_ms, status.audio_pts_jumps,
                   status.paused);
            last_shown = status.frames_shown;
            last_dropped = status.frames_dropped;
            last_underruns = status.audio_underruns;
        }

        /* A live source whose playlist still carries no ENDLIST has not
         * ended: an EOF from the demuxer there means the network died and
         * libavformat gave up reloading. Treat it as the outage it is and
         * take the live recovery below instead of "video finished". */
        int live_outage = status.eof && is_live && sources[cur].is_hls &&
                          sources[cur].hls_video.live;
        if (live_outage)
            vt_log("hls live: demuxer eof at %u ms while the playlist is "
                   "still live -> recover", (unsigned)status.position_ms);
        if (status.position_ms != progress_pos_ms || status.paused ||
            hd_resolving || preparing || status.eof || status.error) {
            progress_pos_ms = status.position_ms;
            progress_wall_us = now;
        }
        uint64_t stalled_us = now - progress_wall_us;
        stalled = stalled_us > STALL_SHOW_US && status.ready_frames == 0;
        if (stalled && stalled_us > STALL_ERROR_US && sources[cur].is_hls &&
            !stall_error) {
            stall_error = 1;
            vt_log("stall: no progress for %u s at %u ms with an empty ring "
                   "-> recover", (unsigned)(stalled_us / 1000000ULL),
                   (unsigned)status.position_ms);
        }
        /* Every demanded segment failing (a 404 on each one after a quality
         * switch, in the device log of 2026-09-05) leaves the demuxer
         * skipping through the whole playlist with a frozen picture and
         * neither error nor EOF for a minute or more. Three in a row is a
         * dead source; take the recovery ladder now. */
        int hls_dead = sources[cur].is_hls &&
                       (sources[cur].hls_video.consecutive_failures >= 3 ||
                        sources[cur].hls_audio.consecutive_failures >= 3);
        if (hls_dead && !stall_error) {
            stall_error = 1;
            vt_log("hls: %u/%u consecutive segment failures at %u ms -> "
                   "recover", sources[cur].hls_video.consecutive_failures,
                   sources[cur].hls_audio.consecutive_failures,
                   (unsigned)status.position_ms);
        }
        if (status.error || live_outage || live_resume || stall_error) {
            const char *backend_name =
                vt_decoder_backend(player) == VT_DECODER_BACKEND_HARDWARE ? "hw"
                                                                          : "none";
            vt_log("%s=%s q=%s decode error at %u ms: %ux%u@%dfps "
                   "ready=%d/%d decoded=%u shown=%u dropped=%u",
                   backend_name, video->video_id,
                   offline ? "local" : is_hd ? "hd" : "direct",
                   (unsigned)status.position_ms, status.width, status.height,
                   status.fps, status.ready_frames, status.frame_capacity,
                   status.frames_decoded, status.frames_shown,
                   status.frames_dropped);
            /* Every path out of this ladder either recovers (continue) or
             * ends playback (break), so the stall verdict is consumed here
             * — leaving it set would make the next frame's status look like
             * a fresh error on whatever source the recovery just opened. */
            stall_error = 0;
            /* HLS recovers in two stages: first the same session reopened
             * at the failing position — SceVideodec starts clean at a
             * segment boundary keyframe with none of the streaming
             * session's accumulated state. One attempt per position; if
             * the reopened session dies on the same spot again before
             * playing 15 s further, the stream itself is the problem and
             * the second stage below drops to the direct progressive
             * format at the same position rather than showing an error. */
            if (vt_decoder_backend(player) == VT_DECODER_BACKEND_HARDWARE &&
                sources[cur].is_hls &&
                (live_resume || hls_error_retry_pos == UINT64_MAX ||
                 status.position_ms >= hls_error_retry_pos + 15000)) {
                if (!live_resume) hls_error_retry_pos = status.position_ms;
                progress_wall_us = now;
                if (is_live) {
                    /* Live recovers by opening the WHOLE source afresh at
                     * the current edge. A seek cannot do it: the served
                     * playlist accumulates (media/hls.c), so timestamp 0 is
                     * however far back the session started, not "the edge" —
                     * and the manifest response the app already holds mints
                     * a brand-new window. */
                    int was_paused = live_resume ? 0 : status.paused;
                    vt_log("hls live %s at %u ms -> fresh open at the edge",
                           live_resume ? "resume after a long pause"
                           : live_outage ? "outage" : "error",
                           (unsigned)status.position_ms);
                    live_resume = 0;
                    vt_decoder_destroy(player);
                    player = NULL;
                    player_source_term(&sources[cur]);
                    cancel = 0;
                    VtDecoderPlayer *nplayer = NULL;
                    ret = open_source_blocking(video->video_id, 1,
                                               &resolve.stream, quality,
                                               volume, was_paused, &cancel,
                                               &sources[cur], &nplayer);
                    reset_input(&previous, &gesture);
                    if (ret == 0 && nplayer && !cancel) {
                        player = nplayer;
                        live_join_ms = 0;
                        hud_visible = 1;
                        hud_deadline = sceKernelGetProcessTimeWide() +
                                       HUD_HIDE_US;
                        continue;
                    }
                    exit_code = -1;
                    break;
                }
                uint64_t retry_pos = vt_hls_segment_start_ms(
                    &sources[cur].hls_video, status.position_ms);
                SeekTask task = { player, retry_pos };
                int was_paused = status.paused;
                cancel = 0;
                ret = vt_ui_run_task(vt_str(S_RESUMING), seek_task, &task,
                                     &cancel);
                vt_log("hls error at %u ms -> session reopen at %llu ms "
                       "ret=%d",
                       (unsigned)status.position_ms,
                       (unsigned long long)retry_pos, ret);
                reset_input(&previous, &gesture);
                if (ret >= 0 && !cancel) {
                    if (was_paused) vt_decoder_set_paused(player, 1);
                    hud_visible = 1;
                    hud_deadline = sceKernelGetProcessTimeWide() + HUD_HIDE_US;
                    continue;
                }
            }
            if (vt_decoder_backend(player) == VT_DECODER_BACKEND_HARDWARE &&
                sources[cur].is_hls && resolve.stream.format_count > 0) {
                uint64_t resume_ms = status.position_ms;
                int was_paused = status.paused;
                vt_log("hls error at %u ms -> direct fallback",
                       (unsigned)resume_ms);
                vt_decoder_destroy(player);
                player = NULL;
                player_source_term(&sources[cur]);
                cancel = 0;
                VtDecoderPlayer *nplayer = NULL;
                ret = open_direct_blocking_at(&resolve.stream, quality,
                                              resume_ms, volume, was_paused,
                                              &cancel, &sources[cur],
                                              &nplayer);
                reset_input(&previous, &gesture);
                if (ret == 0 && nplayer && !cancel) {
                    player = nplayer;
                    is_hd = 0;
                    char detail[40], qlabel_tmp[24];
                    quality_label(0, &resolve.stream, quality, qlabel_tmp,
                                  sizeof qlabel_tmp);
                    snprintf(detail, sizeof detail, vt_str(S_CONTINUE_IN),
                             qlabel_tmp);
                    vt_ui_toast(vt_str(S_HD_PLAYBACK_BROKEN), detail, 100);
                    hud_visible = 1;
                    hud_deadline = sceKernelGetProcessTimeWide() + HUD_HIDE_US;
                    continue;
                }
                exit_code = -1;
                break;
            }
            /* A direct stream gets the same first stage: one reopen of the
             * hardware session at the failing position, through the seek
             * path every user seek already takes (a fresh Range cursor,
             * resume on the keyframe at or before the position). Dying on
             * the same spot again within 15 s means the stream itself is
             * the problem, and there is nothing further down to drop to —
             * the software decoder that used to take over here was removed
             * in 2026-09 after never playing a frame on a device. Only for
             * a session that is still open: a reopen that already failed
             * inside the engine (a user seek on a dead link) leaves it
             * closed, and a seek cannot bring a closed session back. */
            if (vt_decoder_backend(player) == VT_DECODER_BACKEND_HARDWARE &&
                !sources[cur].is_hls && status.opened &&
                (direct_error_retry_pos == UINT64_MAX ||
                 status.position_ms >= direct_error_retry_pos + 15000)) {
                direct_error_retry_pos = status.position_ms;
                progress_wall_us = now;
                SeekTask task = { player, status.position_ms };
                int was_paused = status.paused;
                cancel = 0;
                ret = vt_ui_run_task(vt_str(S_RESUMING), seek_task, &task,
                                     &cancel);
                vt_log("direct error at %u ms -> session reopen ret=%d",
                       (unsigned)task.position_ms, ret);
                reset_input(&previous, &gesture);
                if (ret >= 0 && !cancel) {
                    if (was_paused) vt_decoder_set_paused(player, 1);
                    hud_visible = 1;
                    hud_deadline = sceKernelGetProcessTimeWide() + HUD_HIDE_US;
                    continue;
                }
            } else {
                vt_log("%s error is terminal (%s)", backend_name,
                       status.opened ? "no further fallback"
                                     : "session already closed by a failed reopen");
            }
            /* A downloaded file that dies this early is not a stream having a
             * bad moment: reopening it at the same place has been tried and
             * there is no other source to go to. The wrapper tells the engine,
             * and plays the video from the network instead if it can.
             *
             * Not when the viewer pressed Circle at the reopen's spinner
             * ("Resuming"): the reopen was never given its chance, so nothing
             * says the file is bad, only that the viewer has left — and the
             * verdict wipes the download and starts streaming the video they
             * just walked out of. */
            if (local_at_start && !cancel) {
                vt_log("%s: downloaded file failed at %u ms",
                       video->video_id, (unsigned)status.position_ms);
                bad_local = 1;
            }
            exit_code = -1;
            break;
        }
        if (status.eof) break;

        int switch_active = hd_resolving || preparing;
        int interacted = 0;
        if (pressed & SCE_CTRL_CIRCLE) {
            if (hd_resolving) {
                /* Cancel stage 1 only — playback is untouched, nothing has
                 * been opened yet. */
                hd_job.cancel = 1;
            } else if (preparing) {
                /* Cancel stage 2 — same: playback is still untouched, the
                 * old source is still the one playing. */
                prep_job.cancel = 1;
            } else {
                break;
            }
        }
        int toggle_pause = 0;
        if (pressed & SCE_CTRL_CROSS) {
            toggle_pause = 1;
            interacted = 1;
        }
        if (pressed & SCE_CTRL_SQUARE) {
            fill_screen = !fill_screen;
            interacted = 1;
        }
        if (pressed & SCE_CTRL_TRIANGLE && !switch_active &&
            quality_pickable) {
            int current_entry = is_hd ? 0 : quality + 1;
            int chosen = quality_picker_run(player, fill_screen,
                                            &resolve.stream, current_entry);
            reset_input(&previous, &gesture);
            /* The picker ran its own loop for as long as the user browsed it,
             * so the frame's `now` is stale by exactly that. Everything below
             * dates a deadline from it — without this the label's window was
             * measured from before the menu even opened and had usually
             * expired by the time the switch it announces began. */
            now = sceKernelGetProcessTimeWide();
            if (chosen >= 0 && chosen != current_entry) {
                int new_hd = (chosen == 0);
                int new_quality = new_hd ? quality : chosen - 1;
                if (new_hd) {
                    hd_job = (HdResolveJob){
                        .video_id = video->video_id,
                        .cancel = 0, .done = 0, .result = -1,
                        .url = "", .hd_reason = "",
                    };
                    snprintf(switch_label, sizeof switch_label,
                             vt_str(S_SWITCHING_TO_HD));
                    switch_label_deadline = now + SWITCH_LABEL_SHOW_US;
                    switch_animated = 1;
                    if (pthread_create(&hd_thread, NULL, hd_resolve_worker,
                                       &hd_job) == 0) {
                        hd_resolving = 1;
                    } else {
                        vt_ui_toast(vt_str(S_QUALITY_SWITCH_FAILED), NULL, 120);
                    }
                } else {
                    /* Direct format: URL already known, so this skips
                     * straight to stage 2 (prepare) — the current source
                     * keeps playing normally while the new connection is
                     * opened in the background. */
                    const yt_format_t *f = &resolve.stream.formats[new_quality];
                    char qlabel_tmp[24];
                    quality_label(0, &resolve.stream, new_quality, qlabel_tmp,
                                 sizeof qlabel_tmp);
                    snprintf(switch_label, sizeof switch_label,
                             vt_str(S_SWITCHING_TO), qlabel_tmp);
                    switch_label_deadline = now + SWITCH_LABEL_SHOW_US;
                    switch_animated = 0;
                    prep_job = (PrepareJob){
                        .url = f->url, .exp_w = f->width, .exp_h = f->height,
                        .exp_fps = f->fps, .is_hls = 0,
                        .source_out = &sources[1 - cur],
                        .cancel = 0, .done = 0, .result = -1, .player = NULL,
                    };
                    prep_ns = 1 - cur;
                    prep_new_is_hd = 0;
                    prep_new_quality = new_quality;
                    if (pthread_create(&prep_thread, NULL, prepare_worker,
                                       &prep_job) == 0) {
                        preparing = 1;
                    } else {
                        vt_ui_toast(vt_str(S_QUALITY_SWITCH_FAILED), NULL, 120);
                    }
                }
            }
            hud_visible = 1;
            hud_deadline = now + HUD_HIDE_US;
            interacted = 1;
        }
        if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
            volume += (pressed & SCE_CTRL_UP) ? 10 : -10;
            if (volume < 0) volume = 0;
            if (volume > 300) volume = 300;
            vt_decoder_set_volume(player, volume);
            interacted = 1;
        }
        uint64_t seek_target = 0;
        int want_seek = 0;
        /* Nothing to seek within on a live stream: what the demuxer holds is
         * a window that keeps moving, and a position inside it is not a place
         * anyone can be sent back to. */
        int seekable = !switch_active && !is_live;
        if (seekable && (pressed & (SCE_CTRL_LEFT | SCE_CTRL_LTRIGGER))) {
            seek_target = status.position_ms > SEEK_STEP_MS
                        ? status.position_ms - SEEK_STEP_MS : 0;
            want_seek = 1;
        } else if (seekable &&
                  (pressed & (SCE_CTRL_RIGHT | SCE_CTRL_RTRIGGER))) {
            seek_target = status.position_ms + SEEK_STEP_MS;
            want_seek = 1;
        }
        /* Called every frame, flags or not: a drag that gets no event this
         * frame still has to report itself as running. */
        switch (vt_player_touch_step(touch_flags, &touch, hud_visible,
                                     seekable && status.duration_ms > 0,
                                     status.duration_ms, &gesture,
                                     &seek_target)) {
        case VT_PLAYER_TOUCH_TOGGLE_PAUSE:
            toggle_pause = 1;
            interacted = 1;
            break;
        case VT_PLAYER_TOUCH_SEEK:
            want_seek = 1;
            break;
        case VT_PLAYER_TOUCH_SCRUBBING:
        case VT_PLAYER_TOUCH_SHOW_HUD:
            /* Both just need the bar up and its timer restarted, which the
             * `interacted` block below does — without it the HUD would fade
             * out from under the finger halfway through a drag. */
            interacted = 1;
            break;
        case VT_PLAYER_TOUCH_NONE:
            break;
        }
        if (toggle_pause) {
            if (!status.paused) {
                paused_since_us = now;
                vt_decoder_set_paused(player, 1);
            } else if (is_live && sources[cur].is_hls &&
                       now - paused_since_us > LIVE_PAUSE_REOPEN_US) {
                /* Handled at the top of the next iteration, where the live
                 * recovery lives. */
                live_resume = 1;
            } else {
                vt_decoder_set_paused(player, 0);
            }
        }
        if (want_seek) {
            /* The viewer has moved on from the start: a failure from here on
             * is where they went, not necessarily the file's first seconds. */
            local_at_start = 0;
            /* Never ask the engine to resume AT the end: audio hits EOF
             * before its first grain and the session ends in a black frame
             * instead of showing the last second and finishing. */
            if (status.duration_ms > 1000 &&
                seek_target > status.duration_ms - 1000)
                seek_target = status.duration_ms - 1000;
            /* HLS resumes on segment boundaries and nowhere else, for a user
             * seek exactly as for a quality switch — see
             * vt_hls_segment_start_ms. */
            if (sources[cur].is_hls)
                seek_target = vt_hls_segment_start_ms(&sources[cur].hls_video,
                                                      seek_target);
            /* A seek reopens the SAME player's own session in place (unlike
             * a quality switch, it isn't "keep an old source alive"), so the
             * blocking spinner here is fine — nothing is left unserviced. */
            SeekTask task = { player, seek_target };
            cancel = 0;
            /* Cancellable: a seek is a network open, and on a dead link the
             * spinner used to be inescapable. Circle here means "I want
             * out" — the engine has closed the session by then. */
            int seek_ret = vt_ui_run_task(vt_str(S_SEEKING), seek_task, &task,
                                          &cancel);
            reset_input(&previous, &gesture);
            if (cancel) {
                vt_log("seek to %llu ms cancelled by the user",
                       (unsigned long long)seek_target);
                break;
            }
            /* A failed reopen is reported by the engine through status.error
             * (sticky), so the recovery ladder above picks it up next frame
             * — for HLS with the direct-format fallback, for a direct stream
             * as a clean end, since the session is closed by then and a seek
             * cannot reopen it. The log line is what makes it attributable
             * afterwards. */
            if (seek_ret < 0)
                vt_log("seek to %llu ms failed ret=%d",
                       (unsigned long long)seek_target, seek_ret);
            interacted = 1;
        }
        if (interacted || pressed) {
            hud_visible = 1;
            hud_deadline = now + HUD_HIDE_US;
        } else if (hud_visible && now > hud_deadline && !status.paused &&
                   !switch_active) {
            /* A switch in flight keeps the bar up. Not for the banner any
             * more — that draws on its own — but for the quality badge in
             * it, which is where the switch's result actually shows up. */
            hud_visible = 0;
        }

        vita2d_start_drawing();
        vita2d_clear_screen();
        vt_decoder_present(player, fill_screen);
        if (hud_visible) {
            vt_decoder_get_status(player, &status);
            char qlabel[40] = "";
            if (!offline)
                quality_label(is_hd, &resolve.stream, quality, qlabel,
                              sizeof qlabel);
            VtDecoderPlayerStatus hud_status = status;
            if (is_live)
                hud_status.position_ms = status.position_ms > live_join_ms
                                       ? status.position_ms - live_join_ms : 0;
            /* Offline says so where the decoder name would be: the viewer can
             * tell at a glance that nothing is being streamed. */
            draw_hud(video, &hud_status, volume,
                     offline ? vt_str(S_PLAYER_OFFLINE) : "HW", qlabel,
                     gesture.active ? (int64_t)gesture.target_ms : -1,
                     is_live, quality_pickable);
        }
        /* Outside the hud_visible block on purpose: stages 1 and 2 of a
         * switch run for seconds with the video still playing, and this is
         * the only thing on screen that says so. */
        if (switch_active || now < switch_label_deadline)
            draw_switch_overlay(switch_label, now, switch_animated);
        else if (stalled)
            draw_switch_overlay(vt_str(S_BUFFERING), now, 1);
        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vt_decoder_render_complete(player);
        vita2d_swap_buffers();
        sceKernelDelayThread(1000);
    }

    /* If stage 1 or 2 was still in flight, cancel and wait it out so its
     * background thread doesn't outlive this function's stack frame. Stage
     * 1 only ever touched the network, nothing else to clean up; stage 2
     * may have already allocated the new player/connection by the time it
     * notices the cancel, in which case that needs tearing down too since
     * it was never committed. */
    if (hd_resolving) {
        hd_job.cancel = 1;
        pthread_join(hd_thread, NULL);
    }
    if (preparing) {
        prep_job.cancel = 1;
        pthread_join(prep_thread, NULL);
        if (prep_job.result == 0) {
            vt_decoder_destroy(prep_job.player);
            player_source_term(&sources[prep_ns]);
        }
    }

    cancel = 1;
    vt_decoder_destroy(player);
    player_source_term(&sources[cur]);
    ui_touch_reset();
    /* Nothing was watched, and the wrapper has the message to give. */
    if (bad_local) return PLAY_LOCAL_BAD;
    /* Recorded on the way out, not on the way in. It only lands here if the
     * stream opened, so nothing unplayable is remembered as watched — and the
     * synchronous card write it costs happens where a stall does not show,
     * rather than in the frames the whole HD-first path exists to protect. */
    vt_store_history_add(video);
    if (exit_code < 0)
        vt_ui_toast(vt_str(S_PLAYBACK_ERROR), vt_str(S_VIDEO_ENDED), 120);
    return PLAY_ENDED;
}

/* --- public entry ---------------------------------------------------------- */

void player_screen_run(const yt_video_t *video) {
    /* Same size as the stream factory keeps, so a path that fits here always
     * fits there: a good file must not look bad because its name was cut. */
    char path[VT_LOCAL_STREAM_PATH_MAX];
    if (dl_local_path(video->video_id, path, sizeof path)) {
        /* The download wins over streaming: it starts at once, takes no
         * bandwidth, and works with Wi-Fi off. It is also not bracketed by
         * dl_playback_begin/end below — it uses no network, and holding the
         * engine back for it would stall the other downloads for nothing. */
        if (play_video(video, path) != PLAY_LOCAL_BAD) return;

        /* The file passed the engine's check and still would not play. Hand
         * it back so it can be fetched again, say so, and — if there is a
         * network — fall through to streaming, so the viewer still gets to
         * watch what they asked for. */
        dl_report_bad_file(video->video_id);
        int online = http_connected();
        vt_log("%s: downloaded file is bad, reported (%s)", video->video_id,
               online ? "streaming instead" : "offline, nothing to play");
        vt_ui_toast(vt_str(S_PLAYER_DL_DAMAGED),
                    vt_str(S_PLAYER_DL_DAMAGED_HINT), online ? 100 : 150);
        if (!online) return;
    }

    /* An online playback shares the connection with the download engine, which
     * holds its transfer for as long as this is between begin and end so a 720p
     * stream is not starved. The pair is here, around the one call, rather than
     * inside play_video: that function has a dozen ways out (give-ups before
     * the first frame, Circle at any spinner, the HLS-to-direct fallback, a
     * failed quality switch) and a begin without its end would leave every
     * download waiting for a playback that is long over. play_video returns
     * normally from every one of them, so this end always runs. */
    dl_playback_begin();
    play_video(video, NULL);
    dl_playback_end();
}
