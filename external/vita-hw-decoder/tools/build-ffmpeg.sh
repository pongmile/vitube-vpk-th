#!/usr/bin/env bash
set -euo pipefail

# Reproducible builder for the complete public h264_vita backend used by
# wiliwili. Generated sources/objects stay under build/ and are not vendored.

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -z "${VITASDK:-}" ]]; then
  echo "VITASDK is not defined" >&2
  exit 1
fi
vita_sdk="$VITASDK"
prefix="${VITA_HW_DECODER_FFMPEG_ROOT:-$repo_root/build/deps/ffmpeg-vita-hw}"
jobs="${VITAWAVE_FFMPEG_JOBS:-8}"

ffmpeg_commit="ea3d24bbe3c58b171e55fe2151fc7ffaca3ab3d2"
wiliwili_commit="88e5876bea9502d06f46a8656e3530684d3aaf7d"
patch_sha256="2d38529d10c74560db3909cf8c2f3e359b128c04fb29f6dd0085770ed81cef6c"

if [[ ! -x "$vita_sdk/bin/arm-vita-eabi-gcc" ]]; then
  echo "VitaSDK was not found at $vita_sdk" >&2
  exit 1
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/vita-hw-decoder-ffmpeg.XXXXXX")"
trap 'rm -rf "$work"' EXIT

git clone --quiet https://github.com/FFmpeg/FFmpeg.git "$work/ffmpeg"
git -C "$work/ffmpeg" checkout --quiet "$ffmpeg_commit"
git clone --quiet https://github.com/xfangfang/wiliwili.git "$work/wiliwili"
git -C "$work/wiliwili" checkout --quiet "$wiliwili_commit"

patch_file="$work/wiliwili/scripts/psv/ffmpeg/ffmpeg.patch"
if command -v sha256sum >/dev/null 2>&1; then
  actual_sha="$(sha256sum "$patch_file" | awk '{print $1}')"
else
  actual_sha="$(shasum -a 256 "$patch_file" | awk '{print $1}')"
fi
if [[ "$actual_sha" != "$patch_sha256" ]]; then
  echo "Unexpected wiliwili patch SHA-256: $actual_sha" >&2
  exit 1
fi

patch -d "$work/ffmpeg" -p1 < "$patch_file"

# On top of wiliwili's backend: size the SceVideodec DPB by the stream's
# LEVEL rather than its declared max_num_ref_frames. YouTube 720p25 HLS
# encodes declare 3 reference frames and a VUI reorder depth of 1 while
# actually running a 5-frame B-pyramid; a decoder sized for the declaration
# fails deterministically in sceAvcdecDecode once the reorder backlog
# overflows the pool it asked for (observed 2026-08-29, error after the
# same access unit on every run). See the comments inside the patch.
patch -d "$work/ffmpeg" -p1 < "$repo_root/tools/h264-vita-dpb.patch"

tar -C "$work" --exclude='ffmpeg/.git' -czf "$work/ffmpeg-n6.0-h264-vita-source.tar.gz" ffmpeg
git -C "$work/wiliwili" archive --format=tar.gz \
  --prefix=wiliwili-88e5876b/ -o "$work/wiliwili-88e5876b.tar.gz" HEAD

tool_prefix="$vita_sdk/bin/arm-vita-eabi-"
cd "$work/ffmpeg"
# The hls demuxer is enabled on purpose: YouTube only offers 720p as HLS or
# DASH, and ViTube hands the HLS master playlist to this decoder directly.
# It needs no network support here — hls_demuxer_select is only
# "adts_header ac3_parser mov_demuxer mpegts_demuxer" — because the app
# rewrites every playlist URI to a file: URL and resolves it through its own
# vita-https stack via AVFormatContext.io_open. Keep --disable-network.
#
# The aac DECODER is enabled even though this app never decodes AAC with it —
# the Vita's own audio engine does that. libavformat needs it anyway:
# avformat_find_stream_info() cannot complete an AAC stream's parameters
# without a probe decoder, because the ADTS parser deliberately refuses to fill
# sample_rate/channels for AAC (the header can lie about both under SBR). For
# YouTube's HLS audio rendition — ID3-timestamped raw ADTS, not MPEG-TS — that
# one missing piece cascades: no frame size means no packet duration, and
# without durations fill_timing_for_id3_timestamped_stream() gives up after the
# first access unit of every segment. Measured on the 720p path (2026-08-29):
# 9295 of 9322 audio packets carried no dts at all, av_seek_frame() failed, and
# the audio cursor was left 68 s into a 217 s video while the video cursor sat
# at 0 — audible as a hard desync and then a decode error, once the video
# thread started racing to catch up. Opening also pulled 12 segments (~1 MB)
# every time, since find_stream_info kept reading until probesize ran out.
# With the decoder linked in: every packet timestamped, seeks exact, two
# segments read.
env VITASDK="$vita_sdk" PATH="$vita_sdk/bin:/usr/bin:/bin" ./configure \
  --prefix="$prefix" \
  --enable-vita --target-os=vita --enable-cross-compile \
  --cross-prefix="$tool_prefix" \
  --ar="${tool_prefix}gcc-ar" \
  --ranlib="${tool_prefix}gcc-ranlib" \
  --nm="${tool_prefix}gcc-nm" \
  --disable-runtime-cpudetect --disable-armv5te \
  --extra-cflags="-O3 -flto -ffunction-sections -fdata-sections -Wno-error=incompatible-pointer-types -Wno-error=enum-int-mismatch" \
  --extra-ldflags="-flto -Wl,--gc-sections -L$vita_sdk/lib" \
  --disable-shared --enable-static \
  --disable-programs --disable-doc --disable-autodetect --disable-network \
  --disable-avfilter --disable-swscale --disable-swresample --disable-avdevice \
  --disable-encoders \
  --disable-decoders --enable-decoder=h264,h264_vita,aac \
  --disable-demuxers --enable-demuxer=mov,mpegts,aac,h264,hls \
  --disable-muxers --enable-muxer=mov,mp4,mpegts \
  --disable-parsers --enable-parser=aac,aac_latm,h264,mpeg4audio \
  --disable-protocols --enable-protocol=file \
  --disable-bsfs --disable-iconv --disable-lzma --disable-sdl2 --disable-xlib \
  --enable-pthreads

make -j"$jobs"
make install

# Keep the exact license text beside the generated static archives so the VPK
# can redistribute it without relying on a developer's temporary clone.
mkdir -p "$prefix/share/licenses" "$prefix/share/sources"
install -m 0644 "$work/ffmpeg/COPYING.LGPLv2.1" \
  "$prefix/share/licenses/FFmpeg-LGPL-2.1.txt"
install -m 0644 "$work/wiliwili/LICENSE" \
  "$prefix/share/licenses/wiliwili-GPL-3.0.txt"
install -m 0644 "$work/ffmpeg-n6.0-h264-vita-source.tar.gz" \
  "$prefix/share/sources/ffmpeg-n6.0-h264-vita-source.tar.gz"
install -m 0644 "$work/wiliwili-88e5876b.tar.gz" \
  "$prefix/share/sources/wiliwili-88e5876b.tar.gz"
install -m 0644 "$patch_file" \
  "$prefix/share/sources/wiliwili-ffmpeg.patch"

# Read each symbol table ONCE into a variable rather than piping nm straight
# into grep: grep -q stops at its first match, the write that nm attempts next
# gets SIGPIPE, and `set -o pipefail` then reports the whole pipeline as failed
# — turning a symbol that is present into a build failure. That is not
# hypothetical, it is how the hls check below first behaved (2026-08-29) on a
# libavformat.a that carried ff_hls_demuxer at entry 234 of 516; whether it
# fires at all depends on where in the archive the match happens to sit.
avcodec_symbols="$("${tool_prefix}gcc-nm" -g --defined-only \
  "$prefix/lib/libavcodec.a")"
avformat_symbols="$("${tool_prefix}gcc-nm" -g --defined-only \
  "$prefix/lib/libavformat.a")"

# The symbol is ff_h264_vita_decoder (AVCodec struct emitted by the wiliwili
# patch); an earlier revision of this check looked for a
# "ff_h264_vita_hw_decoder" that never existed and therefore only ever failed.
if ! grep -q 'ff_h264_vita_decoder' <<<"$avcodec_symbols"; then
  echo "h264_vita is not registered in libavcodec.a" >&2
  exit 1
fi

# ViTube plays YouTube's 720p through the HLS demuxer, so losing it silently
# would take HD down while everything else still builds.
if ! grep -q 'ff_hls_demuxer' <<<"$avformat_symbols"; then
  echo "the hls demuxer is not registered in libavformat.a" >&2
  exit 1
fi

# Losing the aac decoder is silent in exactly the same way, and costs the HLS
# audio cursor its timestamps rather than the whole demuxer (see the note above
# the configure call).
if ! grep -q 'ff_aac_decoder' <<<"$avcodec_symbols"; then
  echo "the aac decoder is not registered in libavcodec.a" >&2
  exit 1
fi

echo "FFmpeg h264_vita is ready at $prefix"
