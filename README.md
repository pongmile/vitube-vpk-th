# ViTube (VPK Download)

Native YouTube client for the PlayStation Vita. Talks directly to YouTube's
internal API — no companion server, no account requirement, no browser.

**Note:** this app was built largely through AI-assisted ("vibe coded")
development rather than a from-scratch handwritten codebase. It works, but is
early/experimental — **test at your own risk, no guarantees.** Proper
documentation and source will be published here when time allows.

## Features

- Home feed, search, channel uploads, subscriptions
- Optional Google login (device-code flow) for a personalized feed/subs —
  only a refresh token is stored, nothing else
- Watch history & recent searches, stored locally
- Live streams + on-demand playback up to 720p via HLS, using the Vita's
  hardware H.264 decoder (falls back to a progressive stream if HLS fails)
- Bilingual UI (DE/EN) + selectable content region (DE/AT/CH, US, UK, FR, ES, IT)
- Full touch + button controls in the player (scrub, volume, quality switch)

## Install

Download `ViTube.vpk` from the [Releases](../../releases) page and install it
with VitaShell (or any other installer) like any other homebrew — requires
HENkaku/Ensō. First launch creates `ux0:data/ViTube/`; delete that folder any
time to reset the app.

## License

GPL-3.0-only — see [LICENSE](LICENSE).
