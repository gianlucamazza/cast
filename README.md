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
  `--daemon` / `--nm-host`). Built inside an openscreen fork checkout. See
  [`native/castbridge/README.md`](native/castbridge/README.md) for the
  subsystems and the IPC protocol.
- `native/integration/` — provisions the openscreen fork and builds castbridge
  (`setup-openscreen.sh`, `build.sh`); see its
  [README](native/integration/README.md).
- `install/` — native-messaging host registration + optional systemd unit.
- `packaging/aur/` — `PKGBUILD` for the native host.

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
`~/Workspace/tooling/openscreen-build/openscreen`). See `native/integration/README.md`.

## Install (users)

Two pieces: the extension (browser) and the native host (system).

- **Native host** — Arch/AUR: `yay -S castbridge` builds the daemon + the
  `cast_sender` mirror helper and installs the native-messaging manifest
  system-wide. (See `packaging/aur/PKGBUILD`.)
- **Extension** — from [AMO](https://addons.mozilla.org) once published, or
  install the signed `.xpi` attached to a GitHub release.

The daemon is auto-started by the host on first use, inheriting the browser's
Wayland environment (needed for window mirroring).

## Install (from source)

```bash
bash install/install-host.sh            # wrapper + native-messaging manifest
# then: about:debugging#/runtime/this-firefox -> Load Temporary Add-on
#       -> extension/manifest.json
```

For a warm discovery cache, optionally enable `install/castbridge.service`.

## Install permanently (LibreWolf)

`about:debugging` only loads the extension *temporarily* — it is dropped when the
browser restarts. For a permanent install you need an `.xpi`, and Firefox/LibreWolf
require add-ons to be signed (or signature enforcement disabled). Two options:

**A — unsigned `.xpi`, LibreWolf only.** LibreWolf lets you run unsigned add-ons
(stock Firefox release does not); the manifest already declares an explicit id, so
this works:

```bash
npm run build                                              # -> .web-ext-artifacts/cast-<ver>.zip
cp .web-ext-artifacts/cast-*.zip .web-ext-artifacts/cast.xpi
```

1. `about:config` → set `xpinstall.signatures.required` to `false`
2. `about:addons` → gear ⚙ → **Install Add-on From File…** → pick the `.xpi`

**B — signed `.xpi`, any Firefox/LibreWolf.** Sign through Mozilla (AMO) to get a
self-distributable `.xpi` that installs without touching `about:config`:

```bash
AMO_JWT_ISSUER=… AMO_JWT_SECRET=… npm run sign:unlisted    # -> signed .xpi
```

This is also what `release.yml` does on a `v*` tag, attaching the signed `.xpi` to
the GitHub release.

Either way you still need the **native host** (`bash install/install-host.sh`),
otherwise the popup reports `nohost`.

## Compatibility

| Capability        | Support                                                        |
| ----------------- | -------------------------------------------------------------- |
| URL cast (direct) | mp4/webm/ogg/mp3/…, plus HLS `.m3u8` / DASH `.mpd` links       |
| YouTube           | native (TV's YouTube app via MDX + Lounge)                     |
| Screen mirror     | any Wayland compositor (Hyprland today; portal/PipeWire planned) |
| Window mirror     | **Hyprland only** (uses `hyprctl`); elsewhere use screen mirror |
| DRM / MSE / blob: | not URL-castable (Netflix/Disney+/…) — mirror instead          |

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
npx web-ext lint -c web-ext-config.mjs
npx web-ext run  -c web-ext-config.mjs   # launches LibreWolf with the extension
```

CI (`.github/workflows/ci.yml`) lints/builds the extension and shellchecks the
scripts on every push. Tagging `v*` runs `release.yml`, which signs the
extension with `web-ext sign` (unlisted by default; `listed` via
`workflow_dispatch` uses `amo-metadata.json`) and attaches the `.xpi` to the
release. Set the `AMO_JWT_ISSUER` / `AMO_JWT_SECRET` repo secrets first; locally
`npm run sign:unlisted` does the same.

## License

BSD 3-Clause ([`LICENSE`](LICENSE)). The native backend links Google's
openscreen (also BSD 3-Clause); that upstream license governs those sources.
