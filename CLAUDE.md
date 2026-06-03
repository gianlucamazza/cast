# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A LibreWolf/Firefox MV3 extension (`extension/`) plus a native C++ daemon
(`native/castbridge/`) that casts media URLs and mirrors the browser
window/screen to a Chromecast. The native backend is built on a fork of Google's
**openscreen** Cast stack — no third-party helper tools.

Data flow (browser never talks to the daemon directly — only via the relay):

```
LibreWolf ──native messaging──► castbridge --nm-host ──unix socket (newline JSON)──► castbridge --daemon
```

One C++ binary, two roles selected by flag (`castbridge_main.cc`): `--nm-host`
(the relay Firefox launches) and `--daemon` (long-lived discovery/session).

## Commands

Extension (Node/web-ext):
```bash
npm run lint            # web-ext lint -c web-ext-config.cjs
npm run start           # launches LibreWolf with the extension loaded
npm run build           # -> .web-ext-artifacts/*.zip
npm run sign:unlisted   # needs AMO_JWT_ISSUER / AMO_JWT_SECRET
```

Native daemon (builds **inside** an openscreen fork checkout, not standalone):
```bash
bash native/integration/setup-openscreen.sh   # provision the fork (once; ~GB deps, slow)
bash native/integration/build.sh               # -> <fork>/out/Default/castbridge
bash native/integration/gen-clangd.sh          # fix editor/clangd include paths
```
`$OPENSCREEN_DIR` (default `~/Workspace/tooling/openscreen-build/openscreen`) overrides the checkout.

Install from source:
```bash
bash install/install-host.sh   # native-messaging manifest + wrapper; then load extension/manifest.json via about:debugging
```

## Architecture notes

- **There are no unit tests.** CI (`.github/workflows/ci.yml`) only runs
  `web-ext lint`/`build` and `shellcheck` on `install/*.sh` + `native/integration/*.sh`.
  Verify native changes by building; verify extension changes via lint + `web-ext run`.

- **Extension side**: `background/background.js` is the sole owner of the native
  port, the per-tab castability map (fed by `content/detect.js`), the toolbar
  badge, and context menus. Popup and content scripts talk *only* to the
  background page; only the background page talks to the native host. It speaks
  the daemon action protocol and consumes push events.

- **Native subsystems** (owned by `daemon.*`): `device_lister` (mDNS),
  `media_receiver_client` + `media_controller` (URL cast via default receiver
  `CC1AD845`), `mirror_controller` (spawns the `cast_sender` H.264 VAAPI
  subprocess), `window_resolver` (Hyprland-only window→PID via `hyprctl`),
  `youtube_controller` + `youtube_cast_client` (MDX) + `youtube_lounge` (HTTP).
  `nm_relay` does native-messaging framing and auto-spawns the daemon under an
  `flock`; `ipc_server` is the AF_UNIX newline-JSON server.

- **IPC protocol**: newline-delimited JSON. Requests `{id, action, args}` →
  replies `{id, action, ok, data|error}`; daemon also emits id-less push events.
  Actions: `devices`, `media-load`, `media-control`, `mirror-window`,
  `mirror-screen`, `youtube-load`, `status`, `stop`. Events: `session`,
  `media-status`, `devices-changed`, `session-ended`. Error codes the extension
  reacts to: `ambiguous`, `no_devices`, `no_window`, `no_wm`, plus relay-level
  `nohost`/`timeout`.

- **Native host id** is `it.gianlucamazza.castbridge` (used in both
  `background.js` and the install manifest — keep them in sync).

- **Build coupling**: `build.sh` copies `native/castbridge/*` into the fork's
  `cast/castbridge/`, registers the gn target, and grants dep visibility — all
  idempotent. Editor "file not found" / "`std::string` aka `int`" diagnostics are
  spurious (fork-relative includes); `ninja` builds cleanly. Run `gen-clangd.sh`.

## Detailed docs

Read these before non-trivial work on each area — they are kept current:
- `native/castbridge/README.md` — subsystem table + full IPC protocol
- `native/integration/README.md` — fork provisioning, pinned inputs, gn wiring
- `README.md` — user-facing install, compatibility matrix, capabilities

## Conventions

Comments and docs in English; concise, no over-engineering. The openscreen fork
is pinned reproducibly in `native/integration/openscreen.pin` + `patches/` — to
bump it, rebase the fork branch, re-cut the patch (`git format-patch`), update
the pin. License is BSD 3-Clause.
