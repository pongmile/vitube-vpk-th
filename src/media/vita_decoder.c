/* Adapted for ViTube from VitaWave (c) spyro-98, GPL-3.0-only.
 * https://github.com/spyro-98/VitaWave */
#include "media/vita_decoder.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>

#include <libavutil/log.h>

#include <vita_hw_decoder.h>

#include "log.h"

_Static_assert(sizeof(VtDecoderPlayerStatus) == sizeof(VitaHwDecoderPlayerStatus),
	           "hardware status ABI changed");

typedef struct LocalFileCursor {
	SceUID file;
} LocalFileCursor;

/* Hardware only. The CPU decoder that used to sit behind this facade as a
 * fallback (external/vita-sw-decoder, until 2026-09) was reachable for
 * direct progressive streams alone, never played a frame on a device, and
 * doubled every engine fix; a dead hardware session is reopened by the
 * player instead (player_screen.c, status.error branch). */
struct VtDecoderPlayer {
	VtDecoderBackend backend;
	VtDecoderStreamFactory source;
	VtDecoderStreamFactory source_audio;
	VitaHwDecoderPlayer *hardware;
};

static int local_read(void *opaque, void *buffer, size_t size) {
	LocalFileCursor *cursor = opaque;
	return cursor ? sceIoRead(cursor->file, buffer, size) : -1;
}

static int64_t local_seek(void *opaque, int64_t offset, int origin) {
	LocalFileCursor *cursor = opaque;
	return cursor ? sceIoLseek(cursor->file, offset, origin) : -1;
}

static void local_close(void *opaque) {
	LocalFileCursor *cursor = opaque;
	if (!cursor) return;
	if (cursor->file >= 0) sceIoClose(cursor->file);
	free(cursor);
}

static int local_open(void *opaque, VtDecoderStreamHandle *out) {
	const char *path = opaque;
	if (!path || !out) return -1;
	LocalFileCursor *cursor = calloc(1, sizeof(*cursor));
	if (!cursor) return -1;
	cursor->file = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (cursor->file < 0) {
		free(cursor);
		return -1;
	}
	int64_t size = sceIoLseek(cursor->file, 0, SCE_SEEK_END);
	if (size < 0 || sceIoLseek(cursor->file, 0, SCE_SEEK_SET) < 0) {
		local_close(cursor);
		return -1;
	}
	*out = (VtDecoderStreamHandle){ cursor, local_read, local_seek,
	                                local_close, size };
	return 0;
}

void vt_decoder_file_stream_factory(const char *path,
	                                VtDecoderStreamFactory *factory) {
	if (!factory) return;
	factory->opaque = (void *)path;
	factory->open = local_open;
}

static int hw_stream_open(void *opaque, VitaHwDecoderStreamHandle *out) {
	VtDecoderStreamFactory *factory = opaque;
	/* Zeroed, not just declared: a factory fills in the fields it uses and
	 * leaves the rest alone, so anything read out of an uninitialised cursor
	 * is stack garbage. That is not theoretical — shipping it with `cursor;`
	 * made every progressive stream crash on open, because a garbage fetch
	 * pointer made the decoder treat an MP4 as an HLS playlist. */
	VtDecoderStreamHandle cursor = {0};
	int result = factory && factory->open ? factory->open(factory->opaque, &cursor) : -1;
	if (result < 0) return result;
	out->opaque = cursor.opaque;
	out->read = cursor.read;
	out->seek = cursor.seek;
	out->close = cursor.close;
	out->size = cursor.size;
	out->fetch = cursor.fetch;
	out->url = cursor.url;
	return 0;
}

/* FFmpeg's own error lines were invisible on hardware: h264_vita logs the
 * exact SceVideodec/SceAvcdec status ("decode au failed 0x%x", "init library
 * failed 0x%x", "multiple SPS NALs") through av_log, and without a callback
 * all of it went nowhere — diagnosing the 2026-08-29 720p decode failure
 * meant guessing between half a dozen failure sites that each log a distinct
 * line here. Warnings and errors only, capped: vt_log writes ux0: files
 * synchronously, and a demuxer that decides to warn per packet must not turn
 * the log into the bottleneck. */
#define VT_FFMPEG_LOG_LIMIT 64
static int g_ffmpeg_log_count;

static void ffmpeg_log_bridge(void *ptr, int level, const char *fmt,
	                          va_list args) {
	if (level > AV_LOG_WARNING) return;
	if (g_ffmpeg_log_count >= VT_FFMPEG_LOG_LIMIT) return;
	char line[256];
	int print_prefix = 1;
	av_log_format_line(ptr, level, fmt, args, line, sizeof line,
	                   &print_prefix);
	size_t len = strlen(line);
	while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		line[--len] = '\0';
	if (!len) return;
	int count = __sync_fetch_and_add(&g_ffmpeg_log_count, 1);
	if (count >= VT_FFMPEG_LOG_LIMIT) return;
	vt_log("ffmpeg: %s%s", line,
	       count == VT_FFMPEG_LOG_LIMIT - 1 ? " (log limit reached)" : "");
}

int vt_decoder_prepare_hardware_runtime(void) {
	av_log_set_callback(ffmpeg_log_bridge);
	return vita_hw_decoder_prepare_runtime();
}

VtDecoderPlayer *vt_decoder_create(void) {
	return calloc(1, sizeof(VtDecoderPlayer));
}

int vt_decoder_open(VtDecoderPlayer *player, const VtDecoderPlayerConfig *config) {
	if (!player || !config || !config->stream.open) return -1;
	player->source = config->stream;
	player->source_audio = config->stream_audio;
	VitaHwDecoderPlayerConfig hardware = {
		.stream = { &player->source, hw_stream_open },
		.stream_audio = config->stream_audio.open
		              ? (VitaHwDecoderStreamFactory){ &player->source_audio,
		                                              hw_stream_open }
		              : (VitaHwDecoderStreamFactory){ 0 },
		.expected_width = config->expected_width,
		.expected_height = config->expected_height,
		.expected_fps = config->expected_fps,
		.start_position_ms = config->start_position_ms,
		.volume_percent = config->volume_percent,
		.cancel_flag = config->cancel_flag,
		.approximate_seek = config->approximate_seek,
		.start_paused = config->start_paused
	};
	player->hardware = vita_hw_decoder_create();
	int result = player->hardware
	           ? vita_hw_decoder_open(player->hardware, &hardware) : -1;
	if (result >= 0) {
		player->backend = VT_DECODER_BACKEND_HARDWARE;
		return 0;
	}
	if (player->hardware) vita_hw_decoder_destroy(player->hardware);
	player->hardware = NULL;
	return result;
}

void vt_decoder_destroy(VtDecoderPlayer *player) {
	if (!player) return;
	if (player->hardware) vita_hw_decoder_destroy(player->hardware);
	free(player);
}

void vt_decoder_set_paused(VtDecoderPlayer *player, int paused) {
	if (!player || player->backend != VT_DECODER_BACKEND_HARDWARE) return;
	vita_hw_decoder_set_paused(player->hardware, paused);
}

void vt_decoder_set_volume(VtDecoderPlayer *player, int percent) {
	if (!player || player->backend != VT_DECODER_BACKEND_HARDWARE) return;
	vita_hw_decoder_set_volume(player->hardware, percent);
}

int vt_decoder_seek(VtDecoderPlayer *player, uint64_t position_ms) {
	if (!player || player->backend != VT_DECODER_BACKEND_HARDWARE) return -1;
	return vita_hw_decoder_seek(player->hardware, position_ms);
}

int vt_decoder_present(VtDecoderPlayer *player, int fill_screen) {
	if (!player || player->backend != VT_DECODER_BACKEND_HARDWARE) return -1;
	return vita_hw_decoder_present(player->hardware, fill_screen);
}

void vt_decoder_render_complete(VtDecoderPlayer *player) {
	if (!player || player->backend != VT_DECODER_BACKEND_HARDWARE) return;
	vita_hw_decoder_render_complete(player->hardware);
}

void vt_decoder_get_status(VtDecoderPlayer *player, VtDecoderPlayerStatus *status) {
	if (!status) return;
	memset(status, 0, sizeof(*status));
	if (!player || player->backend != VT_DECODER_BACKEND_HARDWARE) return;
	VitaHwDecoderPlayerStatus source;
	vita_hw_decoder_get_status(player->hardware, &source);
	memcpy(status, &source, sizeof(source));
	status->hardware_accelerated = 1;
}

VtDecoderBackend vt_decoder_backend(const VtDecoderPlayer *player) {
	return player ? player->backend : VT_DECODER_BACKEND_NONE;
}

const char *vt_decoder_backend_name(const VtDecoderPlayer *player) {
	return player && player->backend == VT_DECODER_BACKEND_HARDWARE ? "hardware"
	                                                                : "none";
}
