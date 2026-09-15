#ifndef VITA_HW_DECODER_H
#define VITA_HW_DECODER_H

#include <stddef.h>
#include <stdint.h>

/* A factory must create a new independent cursor every time open() is called.
 * The player opens two cursors for a muxed file so audio and video can demux
 * concurrently without sharing seek state. */
typedef struct VitaHwDecoderStreamHandle {
	void *opaque;
	int (*read)(void *opaque, void *buffer, size_t size);
	int64_t (*seek)(void *opaque, int64_t offset, int whence);
	void (*close)(void *opaque);
	int64_t size;
	/* Optional. Set it when read() delivers an HLS playlist rather than a
	 * self-contained media file: FFmpeg then resolves the playlist's
	 * sub-resources (nested playlists, media segments) through this callback
	 * instead of through a protocol handler, of which this build has only
	 * "file" and no network layer whatsoever.
	 *
	 * The URLs arrive with whatever scheme the playlist carries, which the
	 * caller must have rewritten to "file:" — libavformat's hls demuxer
	 * rejects any URI whose protocol is not registered (it asks
	 * avio_find_protocol_name) long before io_open is consulted.
	 *
	 * Must return 0 and a malloc'd buffer the decoder frees with free(), or
	 * a negative value. Called from both cursor threads, so an implementation
	 * has to be safe for the opaque it was handed (each cursor gets its own
	 * handle from the factory). */
	int (*fetch)(void *opaque, const char *url, unsigned char **data,
	             size_t *size);
	/* Required alongside fetch(): the playlist's own URL, in the same
	 * rewritten scheme as the URIs inside it. libavformat's hls demuxer
	 * builds every playlist entry's absolute URL against the containing
	 * playlist's URL, and refuses to create the playlist at all when that
	 * base is empty — it surfaces as ENOMEM from avformat_open_input, which
	 * is a thoroughly misleading way to learn this. Must stay valid until
	 * the player is closed. */
	const char *url;
} VitaHwDecoderStreamHandle;

typedef struct VitaHwDecoderStreamFactory {
	void *opaque;
	int (*open)(void *opaque, VitaHwDecoderStreamHandle *out);
} VitaHwDecoderStreamFactory;

typedef struct VitaHwDecoderPlayer VitaHwDecoderPlayer;

typedef struct VitaHwDecoderPlayerConfig {
	VitaHwDecoderStreamFactory stream;
	/* Optional separate source for the audio cursor. A muxed file leaves this
	 * zeroed and both cursors read the same source, but YouTube's HLS carries
	 * video and audio as two independent playlists, and pointing each cursor
	 * at its own one keeps every byte fetched exactly once. */
	VitaHwDecoderStreamFactory stream_audio;
	uint32_t expected_width;
	uint32_t expected_height;
	int expected_fps;
	uint64_t start_position_ms;
	int volume_percent;
	/* Optional cooperative cancellation flag used during remote opens and
	 * decode. The caller must keep it alive until close returns. */
	volatile int *cancel_flag;
	/* When start_position_ms > 0: 0 (default) seeks frame-exactly, which for
	 * a remote/growing stream means discarding every packet from the
	 * backward-seeked keyframe up to the requested timestamp over the
	 * network before the first frame can even be decoded - can take
	 * seconds for a mid-GOP target. 1 accepts the first keyframe found
	 * instead (typically the very one the backward seek already landed
	 * on, so effectively no discard-scan at all): playback resumes up to
	 * one GOP interval earlier than requested, then catches up to the
	 * audio clock via the normal due-frame consumption in present() -
	 * CPU/decode-bound, not network-bound, so it is fast even though the
	 * seek itself is not frame-exact. Meant for callers that value a fast,
	 * approximate resume (e.g. switching quality mid-playback) over exact
	 * positioning (e.g. a user-initiated seek). */
	int approximate_seek;
	/* Open the session paused: no audio leaves the port and no frame is
	 * advanced until set_paused(0). For callers that preserve a pause across
	 * a reopen — open-then-pause leaks an audible burst in between. */
	int start_paused;
} VitaHwDecoderPlayerConfig;

typedef struct VitaHwDecoderPlayerStatus {
	int opened;
	int paused;
	int eof;
	int error;
	int hardware_accelerated;
	int direct_rendering;
	int ready_frames;
	int frame_capacity;
	int fps;
	uint32_t width;
	uint32_t height;
	uint64_t position_ms;
	uint64_t duration_ms;
	unsigned int frames_decoded;
	unsigned int frames_shown;
	unsigned int frames_dropped;
	/* Audio wobble diagnostics (see elementary_audio.h): port underruns with
	 * the worst preparation gap, and source-side timestamp jumps. */
	unsigned int audio_underruns;
	unsigned int audio_worst_prep_ms;
	unsigned int audio_pts_jumps;
} VitaHwDecoderPlayerStatus;

/* Convenience factory for a normal Vita path. The path must remain valid
 * until the player is closed. */
void vita_hw_decoder_file_stream_factory(const char *path,
	                             VitaHwDecoderStreamFactory *factory);

const char *vita_hw_decoder_backend_name(void);

VitaHwDecoderPlayer *vita_hw_decoder_create(void);
int vita_hw_decoder_open(VitaHwDecoderPlayer *player,
	                    const VitaHwDecoderPlayerConfig *config);
void vita_hw_decoder_close(VitaHwDecoderPlayer *player);
void vita_hw_decoder_destroy(VitaHwDecoderPlayer *player);

void vita_hw_decoder_set_paused(VitaHwDecoderPlayer *player, int paused);
void vita_hw_decoder_set_volume(VitaHwDecoderPlayer *player, int percent);
void vita_hw_decoder_request_stop(VitaHwDecoderPlayer *player);

/* Reopens both independent cursors at the requested media timestamp.
 *
 * Threading contract for seek/close/destroy (and open on a player that has
 * presented): they free GXM-mapped CDRAM and issue no GPU work of their own,
 * so the caller must have completed the previous present()'s rendering —
 * vita2d_wait_rendering_done() followed by render_complete() — before
 * calling them. They may then run on any thread. */
int vita_hw_decoder_seek(VitaHwDecoderPlayer *player, uint64_t position_ms);

/* Call present() inside a vita2d drawing scene, then render_complete() after
 * vita2d_wait_rendering_done(). */
int vita_hw_decoder_present(VitaHwDecoderPlayer *player, int fill_screen);
int vita_hw_decoder_present_rect(VitaHwDecoderPlayer *player,
	                            float x, float y, float width, float height,
	                            int fill_rect);
void vita_hw_decoder_render_complete(VitaHwDecoderPlayer *player);
void vita_hw_decoder_get_status(VitaHwDecoderPlayer *player,
	                           VitaHwDecoderPlayerStatus *status);

#endif
