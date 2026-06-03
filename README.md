# Cast — LibreWolf/Firefox extension + native openscreen backend

Cast a media URL or mirror the browser window/screen to a Chromecast, entirely
from the browser. The backend is a native C++ daemon (`castbridge`) built on the
**openscreen** Cast fork — it does mDNS discovery, drives the default media
receiver (`CC1AD845`) for URL casting, and runs the H.264 VAAPI Cast Streaming
sender for low-latency mirroring. No third-party tools, no `skill-cast` helpers.

```
LibreWolf ──native messaging──► castbridge --nm-host ──unix socket──► castbridged (daemon)
                                                                       ├─ DeviceLister  (mDNS)
                                                                       ├─ MediaReceiverClient (CC1AD845 LOAD)
                                                                       └─ MirrorController (cast_sender)
```

## Components

- `extension/` — the WebExtension (MV3): popup control surface, background event
  page (native port + push events + badge + context menus), content script media
  detection.
- `native/castbridge/` — the C++ daemon + native-messaging relay (one binary,
  `--daemon` / `--nm-host`). Built inside an openscreen fork checkout.
- `native/integration/build.sh` — syncs the sources into the fork and builds.
- `install/` — native-messaging host registration + optional systemd unit.

## Build

The native daemon builds inside an openscreen fork (openscreen at a pinned
commit + the Wayland/H.264 mirror patch). Both inputs are versioned in
`native/integration/` (`openscreen.pin` + `patches/`), so the fork is
provisioned reproducibly:

```bash
bash native/integration/setup-openscreen.sh   # clone+patch+configure the fork (once)
bash native/integration/build.sh               # -> <fork>/out/Default/castbridge
```

`$OPENSCREEN_DIR` overrides the checkout location (default
`~/Workspace/skill-cast/openscreen`). See `native/integration/README.md`.

## Install

```bash
bash install/install-host.sh            # wrapper + native-messaging manifest
# then: about:debugging#/runtime/this-firefox -> Load Temporary Add-on
#       -> extension/manifest.json
```

The daemon is auto-started by the relay on first use (inheriting the browser's
Wayland environment, needed for window mirroring). For a warm discovery cache,
optionally enable `install/castbridge.service`.

## Use

- **Cast a video**: open a page with a direct media URL (mp4/webm/…) or a
  supported site → popup → *Cast this video*. Transport controls (play/pause/
  seek/volume) appear while playing.
- **Mirror**: popup → *Mirror this window* / *Mirror full screen*.
- **Pick a TV**: the device chip lists discovered Chromecasts; the choice is
  remembered.

DRM sites (Netflix/Disney+/…) and MSE/`blob:` streams cannot be URL-cast; mirror
the window instead. YouTube and other site players are best mirrored unless a
direct stream URL is exposed.

## Dev

```bash
npx web-ext lint -c web-ext-config.cjs
npx web-ext run  -c web-ext-config.cjs   # launches LibreWolf with the extension
```
