# ViTube third-party notices

ViTube itself is licensed under GPL-3.0-only (see `LICENSE`). The VPK
statically links the components below; their licence texts ship inside the
VPK under `licenses/` and the complete corresponding source is the ViTube
source archive that accompanies the VPK plus the pinned upstream sources
named here.

| Component | Licence | Source |
|-----------|---------|--------|
| vita-hw-decoder, vita-https (`external/`) | GPL-3.0-only | this archive |
| VitaWave ancestry of `src/media/vita_decoder.c` | GPL-3.0-only | this archive |
| FFmpeg n6.0, commit `ea3d24bbe3c58b171e55fe2151fc7ffaca3ab3d2`, configured without `--enable-gpl` | LGPL-2.1-or-later | https://github.com/FFmpeg/FFmpeg |
| wiliwili `h264_vita` backend, commit `88e5876bea9502d06f46a8656e3530684d3aaf7d` (patch sha256 `2d38529d10c74560db3909cf8c2f3e359b128c04fb29f6dd0085770ed81cef6c`), plus `external/vita-hw-decoder/tools/h264-vita-dpb.patch` and `h264-vita-internal-decode.patch`; the patched FFmpeg is therefore GPL-3.0 | GPL-3.0 | https://github.com/xfangfang/wiliwili |
| libcurl | curl licence | https://curl.se/docs/copyright.html |
| Mbed TLS 3.6.5 | Apache-2.0 | https://github.com/Mbed-TLS/mbedtls |
| zlib, Zstandard | zlib licence, BSD-3-Clause | https://zlib.net, https://github.com/facebook/zstd |
| certifi root-CA bundle (`external/vita-https/src/ca_bundle.c`) | MPL-2.0 | see `external/vita-https/CA_BUNDLE_PROVENANCE.md` |
| jansson (`external/jansson`) | MIT | https://github.com/akheron/jansson |
| DejaVu Sans (`assets/font.ttf`) | Bitstream Vera / DejaVu licence | `assets/FONT-LICENSE.txt` |
| Noto Emoji (`assets/emoji.ttf`) | OFL-1.1 | `assets/EMOJI-LICENSE.txt` |
| vita2d, VitaSDK | MIT / respective licences | https://vitasdk.org |

`external/vita-hw-decoder/tools/build-ffmpeg.sh` reproduces the patched
FFmpeg build and installs the exact upstream source archives next to the
generated libraries (`build/deps/ffmpeg-vita-hw/share/sources/`).
