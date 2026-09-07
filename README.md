# ViTube

Ein nativer YouTube-Client für die PlayStation Vita, geschrieben in C gegen
VitaSDK/vita2d. ViTube spricht direkt mit YouTubes internem ("innertube")
JSON-API, spielt Videos über die Vita-Hardware-Decoder ab und braucht dafür
keinen eigenen Server, kein Konto und keinen Browser.

## Status

Diese App wurde größtenteils per "Vibe Coding" (KI-gestützt, iterativ) gebaut,
nicht komplett von Hand geschrieben. Sie funktioniert, ist aber
früh/experimentell — **Nutzung auf eigene Gefahr, keine Garantie.**
Ausführlichere Dokumentation folgt bei Gelegenheit.

**Download (fertiges VPK):** siehe [Releases](../../releases) — Installation
per VitaShell oder einem beliebigen anderen Installer, benötigt
HENkaku/Ensō.

Lizenz: GPL-3.0-only (siehe [`LICENSE`](LICENSE)). Eine vollständige Liste
aller mitgelieferten Fremdkomponenten und deren Lizenzen steht in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Dieses Repository enthält
den vollständigen Corresponding Source zum veröffentlichten VPK (GPL-3.0 §6).

## Überblick

ViTube läuft als eine einzige Shell mit vier Tabs — **Start**, **Suche**,
**Abos**, **Mehr** — unter einer gemeinsamen Kopf-, Tab- und Hinweisleiste,
plus einem Vollbild-Player, der aus jeder Videozeile heraus geöffnet wird
(`src/ui/list_screen.c` ist der zentrale Router). Für Videolisten, Suche,
Kanäle und Abos ruft die App YouTubes offizielle mobile/TV-Clients so auf,
wie es die echten Apps intern tun; für die Wiedergabe wird 720p wo möglich
direkt über YouTubes eigenes HLS aufgelöst, ohne Account und ohne
Umweg über einen eigenen Server.

## Funktionen

- **Start-Feed, Suche, Kanal-Uploads, Abo-Feed** — alles über YouTubes
  internes innertube-API (`src/yt/innertube.c`), nicht über die öffentliche
  YouTube-Data-API.
- **Login per Google-Gerätecode-Flow** (`src/yt/oauth.c`): Anzeige eines
  Codes plus einer URL, die auf einem zweiten Gerät bestätigt wird; es wird
  dauerhaft nur der Refresh-Token unter `ux0:data/ViTube/oauth.json`
  gespeichert.
- **Verlauf, letzte Suchanfragen, Kanal-Sortierung** — lokal persistiert in
  einer einzigen `state.json` (`src/store.c`), ohne Cloud-Sync.
- **Live-Streams und On-Demand-720p über HLS**, mit Hardware-Videodecoding
  und automatischem Fallback auf ein progressives Format, falls HLS für ein
  Video scheitert (`src/media/hls.c`, `src/ui/player_quality_switch.c`).
- **Zweisprachige Oberfläche** (Deutsch/Englisch, `src/ui/strings.c`) sowie
  eine Auswahl an Inhalts-Regionen/-Sprachen (DE/AT/CH, US, UK, FR, ES, IT;
  `src/yt/region.c`), die Titel, Datumsangaben und Suchergebnisse
  beeinflusst.
- **Touch- und Tasten-Bedienung** im Player (Scrubben, Lautstärke,
  Qualitätswahl) über `src/ui/player_touch.c` / `player_hud.c`.

## Architektur

### Projektstruktur

| Pfad | Inhalt |
|---|---|
| `src/main.c`, `src/app.h` | Programmstart, globale Konstanten (Datenpfade, Bildschirmgröße, Farbpalette) |
| `src/store.c` / `.h` | Persistenter lokaler Zustand: Verlauf, letzte Suchen, Kanal-Sortierung |
| `src/log.c` / `.h` | Anhängendes Logfile, außerdem der einzige globale I/O-Lock, der **jeden** `ux0:`-Dateizugriff der App serialisiert |
| `src/ui/` | Alle Bildschirme/Widgets: Listen, Player, Einstellungen, Login, Log-Ansicht, Text-Eingabe, Thumbnail-Cache, gemeinsames Zeichen-Toolkit (`ui.c`) |
| `src/yt/` | YouTube-Client: innertube-Requests, OAuth, JSON-Parsing, Regions-/Sprachwahl |
| `src/media/` | Wiedergabe-Quellen: HLS-Demuxing, progressives HTTPS-Streaming, Fassade über den Hardware-Decoder |
| `src/net/` | Dünner HTTP(S)-Wrapper über `vita-https` |
| `external/` | Vendorte Abhängigkeiten (siehe unten) |
| `sce_modules/`, `sce_sys/` | Inhalte der VPK-Verpackung: System-Plugin `reAvPlayer.suprx`, App-Icon |
| `assets/` | Schriftarten (DejaVu Sans, Noto Emoji) samt Lizenztexten |
| `tests/` | Host-seitige Unit-Tests (siehe [Tests](#tests)) |

### App-Kern & UI

`src/main.c` initialisiert Logging, Ein-/Ausgabe, Region/Sprache, lokalen
Zustand und Netzwerk und übergibt dann dauerhaft an
`list_screen_run()` — es gibt keine weitere Ereignisschleife auf höherer
Ebene. `state.json` wird synchron bei jeder Änderung geschrieben (Verlauf
maximal 30 Einträge mit Move-to-Front bei erneutem Ansehen, Suchen maximal
10); Bildschirme erkennen Änderungen billig über einen Revisions-Zähler statt
über Inhaltsvergleich. Sämtlicher `ux0:`-Dateizugriff — Logging, Store,
Thumbnail-Cache, HLS-Prefetch — läuft durch denselben Lock in `log.c`,
nachdem ein realer Hardware-Crash (2026-08-27) auf nebenläufigen,
unserialisierten `SceIo`-Zugriff zurückgeführt wurde.

### YouTube-Anbindung (`src/yt/`)

ViTube spricht YouTubes nicht-öffentliches innertube-API — dasselbe JSON-API,
das youtube.com und die offiziellen Apps intern verwenden. Da es kein
öffentliches API ist, gibt sich die App wechselnd als drei verschiedene
YouTube-Clients aus, weil einzelne Endpunkte je nach Anmeldestatus
unterschiedlich reagieren:

- **ANDROID** — Standard für Suche, "Nächstes Video"/Vorschläge und
  Stream-Auflösung.
- **TVHTML5** ("YouTube on TV") — für alles, was den OAuth-Token benutzt
  (Start-Feed mit Login, Abos, Subscribe/Unsubscribe), da der Token genau auf
  diesen Client-Typ ausgestellt ist.
- **VISIONOS** — der einzige Client, der (a) einen echten anonymen Start-Feed
  liefert und (b) ein bereits signiertes HLS-Manifest zurückgibt, ohne dass
  ViTube YouTubes JavaScript-`n`-Parameter-Cipher lösen müsste — das ist der
  Weg, über den 720p ohne Account funktioniert.

Antworten werden von einer einzigen rekursiven `walk()`-Funktion in
`src/yt/parse.c` geparst, die alle bekannten Renderer-Formen (`videoRenderer`,
`tileRenderer`, `videoWithContextData`, `compactChannelData`, …) über alle
drei Client-Typen hinweg erkennt und in feste, heap-freie Structs
(`src/yt/model.h`) füllt. Login läuft über Googles OAuth2-Gerätecode-Flow
(`src/yt/oauth.c`); persistiert wird ausschließlich der Refresh-Token unter
`VITUBE_OAUTH_PATH`. Die dafür verwendete Client-ID/Secret-Kombination in
`oauth.c` ist keine eigene Anwendung, sondern die öffentlich bekannte
Identität von Googles eigenem "YouTube on TV"-Gerätetyp (dieselbe, die u. a.
in yt-dlp und Kodis YouTube-Addon verwendet wird) — nötig, weil innertube
bestimmte Endpunkte nur für offizielle Client-Typen freischaltet.

### Medienwiedergabe (`src/media/`)

Zwei unabhängige Achsen bestimmen, wie ein Video abgespielt wird:

1. **Quelltyp** — eine einzelne, per Byte-Range lesbare Datei
   (`src/media/remote_stream.c`, für progressive Formate) oder eine
   HLS-Playlist (`src/media/hls.c`, für Live-Streams und On-Demand-720p).
2. **Decoder-Engine** — Hardware-H.264-Decoding über `h264_vita`/SceVideodec
   (vendort in `external/vita-hw-decoder`), vermittelt durch
   `src/media/vita_decoder.c`. Einen CPU-Software-Decoder gibt es nicht
   mehr: er war nur für Direktformate erreichbar, hat auf keinem Gerät je
   ein Bild geliefert und kostete jeden Engine-Fix doppelt (Git-Historie
   von `external/vita-sw-decoder`). Stirbt eine Hardware-Session, öffnet
   der Player sie einmal an derselben Position neu; bei HLS weicht die
   Wiedergabe danach auf ein direktes progressives Format aus.

HLS deckt zwei YouTube-Manifestformen ab, die `hls.c` allein am Vorhandensein
von `#EXT-X-ENDLIST` unterscheidet: On-Demand-720p (demuxt in getrennte
Video-/Audio-Playlists, je einmal geladen) und Live (eine gemuxte
MPEG-TS-Playlist mit rollierendem Fenster, einmal geladen und für beide
Decoder-Cursor gemeinsam genutzt). Ein eigenes Worker-Thread-Engine in
`hls.c` lädt kommende Segmente vorab in einen bytebegrenzten Ring, damit der
FFmpeg-Demux-/Audiotakt-Thread nie auf das Netzwerk wartet.

`src/ui/player_quality_switch.c` implementiert den Wechsel zwischen HD (HLS)
und Direktqualität als dreistufigen asynchronen Vorgang (Auflösen →
Vorbereiten → Übernehmen), weil auf der Vita immer nur eine
Hardware-AAC-Decoder-Instanz gleichzeitig existieren kann.

720p lief davor über einen selbst gehosteten, mittlerweile aus dem Repository
entfernten `yt-dlp`-Server: die dafür nötige Cookie-Session lief immer wieder
ab und machte den Ansatz im Betrieb unzuverlässig, weshalb er zugunsten der
direkten HLS-Auflösung aufgegeben wurde.

### Netzwerk (`src/net/`)

`src/net/http.c` ist ein dünner Wrapper über das vendorte Paket
`external/vita-https`, das seinerseits einen eigens gebauten
curl+Mbed-TLS-Stack für die Vita bereitstellt (nicht die
System-VitaSDK-curl/OpenSSL-Kombination, deren OpenSSL-1.0.2-Port
End-of-Life und GPLv3-inkompatibel ist). `vita-https` erzwingt TLS 1.2 als
Minimum, verifiziert Peer und Hostname standardmäßig gegen ein mitgeliefertes
Mozilla-CA-Bundle, erlaubt nur `https://`-URLs und verlangt für private/
selbstsignierte Gegenstellen ein explizites `sha256//`-SPKI-Pinning.

## Build

### Voraussetzungen

- Ein installiertes [VitaSDK](https://vitasdk.org) mit gesetzter
  `VITASDK`-Umgebungsvariable.
- CMake ≥ 3.16.
- Die drei folgenden Artefakte müssen existieren, bevor sich das
  Top-Level-`CMakeLists.txt` überhaupt konfigurieren lässt (es bricht sonst
  mit `FATAL_ERROR` ab): eine gebaute FFmpeg-Bibliothek unter
  `external/vita-hw-decoder/build/deps/ffmpeg-vita-hw/lib/libavcodec.a`,
  eine gebaute curl-Bibliothek unter
  `external/vita-https/build/deps/curl-mbedtls/lib/libcurl.a`, sowie
  `sce_modules/reAvPlayer.suprx` (liegt bereits im Repository).

### 1. Vendorte Abhängigkeiten bauen

Beide Build-Skripte müssen einmalig mit gesetzter `VITASDK`-Variable
laufen.

```sh
export VITASDK=/pfad/zu/vitasdk

cd external/vita-hw-decoder && ./tools/build-ffmpeg.sh && cd -
cd external/vita-https      && ./tools/build-curl-mbedtls.sh && cd -
```

### 2. ViTube bauen

```sh
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Das Ergebnis ist `build/ViTube.vpk` — die Verpackung
(`vita_create_vpk` in `CMakeLists.txt`) bündelt Icon, das Plugin
`reAvPlayer.suprx`, alle relevanten Lizenztexte (GPLv3 §4/§6-konform: eigene
GPL, `THIRD_PARTY_NOTICES.md`, FFmpeg-LGPL, wiliwilis GPL-Patch,
CA-Bundle-MPL, Font-Lizenzen) sowie die beiden Schriftarten aus `assets/`.

### Installation auf der Vita

Die entstandene `ViTube.vpk` per VitaShell oder einem beliebigen anderen
Installer auf die Vita übertragen und installieren. Beim ersten Start legt
die App `ux0:data/ViTube/` an (Login-Refresh-Token, Verlauf/Einstellungen,
Region/Sprache, HD-Präferenz) — dieser Ordner lässt sich löschen, um die App
in den Auslieferungszustand zurückzusetzen.

## Tests

`tests/` enthält vier host-seitige Unit-Tests, die **nicht** auf der Vita
laufen: `tests/stubs/` stellt Ersatz-Header für `vita-https` und einen Teil
der VitaSDK-Kernel-Header bereit, damit sich Logik wie das HLS-Playlist-/
Segment-Handling auf der Build-Maschine ausführen lässt.

- `test_hls.c` — HLS-Playlist-, Segment- und Cursor-Logik gegen einen
  simulierten HTTP-Server.
- `test_parse.c` — der innertube-JSON-Parser gegen echte, mitgeschnittene
  API-Antworten in `tests/fixtures/`; muss mit `tests/` als Arbeitsverzeichnis
  ausgeführt werden, da die Fixtures relativ referenziert werden.
- `test_player_touch.c`, `test_strings.c` — Touch-Gesten-Mathematik bzw.
  Format-String-Parität zwischen der deutschen und englischen
  Textstelle-Tabelle.

Es gibt **kein** CMake/CTest-Ziel und kein Build-Skript unter `tests/`. Nur
für `test_player_touch.c` und `test_strings.c` ist der Build-Befehl direkt im
Datei-Kopfkommentar dokumentiert (ein einfacher `gcc`-Einzeiler, der die
jeweilige `src/`-Datei mitlinkt). Für `test_hls.c` und `test_parse.c` ist kein
Build-Befehl im Repository hinterlegt.

## Lizenz & Third-Party-Notices

ViTube steht unter **GPL-3.0-only** ([`LICENSE`](LICENSE)). Eine Tabelle
aller mitgelieferten bzw. gelinkten Fremdkomponenten mit Lizenz und
Quellenangabe steht in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) —
unter anderem der gepatchte FFmpeg-Build (LGPL-Basis, durch den
`h264_vita`-Hardware-Decode-Patch von wiliwili zu GPL-3.0), ReAvPlayer (MIT),
libcurl, Mbed TLS, jansson, sowie die mitgelieferten Schriftarten.

`src/media/vita_decoder.c` geht in seiner Abstammung auf das
[VitaWave](https://github.com/spyro-98/VitaWave)-Projekt zurück. Der
VitaWave-Source selbst ist nicht Teil dieses Repositories (er wird von
ViTube nicht mitgebaut) — der vollständige Source steht bereits öffentlich
im verlinkten Upstream-Repository.
