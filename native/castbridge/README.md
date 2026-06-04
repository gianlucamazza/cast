# castbridge — native backend

One C++ binary, two roles (`castbridge_main.cc`):

- `castbridge --nm-host` — the native-messaging relay Firefox launches.
- `castbridge --daemon` — the long-lived discovery/session daemon.

The relay auto-starts the daemon on first use and bridges browser ⇄ daemon, so
the browser only ever talks to the relay.

```
LibreWolf ──native messaging──► nm_relay ──unix socket (newline JSON)──► daemon
                                                                          ├─ DeviceLister          (mDNS)
                                                                          ├─ MediaController       → MediaReceiverClient (CC1AD845 LOAD)
                                                                          ├─ MirrorController      → spawns cast_sender (H.264 VAAPI)
                                                                          └─ YouTubeController     → YouTubeCastClient (MDX) + YouTubeLounge (HTTP)
```

## Subsystems

| File | Role |
|------|------|
| `nm_relay.*` | native-messaging framing (uint32 len + JSON) ⇄ unix socket lines; auto-spawns the daemon under an `flock` so concurrent relays don't race. |
| `ipc_server.*` | AF_UNIX server, newline-delimited JSON, multi-client, `poll()` + `eventfd` wakeups. |
| `daemon.*` | owns the subsystems; dispatches request actions and broadcasts push events; signal-driven lifecycle. |
| `device_lister.*` | openscreen mDNS discovery; thread-safe snapshot, offline diffing, LAN-interface filtering. |
| `media_receiver_client.*` | drives the default media receiver `CC1AD845`: connect → LAUNCH → LOAD, status updates, play/pause/seek/volume/mute, reconnect/teardown. |
| `media_controller.*` | binds IPC requests to the receiver client on the TaskRunner thread (load timeout, one-shot guards, status broadcast). |
| `mirror_controller.*` | launches the `cast_sender` subprocess for window/screen mirroring (H.264 VAAPI, bitrate, optional audio PID); monitors/reaps it. |
| `window_resolver.*` | resolves a window to address+PID via `hyprctl` (Hyprland); reports `wm_available=false` elsewhere so the daemon returns an actionable error. |
| `youtube_cast_client.*` | Cast channel to the TV's YouTube app (LAUNCH `233637DE`, MDX handshake → screenId). |
| `youtube_lounge.*` | YouTube Lounge HTTP API (token → bind → setPlaylist + play/pause/seek) via a managed `curl` subprocess; re-auths on an expired session. Also long-polls the event channel (`Poll`/`Refresh`) to read the TV's real playback state. |
| `youtube_controller.*` | orchestrates the two YouTube pieces; Lounge commands run on one worker thread (monotonic session state), while a dedicated poll thread reads the event channel and pushes real play/pause status. |

## IPC protocol

Newline-delimited JSON over the unix socket. Requests carry `{id, action, args}`
and get a `{id, action, ok, data|error}` reply; the daemon also emits unsolicited
events (no `id`).

**Actions:** `devices`, `media-load`, `media-control` (`{cmd: play|pause|seek|volume|mute, value}`),
`mirror-window`, `mirror-screen`, `youtube-load`, `status`, `stop`.

**Events:** `session` (authoritative state on start/stop/natural end — now carries
the real YouTube play/pause state from the Lounge event channel, not an optimistic
default), `media-status` (live position for the URL receiver), `devices-changed`,
`session-ended`.

Error codes the extension reacts to: `ambiguous`, `no_devices`, `no_window`,
`no_wm`, plus relay-level `nohost`/`timeout`.

## Build

Not built standalone — see [`../integration/`](../integration/) (it links the
openscreen Cast libraries inside the fork checkout). Editor diagnostics: run
`../integration/gen-clangd.sh`.
