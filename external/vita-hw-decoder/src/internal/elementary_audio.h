#ifndef VITA_HW_DECODER_ELEMENTARY_AUDIO_H
#define VITA_HW_DECODER_ELEMENTARY_AUDIO_H

#include <pthread.h>
#include <stdint.h>

#include <psp2/kernel/threadmgr.h>

#include <libavformat/avformat.h>

#include "internal/vita_aac_decoder.h"

/* Reads AAC packets from an AVFormatContext owned by the caller, decodes them
 * with sceAudiodec and sends PCM through blocking sceAudioOutOutput. The
 * submitted PTS is the player audio master clock. */

typedef struct {
	AVFormatContext *demux;   /* owned by the caller, NOT closed here */
	int stream_index;
	VitaAacDecoder decoder;
	volatile int *cancel;      /* shared with the caller, not owned */
	volatile int *start_gate;  /* decoder is ready, output starts only on GO */
	volatile int had_error;
	volatile int eof;
	/* Wall clock at which the thread entered av_read_frame(), 0 while no read
	 * is in flight; the owner's interrupt callback aborts a stuck read. */
	volatile uint64_t read_started_us;
	/* ARM32 can publish a 32-bit millisecond clock atomically; the previous
	 * volatile uint64_t could tear between the audio and render threads. */
	volatile uint32_t played_until_ms;
	/* Wobble diagnostics, counters only — the audio thread must never touch
	 * the log (a synchronous card write from here would itself be audible).
	 * underruns: the gap between one accepted grain and the next submission
	 * exceeded a grain's duration, so the port ran dry — an audible click or
	 * dropout of that length. worst_prep_ms: the longest such gap. pts_jumps:
	 * the source's own timestamps were not contiguous between access units
	 * (encoder splice at a live segment boundary) — audible as a wobble that
	 * no amount of buffering can prevent, because it is in the stream. */
	volatile uint32_t underruns;
	volatile uint32_t worst_prep_ms;
	volatile uint32_t pts_jumps;
	uint32_t sample_rate;
	AVPacket *prefetched_packet; /* first probed TS AU, consumed by the worker */
	int input_is_adts;
	volatile int paused;       /* the caller raises/lowers it, the thread honors it */
	volatile int volume_percent; /* shared 0..300 perceived volume */
	uint64_t initial_position_ms; /* progressive restart: discard earlier AUs */
	int port;
	/* MUST be a pthread, never a raw sceKernelCreateThread: this thread ends
	 * up inside PThread-Embedded synchronization (the HTTPS range cursor's
	 * prefetch mutex on every demux read, and any pthread mutex the app's
	 * log sink takes), and PTE's contended path (pte_osSemaphoreCancellable-
	 * Pend) dereferences the calling thread's VitaSDK pthread TLS - which a
	 * raw Sce thread does not have, so the first CONTENDED acquisition is a
	 * NULL-deref data abort. Confirmed the hard way: all ten real-hardware
	 * crash dumps of 2026-08-27 were this exact thread dying at that exact
	 * instruction. The video thread already runs as a pthread for the same
	 * reason (see software_video.c). */
	pthread_t thread;
	int decoder_ready; /* guard for vita_hw_aac_decoder_term(), see elementary_audio.c */
	int port_open;      /* guard for sceAudioOutReleasePort() */
	int started;        /* guard for pthread_join() */
} ElementaryAudioState;

/* Finds the first audio track in `demux` (if stream_index < 0) or uses the
 * one given, initializes the AAC decoder with the codecpar parameters and
 * starts the read/decode/output thread. Returns 0 on success, <0 on error (no
 * resource stays allocated on failure). */
int vita_hw_elementary_audio_start(ElementaryAudioState *st, AVFormatContext *demux,
                           int stream_index, uint64_t initial_position_ms,
                           volatile int *cancel,
                           volatile int *start_gate);

/* Waits for the thread to end (because of cancel/eof/had_error), closes the
 * audio port and the decoder. Safe even if vita_hw_elementary_audio_start() was never
 * called or failed (zeroed state). */
void vita_hw_elementary_audio_join(ElementaryAudioState *st);

/* Last pts (microseconds) actually sent to sceAudioOutOutput. Used by the
 * video thread as the synchronization clock. */
uint64_t vita_hw_elementary_audio_clock_us(const ElementaryAudioState *st);

void vita_hw_elementary_audio_set_volume(ElementaryAudioState *st, int percent);

#endif /* VITA_HW_DECODER_ELEMENTARY_AUDIO_H */
