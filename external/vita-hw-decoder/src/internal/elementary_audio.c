#include "internal/elementary_audio.h"

#include <pthread.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

#include "module_log.h"

/* Audio output uses a dedicated pthread worker (see the thread field's
 * comment in elementary_audio.h for why it must be a pthread). The stack
 * leaves headroom for demux and hardware AAC calls without coupling the
 * module to an app thread. */
#define ELEMENTARY_AUDIO_THREAD_STACK    0x40000
/* Vita AudioOut grain used by the hardware AAC output loop. */
#define ELEMENTARY_AUDIO_GRAIN 1024
/* Short wait while paused: it is not pacing, just to avoid spinning the CPU
 * while the thread stays alive without producing output. */
#define ELEMENTARY_AUDIO_PAUSE_DELAY_US (10 * 1000)
/* A resume discard-scan publishes no clock, so the picture is black while
 * it runs. One that outlives the app's own per-segment fetch ceiling (40 s)
 * is a failed audio seek, not a slow one: report it instead of sitting on
 * a healthy-looking black player forever. */
#define ELEMENTARY_AUDIO_DISCARD_LIMIT_US (45ULL * 1000000ULL)
/* One bad access unit costs 23 ms of silence; a dead master clock costs the
 * whole video. Give up only on a run of consecutive failures. */
#define ELEMENTARY_AUDIO_MAX_CONSECUTIVE_DECODE_FAILURES 10

/* Minimal AudioSpecificConfig reader — just enough to recognize explicitly
 * signaled SBR (HE-AAC). FFmpeg's mov demuxer reports such a track with the
 * SBR OUTPUT rate in codecpar (ff_mp4_read_dec_config_descr prefers
 * ext_sample_rate), but sceAudiodec wants the CORE rate together with
 * isSbr=1 — wiliwili (vitadec_audio.c) passes exactly the core rate from
 * this same structure. Feeding it the output rate instead made
 * sceAudiodecDecode reject every access unit of an HE-AAC video with
 * 0x807F0000 (API_FAIL) while plain AAC-LC played fine, since for LC the
 * two rates coincide. */
static const uint32_t asc_rates[13] = {
	96000, 88200, 64000, 48000, 44100, 32000, 24000,
	22050, 16000, 12000, 11025, 8000, 7350
};

typedef struct {
	const uint8_t *data;
	size_t size;
	size_t bit;
} asc_reader_t;

static uint32_t asc_bits(asc_reader_t *r, unsigned count) {
	uint32_t value = 0;
	while (count--) {
		size_t byte = r->bit >> 3;
		if (byte >= r->size) return 0; /* over-read: parse fails on rate 0 */
		value = (value << 1) |
		        ((r->data[byte] >> (7 - (r->bit & 7))) & 1u);
		r->bit++;
	}
	return value;
}

static uint32_t asc_rate(asc_reader_t *r) {
	uint32_t index = asc_bits(r, 4);
	if (index == 15) return asc_bits(r, 24);
	return index < 13 ? asc_rates[index] : 0;
}

/* Returns 0 and fills the outputs when the config parses; -1 otherwise.
 * ext_rate is 0 unless the config explicitly signals SBR (AOT 5) or
 * SBR+PS (AOT 29). */
static int parse_audio_specific_config(const uint8_t *data, size_t size,
	                               uint32_t *core_rate, uint32_t *ext_rate,
	                               uint32_t *chan_config) {
	if (!data || size < 2) return -1;
	asc_reader_t r = { data, size, 0 };
	uint32_t aot = asc_bits(&r, 5);
	if (aot == 31) return -1; /* escape values do not occur on YouTube */
	uint32_t rate = asc_rate(&r);
	uint32_t channels = asc_bits(&r, 4);
	if (!rate) return -1;
	*core_rate = rate;
	*chan_config = channels;
	*ext_rate = 0;
	if (aot == 5 || aot == 29)
		*ext_rate = asc_rate(&r);
	return 0;
}

static int first_audio_stream(const AVFormatContext *ctx) {
	for (unsigned int i = 0; ctx && i < ctx->nb_streams; i++) {
		if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
			return (int)i;
	}
	return -1;
}

static int parse_adts_header(const uint8_t *data, size_t size,
	                         uint32_t *channels, uint32_t *sample_rate) {
	static const uint32_t rates[13] = {
		96000, 88200, 64000, 48000, 44100, 32000, 24000,
		22050, 16000, 12000, 11025, 8000, 7350
	};
	if (!data || size < 7 || data[0] != 0xff || (data[1] & 0xf6) != 0xf0)
		return -1;
	unsigned rate_index = (data[2] >> 2) & 0x0f;
	unsigned channel_config = ((unsigned)(data[2] & 1) << 2) |
	                          ((unsigned)data[3] >> 6);
	if (rate_index >= 13 || channel_config < 1 || channel_config > 2)
		return -1;
	if (channels) *channels = channel_config;
	if (sample_rate) *sample_rate = rates[rate_index];
	return 0;
}

/* Reads the next packet of the selected audio track, silently discarding
 * packets belonging to other tracks that may be present in the same
 * AVFormatContext. Returns
 * 1 if *packet was filled in, 0 at EOF, <0 on error (including the abort due
 * to cancel: the interrupt_callback is already attached to the
 * AVFormatContext by the caller, see elementary_audio.h). */
static int read_audio_packet(ElementaryAudioState *st, AVPacket *packet) {
	if (st->prefetched_packet) {
		av_packet_move_ref(packet, st->prefetched_packet);
		av_packet_free(&st->prefetched_packet);
		return 1;
	}
	int ret;
	/* Arm the owner's read watchdog only while a read is in flight. */
	st->read_started_us = sceKernelGetProcessTimeWide();
	while ((ret = av_read_frame(st->demux, packet)) >= 0) {
		if (packet->stream_index == st->stream_index) break;
		av_packet_unref(packet);
	}
	st->read_started_us = 0;
	if (ret >= 0) return 1;
	return ret == AVERROR_EOF ? 0 : ret;
}

static void *vita_hw_elementary_audio_thread(void *argp) {
	ElementaryAudioState *st = (ElementaryAudioState *)argp;

	/* Above the video thread (65) and everything else in the process: this
	 * thread's whole duty cycle is a few ms of AAC decode per 23 ms grain,
	 * so it can starve nothing — but at the default priority it was the one
	 * being starved, by video decode bursts and TLS workers, and every such
	 * moment past one grain is an audible dropout (measured on hardware
	 * 2026-09-01: 30 underruns in 5½ min, worst 53 ms, none source-side).
	 * The audio clock is the playback master; the master must not wait. */
	sceKernelChangeThreadPriority(sceKernelGetThreadId(), 64);

	AVPacket *packet = av_packet_alloc();
	if (!packet) {
		st->had_error = 1;
		return NULL;
	}

	const AVRational stream_tb = st->demux->streams[st->stream_index]->time_base;
	const AVRational us_tb = { 1, 1000000 };
	int decode_attempts = 0;
	int consecutive_decode_failures = 0;
	uint64_t discard_started_us = 0;
	int last_applied_volume = -1;
	/* Diagnostics (see elementary_audio.h). last_return_us == 0 disarms the
	 * underrun check across pauses and the start gate, where a long gap is
	 * intentional, not a starvation. */
	uint64_t last_return_us = 0;
	const uint32_t grain_ms =
	    ELEMENTARY_AUDIO_GRAIN * 1000U /
	    (st->sample_rate ? st->sample_rate : 44100U);
	int64_t expected_pts_us = INT64_MIN;
	while (st->start_gate && !*st->start_gate && !*st->cancel)
		sceKernelDelayThread(1000);
	/* Floor the clock at the resume position before the discard-scan below:
	 * the video side may then present the resume keyframe right away instead
	 * of a black screen, and position_ms reads the seek target, not 0. */
	if (st->initial_position_ms > 0)
		st->played_until_ms = st->initial_position_ms > UINT32_MAX
		                    ? UINT32_MAX : (uint32_t)st->initial_position_ms;

	while (!*st->cancel) {
		int requested_volume = st->volume_percent;
		if (requested_volume != last_applied_volume && st->port >= 0) {
			last_applied_volume = requested_volume;
			int hardware_percent = requested_volume > 100 ? 100 : requested_volume;
			int hardware_volume = hardware_percent * SCE_AUDIO_VOLUME_0DB / 100;
			int volume[2] = { hardware_volume, hardware_volume };
			sceAudioOutSetVolume(st->port,
			                      SCE_AUDIO_VOLUME_FLAG_L_CH |
			                          SCE_AUDIO_VOLUME_FLAG_R_CH,
			                      volume);
		}
		if (st->paused) {
			/* No sceAudioOutOutput and no advancing of played_pts_us while
			 * paused (SS5 point 5): the thread stays alive and rechecks the
			 * state after a short interval. */
			last_return_us = 0;
			sceKernelDelayThread(ELEMENTARY_AUDIO_PAUSE_DELAY_US);
			continue;
		}

		int ret = read_audio_packet(st, packet);
		if (ret == 0) {
			st->eof = 1;
			break;
		}
		if (ret < 0) {
			/* If cancel is already raised, the error is almost certainly the
			 * interrupt_callback abort: it is not a decode failure, so we
			 * exit cleanly without had_error. */
			if (!*st->cancel) {
				log_printf("elementary_audio: av_read_frame -> %d", ret);
				st->had_error = 1;
			}
			break;
		}
		if (st->initial_position_ms > 0) {
			int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
			int64_t pts_ms = pts == AV_NOPTS_VALUE ? -1
			               : av_rescale_q(pts, stream_tb, (AVRational){ 1, 1000 });
			/* An AU without any timestamp is accepted: better a possibly
			 * early start than an unbounded scan. */
			if (pts_ms >= 0 && (uint64_t)pts_ms < st->initial_position_ms) {
				av_packet_unref(packet);
				uint64_t now_us = sceKernelGetProcessTimeWide();
				if (!discard_started_us) {
					discard_started_us = now_us;
				} else if (now_us - discard_started_us >
				           ELEMENTARY_AUDIO_DISCARD_LIMIT_US) {
					log_printf("elementary_audio: resume scan stuck at %lld ms, "
					           "wanted %llu ms",
					           (long long)pts_ms,
					           (unsigned long long)st->initial_position_ms);
					st->had_error = 1;
					break;
				}
				continue;
			}
			log_printf("elementary_audio: resume AU=%lld ms requested=%llu ms",
			           (long long)pts_ms,
			           (unsigned long long)st->initial_position_ms);
			st->initial_position_ms = 0;
		}

		VitaAacDecodedAudio decoded;
		memset(&decoded, 0, sizeof(decoded));
		int decode_ret = vita_hw_aac_decoder_decode(&st->decoder, packet->data,
		                                         (size_t)packet->size, &decoded);
		if (decode_ret < 0) {
			/* AAC has no inter-frame dependency: a rejected access unit (an
			 * oversized bit-reservoir peak, a truncated AU at a live segment
			 * splice, a transient sceAudiodec BUSY) is one 23 ms gap, so skip
			 * it. Only a RUN of failures means the stream or the decoder is
			 * gone. The log line stays on the first failure of a run and on
			 * giving up — the audio thread must not write the card per AU. */
			consecutive_decode_failures++;
			if (consecutive_decode_failures == 1 ||
			    consecutive_decode_failures >=
			        ELEMENTARY_AUDIO_MAX_CONSECUTIVE_DECODE_FAILURES)
				log_printf("elementary_audio: vita_hw_aac_decoder_decode -> %d "
				           "(attempt #%d, %d in a row)",
				           decode_ret, decode_attempts,
				           consecutive_decode_failures);
			av_packet_unref(packet);
			decode_attempts++;
			if (consecutive_decode_failures >=
			    ELEMENTARY_AUDIO_MAX_CONSECUTIVE_DECODE_FAILURES) {
				st->had_error = 1;
				break;
			}
			continue;
		}
		decode_attempts++;
		consecutive_decode_failures = 0;

		/* The pts must be read before av_packet_unref(): unref clears it. */
		int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
		av_packet_unref(packet);

		if (decode_ret == 0 || !decoded.pcm || decoded.frames == 0) {
			/* No samples produced for this packet: it is not an error
			 * (symmetry with the video decoder, SS5 point 2), we keep
			 * feeding packets. */
			continue;
		}
		/* A pause that lands after decode must not discard this access unit.
		 * The per-grain loop below holds the decoded PCM until resume. */
		if (*st->cancel) continue;

		/* The AudioOut port consumes exactly the grain chosen at open (1024
		 * samples/channel). SBR can make one AAC access unit produce 2048
		 * samples, which must be delivered as two consecutive grains; passing
		 * the base pointer only once would discard half the PCM while moving the
		 * clock by the full packet. AAC-LC/SBR are expected to be exact
		 * multiples here; fail closed on any other shape instead of corrupting
		 * sync silently. */
		if (decoded.frames % ELEMENTARY_AUDIO_GRAIN != 0) {
			log_printf("elementary_audio: PCM not aligned to output grain (%u frames)",
			          decoded.frames);
			st->had_error = 1;
			break;
		}
		/* Same 100..200% software boost as the direct player. This PCM buffer
		 * belongs to our decoder and isn't reused until the next decode, which
		 * happens only after all blocking AudioOut grains below have returned. */
		if (requested_volume > 100) {
			int16_t *pcm = (int16_t *)(uintptr_t)decoded.pcm;
			uint32_t samples = decoded.frames * decoded.channels;
			for (uint32_t i = 0; i < samples; i++) {
				int32_t value = (int32_t)pcm[i] * requested_volume / 100;
				if (value > INT16_MAX) value = INT16_MAX;
				else if (value < INT16_MIN) value = INT16_MIN;
				pcm[i] = (int16_t)value;
			}
		}
		int64_t pts_us = pts != AV_NOPTS_VALUE
		               ? av_rescale_q(pts, stream_tb, us_tb) : AV_NOPTS_VALUE;
		/* Source continuity: this AU should start where the previous one
		 * ended. YouTube live splices segments from an encoder that is not
		 * sample-exact; a jump here is a wobble that is IN the stream. */
		if (pts_us >= 0) {
			if (expected_pts_us != INT64_MIN) {
				int64_t off_by = pts_us - expected_pts_us;
				if (off_by < 0) off_by = -off_by;
				if (off_by > 10000) st->pts_jumps++;
			}
			expected_pts_us = pts_us + (int64_t)decoded.frames * 1000000LL /
			                  st->sample_rate;
		}
		for (uint32_t offset = 0; offset < decoded.frames;
		     offset += ELEMENTARY_AUDIO_GRAIN) {
			int waited = 0;
			while (st->paused && !*st->cancel) {
				sceKernelDelayThread(ELEMENTARY_AUDIO_PAUSE_DELAY_US);
				waited = 1;
			}
			if (waited) last_return_us = 0;
			if (*st->cancel) break;
			/* Underrun watch: the port holds roughly one grain of headroom
			 * past the blocking call, so preparation time beyond a grain's
			 * duration means it ran dry and the listener heard it. */
			uint64_t now_us = sceKernelGetProcessTimeWide();
			if (last_return_us) {
				uint32_t prep_ms =
				    (uint32_t)((now_us - last_return_us) / 1000ULL);
				if (prep_ms > grain_ms) {
					st->underruns++;
					if (prep_ms > st->worst_prep_ms)
						st->worst_prep_ms = prep_ms;
				}
			}
			const int16_t *grain = decoded.pcm + offset * st->decoder.channels;
			int output_ret = sceAudioOutOutput(st->port, grain);
			last_return_us = sceKernelGetProcessTimeWide();
			if (output_ret < 0) {
				log_printf("elementary_audio: sceAudioOutOutput -> 0x%08X",
				          (unsigned)output_ret);
				st->had_error = 1;
				break;
			}

			/* Publish only samples that AudioOut has accepted. */
			if (pts_us >= 0) {
				uint64_t end_us = (uint64_t)pts_us +
				    (uint64_t)(offset + ELEMENTARY_AUDIO_GRAIN) * 1000000ULL /
				    st->sample_rate;
				st->played_until_ms = end_us / 1000ULL > UINT32_MAX
				                         ? UINT32_MAX : (uint32_t)(end_us / 1000ULL);
			}
		}
		if (st->had_error || *st->cancel) break;
	}

	av_packet_free(&packet);
	return NULL;
}

int vita_hw_elementary_audio_start(ElementaryAudioState *st, AVFormatContext *demux,
                           int stream_index, uint64_t initial_position_ms,
                           volatile int *cancel,
                           volatile int *start_gate) {
	if (!st || !demux || !cancel) return -1;
	memset(st, 0, sizeof(*st));
	st->port = -1;
	st->demux = demux;
	st->cancel = cancel;
	st->start_gate = start_gate;
	st->initial_position_ms = initial_position_ms;
	st->volume_percent = 100;

	if (stream_index < 0) stream_index = first_audio_stream(demux);
	if (stream_index < 0 || (unsigned int)stream_index >= demux->nb_streams ||
	    demux->streams[stream_index]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
		log_printf("elementary_audio: no valid audio track (stream_index=%d)",
		          stream_index);
		return -1;
	}
	st->stream_index = stream_index;

	const AVCodecParameters *params = demux->streams[stream_index]->codecpar;
	uint32_t channels = params->ch_layout.nb_channels > 0
	                   ? (uint32_t)params->ch_layout.nb_channels : 0;
	uint32_t sample_rate = params->sample_rate > 0 ? (uint32_t)params->sample_rate : 0;
	uint32_t adts_core_rate = 0;
	/* MPEG-TS carries AAC as ADTS. FFmpeg can identify the stream while still
	 * leaving codecpar channels/rate empty after a short probe.
	 * Inspect one AU directly, retain it for the worker, and configure
	 * sceAudiodec for ADTS instead of rejecting an otherwise valid live. */
	if (params->codec_id == AV_CODEC_ID_AAC &&
	    (!channels || !sample_rate || params->extradata_size <= 0)) {
		AVPacket *probe = av_packet_alloc();
		int probe_ret = probe ? read_audio_packet(st, probe) : AVERROR(ENOMEM);
		if (probe_ret > 0) {
			uint32_t adts_channels = 0, adts_rate = 0;
			if (parse_adts_header(probe->data, (size_t)probe->size,
			                      &adts_channels, &adts_rate) == 0) {
				st->input_is_adts = 1;
				adts_core_rate = adts_rate;
				if (!channels) channels = adts_channels;
				if (!sample_rate) sample_rate = adts_rate;
				log_printf("elementary_audio: ADTS detected ch=%u rate=%u packet=%d",
				          adts_channels, adts_rate, probe->size);
			}
			st->prefetched_packet = probe;
		} else {
			av_packet_free(&probe);
			if ((!channels || !sample_rate) && probe_ret < 0)
				log_printf("elementary_audio: probe ADTS -> %d", probe_ret);
		}
	}
	if (!channels || !sample_rate) {
		log_printf("elementary_audio: missing codec parameters (ch=%u rate=%u)",
		          channels, sample_rate);
		av_packet_free(&st->prefetched_packet);
		return -1;
	}

	/* MP4 tracks carry an AudioSpecificConfig; explicit SBR changes what
	 * sceAudiodec has to be told (see parse_audio_specific_config). The
	 * AudioOut port and the playback clock keep the OUTPUT rate — with SBR
	 * active the decoder emits twice the core rate, which is exactly the
	 * container-declared rate again. */
	uint32_t decoder_rate = sample_rate;
	if (params->extradata && params->extradata_size >= 2) {
		uint32_t core = 0, ext = 0, cfg_channels = 0;
		if (parse_audio_specific_config(params->extradata,
		                                (size_t)params->extradata_size,
		                                &core, &ext, &cfg_channels) == 0 &&
		    ext && core) {
			decoder_rate = core;
			sample_rate = ext;
			if (cfg_channels >= 1 && cfg_channels <= 2)
				channels = cfg_channels;
			log_printf("elementary_audio: SBR signaled core=%u out=%u ch=%u",
			          core, ext, channels);
		}
	}
	/* HE-AAC over ADTS signals SBR implicitly: the ADTS header carries the
	 * core rate while FFmpeg's codecpar (when it has one) already carries
	 * the doubled output rate. Mirror the MP4 branch — core rate to
	 * sceAudiodec, output rate to the port and the clock — instead of
	 * handing the decoder the output rate with SBR on, which it rejects. */
	if (st->input_is_adts && adts_core_rate && adts_core_rate * 2 == sample_rate) {
		decoder_rate = adts_core_rate;
		log_printf("elementary_audio: ADTS SBR core=%u out=%u", decoder_rate,
		           sample_rate);
	}
	st->sample_rate = sample_rate;

	/* SBR state is selected inside vita_hw_aac_decoder_init_ex. */
	int ret = vita_hw_aac_decoder_init_ex(&st->decoder, channels, decoder_rate,
	                                  st->input_is_adts);
	if (ret < 0) {
		log_printf("elementary_audio: vita_hw_aac_decoder_init -> 0x%08X", (unsigned)ret);
		av_packet_free(&st->prefetched_packet);
		return ret;
	}
	st->decoder_ready = 1;

	int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, ELEMENTARY_AUDIO_GRAIN,
	                               (int)sample_rate,
	                               channels == 1 ? SCE_AUDIO_OUT_MODE_MONO
	                                             : SCE_AUDIO_OUT_MODE_STEREO);
	log_printf("elementary_audio: sceAudioOutOpenPort -> 0x%08X (ch=%u rate=%u)",
	          (unsigned)port, channels, sample_rate);
	if (port < 0) {
		vita_hw_aac_decoder_term(&st->decoder);
		st->decoder_ready = 0;
		av_packet_free(&st->prefetched_packet);
		return port;
	}
	st->port = port;
	st->port_open = 1;
	int volume[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
	sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
	                     volume);

	/* pthread_create, NOT sceKernelCreateThread: this thread takes pthread
	 * mutexes (the HTTPS prefetch lock on every demux read, the app's log
	 * lock), and PTE's contended path needs the caller's pthread TLS - a raw
	 * Sce thread crashes with a NULL-deref data abort on the first contended
	 * acquisition. All ten 2026-08-27 crash dumps were this thread dying
	 * there. See the thread field's comment in elementary_audio.h. */
	pthread_attr_t attr;
	ret = pthread_attr_init(&attr);
	int attr_ready = ret == 0;
	if (ret == 0)
		ret = pthread_attr_setstacksize(&attr, ELEMENTARY_AUDIO_THREAD_STACK);
	if (ret == 0)
		ret = pthread_create(&st->thread, &attr, vita_hw_elementary_audio_thread, st);
	if (attr_ready) pthread_attr_destroy(&attr);
	if (ret != 0) {
		log_printf("elementary_audio: pthread_create -> %d", ret);
		sceAudioOutReleasePort(st->port);
		st->port = -1;
		st->port_open = 0;
		vita_hw_aac_decoder_term(&st->decoder);
		st->decoder_ready = 0;
		av_packet_free(&st->prefetched_packet);
		return ret > 0 ? -ret : ret;
	}

	st->started = 1;
	return 0;
}

void vita_hw_elementary_audio_join(ElementaryAudioState *st) {
	if (!st) return;
	/* Each step is gated on its own state flag, not on the numeric value of
	 * the corresponding field: if vita_hw_elementary_audio_start() was never called
	 * (or failed) on a state zeroed by the caller, port set to 0 (or a
	 * zeroed pthread_t) would be indistinguishable from valid handles. */
	if (st->started) pthread_join(st->thread, NULL);
	if (st->port_open) {
		sceAudioOutReleasePort(st->port);
		st->port_open = 0;
	}
	if (st->decoder_ready) {
		/* vita_hw_aac_decoder_term() is not safe on a VitaAacDecoder that was
		 * never initialized: teardown_partial() reads es_memblock/pcm_memblock
		 * from it as already written by vita_hw_aac_decoder_init(), which on a
		 * zeroed struct would be 0 (an apparently valid memblock) instead of
		 * the expected -1 sentinel. decoder_ready is the only correct guard
		 * here. */
		vita_hw_aac_decoder_term(&st->decoder);
		st->decoder_ready = 0;
	}
	av_packet_free(&st->prefetched_packet);
	st->port = -1;
	st->started = 0;
}

uint64_t vita_hw_elementary_audio_clock_us(const ElementaryAudioState *st) {
	if (!st) return 0;
	return (uint64_t)st->played_until_ms * 1000ULL;
}

void vita_hw_elementary_audio_set_volume(ElementaryAudioState *st, int percent) {
	if (!st) return;
	if (percent < 0) percent = 0;
	if (percent > 300) percent = 300;
	st->volume_percent = percent;
}
