# ViTube

A native YouTube client for the PlayStation Vita, written in C against
VitaSDK/vita2d. ViTube talks directly to YouTube's internal ("innertube")
JSON API, plays videos through the Vita's hardware decoders.

## Status

This app was built largely through vibe coding. It works, but
is early/experimental — **use at your own risk, no guarantees.**

**Download (prebuilt VPK):** see [Releases](../../releases) — install with
VitaShell or any other installer, requires HENkaku/Ensō.

**Version 2.0** removes a bundled system plugin whose hooks could crash
other apps with hardware video decoding after ViTube was closed — see
[Installing on the Vita](#installing-on-the-vita) for details and a note for
anyone updating from an older version.

License: GPL-3.0-only (see [`LICENSE`](LICENSE)). A full list of bundled
third-party components and their licenses is in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). This repository contains
the complete Corresponding Source for the published VPK (GPL-3.0 §6).

## Overview

ViTube runs as a single shell with four tabs — **Home**, **Search**,
**Subscriptions**, **More** — under a shared header/tab/toast bar, plus a
full-screen player opened from any video row (`src/ui/list_screen.c` is the
central router). For video lists, search, channels, and subscriptions the
app calls YouTube's official mobile/TV clients the same way the real apps do
internally; for playback, 720p is resolved directly through YouTube's own
HLS wherever possible, without an account and without routing through a
custom server.

## Features

- **Home feed, search, channel uploads, subscription feed** — all via
  YouTube's internal innertube API (`src/yt/innertube.c`), not the public
  YouTube Data API.
- **Login via Google's device-code flow** (`src/yt/oauth.c`): shows a code
  plus a URL confirmed on a second device; only the refresh token is
  persisted, under `ux0:data/ViTube/oauth.json`.
- **History, recent searches, channel sort order** — persisted locally in a
  single `state.json` (`src/store.c`), no cloud sync.
- **Live streams and on-demand 720p via HLS**, with hardware video decoding
  and automatic fallback to a progressive format if HLS fails for a given
  video (`src/media/hls.c`, `src/ui/player_quality_switch.c`).
- **Six-language UI** — German, English, Spanish, Brazilian Portuguese,
  Italian, French (`src/ui/strings.c`, one table per language in
  `src/ui/strings_*.c`) — plus a choice of content regions/languages
  (DE/AT/CH, US, UK, FR, ES, IT, BR, TH; `src/yt/region.c`), which affects
  titles, dates, and search results. On first launch the region follows the
  console language, and the UI language follows the region.
- **Thai text** — titles and channel names in Thai render with a bundled
  Noto Sans Thai. vita2d has no shaping engine, so `tools/make-thai-font.py`
  shapes every Thai cluster with HarfBuzz at build time and bakes the result
  into private-use glyphs that `src/ui/thai_shape.c` substitutes at runtime.
- **Touch and button controls** in the player (scrubbing, volume, quality
  selection) via `src/ui/player_touch.c` / `player_hud.c`.

## Architecture

### Project structure

| Path | Contents |
|---|---|
| `src/main.c`, `src/app.h` | Program entry point, global constants (data paths, screen size, color palette) |
| `src/store.c` / `.h` | Persistent local state: history, recent searches, channel sort order |
| `src/log.c` / `.h` | Append-only log file, also the app's single global I/O lock that serializes **every** `ux0:` file access |
| `src/ui/` | All screens/widgets: lists, player, settings, login, log viewer, text input, thumbnail cache, shared drawing toolkit (`ui.c`) |
| `src/yt/` | YouTube client: innertube requests, OAuth, JSON parsing, region/language selection |
| `src/media/` | Playback sources: HLS demuxing, progressive HTTPS streaming, facade over the hardware decoder |
| `src/net/` | Thin HTTP(S) wrapper over `vita-https` |
| `external/` | Vendored dependencies (see below) |
| `sce_sys/` | Contents of the VPK package: app icon |
| `assets/` | Fonts (DejaVu Sans, Noto Emoji, Noto Sans Thai) with their license texts |
| `tests/` | Host-side unit tests (see [Tests](#tests)) |

### App core & UI

`src/main.c` initializes logging, I/O, region/language, local state, and
networking, then hands off permanently to `list_screen_run()` — there is no
further higher-level event loop. `state.json` is written synchronously on
every change (history capped at 30 entries with move-to-front on rewatch,
searches capped at 10); screens detect changes cheaply via a revision
counter rather than content comparison. All `ux0:` file access — logging,
store, thumbnail cache, HLS prefetch — goes through the same lock in
`log.c`, after a real hardware crash (2026-08-27) was traced back to
concurrent, unserialized `SceIo` access.

### YouTube integration (`src/yt/`)

ViTube speaks YouTube's non-public innertube API — the same JSON API that
youtube.com and the official apps use internally. Since it isn't a public
API, the app impersonates three different YouTube clients depending on the
endpoint, because individual endpoints behave differently depending on
sign-in state:

- **ANDROID** — the default for search, "up next"/suggestions, and stream
  resolution.
- **TVHTML5** ("YouTube on TV") — for everything that uses the OAuth token
  (home feed while logged in, subscriptions, subscribe/unsubscribe), since
  the token is issued specifically for this client type.
- **VISIONOS** — the only client that (a) returns a real anonymous home feed
  and (b) returns an already-signed HLS manifest without ViTube having to
  solve YouTube's JavaScript `n`-parameter cipher — this is how 720p works
  without an account.

Responses are parsed by a single recursive `walk()` function in
`src/yt/parse.c` that recognizes all known renderer shapes (`videoRenderer`,
`tileRenderer`, `videoWithContextData`, `compactChannelData`, …) across all
three client types and fills fixed, heap-free structs (`src/yt/model.h`).
Login uses Google's OAuth2 device-code flow (`src/yt/oauth.c`); only the
refresh token is persisted, under `VITUBE_OAUTH_PATH`. The client ID/secret
pair used there isn't ViTube's own application — it's the publicly known
identity of Google's own "YouTube on TV" device type (the same one used in,
among others, yt-dlp and Kodi's YouTube add-on) — needed because innertube
only unlocks certain endpoints for official client types.

### Media playback (`src/media/`)

Two independent axes determine how a video is played:

1. **Source type** — a single, byte-range-readable file
   (`src/media/remote_stream.c`, for progressive formats) or an HLS
   playlist (`src/media/hls.c`, for live streams and on-demand 720p).
2. **Decoder engine** — hardware H.264 decoding via `h264_vita`/SceVideodec
   (vendored in `external/vita-hw-decoder`), mediated through
   `src/media/vita_decoder.c`. There is no longer a CPU software decoder: it
   was only reachable for direct formats, never produced a picture on any
   device, and doubled the cost of every engine fix (see the git history of
   `external/vita-sw-decoder`). The decoder calls the SceVideodec Internal
   API directly — it no longer depends on a bundled system-wide plugin (see
   [Installing on the Vita](#installing-on-the-vita)). If a hardware session
   dies, the player reopens it once at the same position; for HLS, playback
   then falls back to a direct progressive format.

HLS covers two YouTube manifest shapes, which `hls.c` distinguishes purely
by the presence of `#EXT-X-ENDLIST`: on-demand 720p (demuxed into separate
video/audio playlists, each loaded once) and live (a muxed MPEG-TS playlist
with a rolling window, loaded once and shared by both decoder cursors). A
dedicated worker-thread engine in `hls.c` prefetches upcoming segments into
a byte-bounded ring so the FFmpeg demux/audio-clock thread never blocks on
the network.

`src/ui/player_quality_switch.c` implements switching between HD (HLS) and
direct quality as a three-stage async operation (resolve → prepare →
commit), because the Vita can only ever have one hardware AAC decoder
instance alive at a time.

720p used to run through a self-hosted `yt-dlp` server, since removed from
the repository: the cookie session it needed kept expiring and made the
approach unreliable in practice, so it was dropped in favor of direct HLS
resolution.

### Networking (`src/net/`)

`src/net/http.c` is a thin wrapper over the vendored package
`external/vita-https`, which in turn provides a purpose-built curl+Mbed TLS
stack for the Vita (not the system VitaSDK curl/OpenSSL combo, whose
OpenSSL 1.0.2 port is end-of-life and GPLv3-incompatible). `vita-https`
enforces TLS 1.2 as a minimum, verifies peer and hostname by default against
a bundled Mozilla CA bundle, allows only `https://` URLs, and requires
explicit `sha256//` SPKI pinning for private/self-signed endpoints.

## Build

### Prerequisites

- An installed [VitaSDK](https://vitasdk.org) with the `VITASDK`
  environment variable set.
- CMake ≥ 3.16.
- The following two artifacts must exist before the top-level
  `CMakeLists.txt` can even configure (it aborts with `FATAL_ERROR`
  otherwise): a built FFmpeg library at
  `external/vita-hw-decoder/build/deps/ffmpeg-vita-hw/lib/libavcodec.a` and a
  built curl library at
  `external/vita-https/build/deps/curl-mbedtls/lib/libcurl.a`.

### 1. Build vendored dependencies

Both build scripts need to be run once with `VITASDK` set.

```sh
export VITASDK=/path/to/vitasdk

cd external/vita-hw-decoder && ./tools/build-ffmpeg.sh && cd -
cd external/vita-https      && ./tools/build-curl-mbedtls.sh && cd -
```

### 2. Build ViTube

```sh
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The result is `build/ViTube.vpk` — the packaging step (`vita_create_vpk` in
`CMakeLists.txt`) bundles the icon, all relevant license texts (GPLv3
§4/§6-compliant: ViTube's own GPL, `THIRD_PARTY_NOTICES.md`, FFmpeg's LGPL,
wiliwili's GPL patch, the CA bundle's MPL, font licenses), and the three fonts
from `assets/`.

### Installing on the Vita

Transfer the resulting `ViTube.vpk` to the Vita via VitaShell or any other
installer and install it. On first launch the app creates
`ux0:data/ViTube/` (login refresh token, history/settings, region/language,
HD preference) — delete that folder any time to reset the app to its
out-of-the-box state.

If you're updating from version 1.2 or earlier, reboot the console once
after installing. Those versions bundled the taiHEN plugin
`reAvPlayer.suprx`, whose system-wide hooks in the shared `SceAvcodecUser`
module stayed active after ViTube was closed from the LiveArea and crashed
other apps with hardware video decoding (VitaDB, for one). Since 2.0, ViTube
no longer needs the plugin — the decoder calls the SceVideodec functions
directly — but a hook left behind by an older version only clears on
reboot.

## Tests

`tests/` contains five host-side unit tests that **do not** run on the
Vita: `tests/stubs/` provides stand-in headers for `vita-https` and part of
the VitaSDK kernel headers so logic like HLS playlist/segment handling can
run on the build machine.

- `test_hls.c` — HLS playlist, segment, and cursor logic against a
  simulated HTTP server.
- `test_parse.c` — the innertube JSON parser against real, captured API
  responses in `tests/fixtures/`; must be run with `tests/` as the working
  directory, since fixtures are referenced relatively.
- `test_player_touch.c`, `test_strings.c` — touch-gesture math and parity of
  every language table against the German original (format strings, button
  glyphs, key names, lengths), respectively.
- `test_thai_shape.c` — the Thai cluster substitution against every entry of
  the generated table, plus pass-through and truncation cases.

There is **no** CMake/CTest target and no build script under `tests/`. Only
for `test_player_touch.c`, `test_strings.c` and `test_thai_shape.c` is the build command
documented directly in the file's header comment (a simple one-line `gcc`
invocation that links in the corresponding `src/` file). For `test_hls.c`
and `test_parse.c` there is no build command recorded in the repository.

## License & third-party notices

ViTube is licensed under **GPL-3.0-only** ([`LICENSE`](LICENSE)). A table of
all bundled/linked third-party components with license and source is in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) — among them the patched
FFmpeg build (LGPL base, brought to GPL-3.0 by wiliwili's `h264_vita`
hardware-decode patch), libcurl, Mbed TLS, jansson, and the bundled fonts.

`src/media/vita_decoder.c` descends from the
[VitaWave](https://github.com/spyro-98/VitaWave) project. VitaWave's own
source is not part of this repository (ViTube does not build it) — the
complete source is already public in the linked upstream repository.
