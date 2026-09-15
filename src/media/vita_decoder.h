/* Adapted for ViTube from VitaWave (c) spyro-98, GPL-3.0-only.
 * https://github.com/spyro-98/VitaWave */
#ifndef VITAWAVE_MEDIA_VITA_DECODER_H
#define VITAWAVE_MEDIA_VITA_DECODER_H

#include <stddef.h>
#include <stdint.h>

typedef struct VtDecoderStreamHandle {
	void *opaque;
	int (*read)(void *opaque, void *buffer, size_t size);
	int64_t (*seek)(void *opaque, int64_t offset, int whence);
	void (*close)(void *opaque);
	int64_t size;
	/* Optional: set when read() delivers an HLS playlist instead of a
	 * media file, so the demuxer resolves the playlist's segments through
	 * this instead of a protocol handler. See vita_hw_decoder.h for the
	 * contract. */
	int (*fetch)(void *opaque, const char *url, unsigned char **data,
	             size_t *size);
	/* Required alongside fetch(): the playlist's own URL in the rewritten
	 * scheme, used as the base for its entries. See vita_hw_decoder.h. */
	const char *url;
} VtDecoderStreamHandle;

typedef struct VtDecoderStreamFactory {
	void *opaque;
	int (*open)(void *opaque, VtDecoderStreamHandle *out);
} VtDecoderStreamFactory;

typedef enum VtDecoderBackend {
	VT_DECODER_BACKEND_NONE = 0,
	VT_DECODER_BACKEND_HARDWARE
} VtDecoderBackend;

typedef struct VtDecoderPlayer VtDecoderPlayer;

typedef struct VtDecoderPlayerConfig {
	VtDecoderStreamFactory stream;
	/* Optional separate source for the audio cursor — YouTube's HLS keeps
	 * video and audio in two playlists. Zeroed for a muxed file. */
	VtDecoderStreamFactory stream_audio;
	uint32_t expected_width;
	uint32_t expected_height;
	int expected_fps;
	uint64_t start_position_ms;
	int volume_percent;
	volatile int *cancel_flag;
	/* When start_position_ms > 0: 0 (default) seeks frame-exactly, which
	 * for a remote stream means discarding every packet from the seeked
	 * keyframe up to the requested timestamp over the network before the
	 * first frame can even be decoded - can take seconds for a mid-GOP
	 * target. 1 accepts the first keyframe found instead (typically
	 * exactly where the seek landed, so no discard-scan at all): playback
	 * resumes up to one GOP interval earlier than requested, then catches
	 * up to the audio clock quickly since that's decode-bound, not
	 * network-bound. Meant for a fast, approximate resume (e.g. switching
	 * quality mid-playback) rather than a user-initiated seek, where exact
	 * positioning matters more than speed. */
	int approximate_seek;
	/* Open paused: nothing is heard and no frame advances until
	 * vt_decoder_set_paused(player, 0). For reopens that preserve a pause —
	 * open-then-pause leaks an audible burst from the new position. */
	int start_paused;
} VtDecoderPlayerConfig;

typedef struct VtDecoderPlayerStatus {
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
	/* Audio wobble diagnostics: port underruns (each one an audible gap of
	 * ~worst_prep length), and jumps in the SOURCE's own audio timestamps (a
	 * wobble baked into the stream). */
	unsigned int audio_underruns;
	unsigned int audio_worst_prep_ms;
	unsigned int audio_pts_jumps;
} VtDecoderPlayerStatus;

void vt_decoder_file_stream_factory(const char *path,
	                                VtDecoderStreamFactory *factory);
/* Routes FFmpeg's warnings and errors into the device log. Once, at start. */
void vt_decoder_init(void);
VtDecoderPlayer *vt_decoder_create(void);
int vt_decoder_open(VtDecoderPlayer *player, const VtDecoderPlayerConfig *config);
void vt_decoder_destroy(VtDecoderPlayer *player);
void vt_decoder_set_paused(VtDecoderPlayer *player, int paused);
void vt_decoder_set_volume(VtDecoderPlayer *player, int percent);
int vt_decoder_seek(VtDecoderPlayer *player, uint64_t position_ms);
int vt_decoder_present(VtDecoderPlayer *player, int fill_screen);
void vt_decoder_render_complete(VtDecoderPlayer *player);
void vt_decoder_get_status(VtDecoderPlayer *player, VtDecoderPlayerStatus *status);
VtDecoderBackend vt_decoder_backend(const VtDecoderPlayer *player);
const char *vt_decoder_backend_name(const VtDecoderPlayer *player);

#endif
