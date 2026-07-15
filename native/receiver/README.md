# castbridge custom Cast receiver (ADR 0013 — nstream)

A minimal custom **CAF v3 Web Receiver** (`index.html`) that lets a Dolby-capable display play
AC-3/E-AC-3 audio directly — no host-side Tier-2 remux — and exposes native audio/text track
selection. It replaces the Default Media Receiver (`CC1AD845`) only when an operator registers
and hosts it (steps below); until then nstream keeps using the DMR + remux path.

**Status: hosted live, awaiting app-id registration — NOT yet cast-validated.** The receiver is
deployed and reachable at **https://gianlucamazza.it/cast-receiver/index.html** (HTTP 200, its
own CSP allowing the gstatic CAF SDK). It has never run on a real device because no Cast
application id points at it yet. Validate on a Dolby-capable _and_ a non-Dolby display once
registered (kb: validate-in-the-field).

Hosting deployed via the `personal-website` repo: `public/cast-receiver/index.html` (a
byte-identical copy of this file, excluded from Prettier + the content-SEO scan) + a dedicated
nginx `location ^~ /cast-receiver/`. Register at the **apex** (www 301s to it).

## Why the enabling plumbing already ships, but this doesn't run

Codec support on Cast is **hardware-dependent, not receiver-fixed**: a custom receiver enables
AC-3/E-AC-3 passthrough _only where the display supports it_, which the receiver probes at
runtime with `canDisplayType()` (see `index.html`). The Default Media Receiver simply doesn't
enable it. The pieces that don't need Google's paid registration are already in the tree and
dormant:

- **castbridge**: `media-load` accepts an optional `appId` (validated, `IsOptionalAppId`), passed
  to `MediaReceiverClient::app_id()` — empty → `kDefaultMediaReceiverAppId` (`CC1AD845`,
  unchanged). `native/castbridge/media_receiver_client.{h,cc}`, `daemon.cc`.
- **nstream**: `bridge._media_load_args(app_id=...)` forwards it; no nstream path sets it yet.
- **capability channel**: this receiver broadcasts a `{type:"capabilities", caps}` message on
  `urn:x-cast:it.gianlucamazza.castbridge` on READY, ready for castbridge to consume (ADR 0016).

What remains is exactly what an operator (not the code) must provide.

## Operator steps to go live

1. **Register an application id** in the [Google Cast Developer Console](https://cast.google.com/publish)
   (Google account + one-time $5 developer registration). Create a **Custom Receiver**, set its
   URL to **https://gianlucamazza.it/cast-receiver/index.html** (already hosted, step 2 ✅).
   Register your test device's serial (the Philips at 192.168.1.228) for unpublished testing.
   _Blocker: the $5 fee currently fails at CheBanca/Nexi 3DS — unblock the card or use another._
2. ~~**Host `index.html` over HTTPS**~~ **DONE** — deployed via the `personal-website` repo (PR
   merged → GHCR → odroid), live at `https://gianlucamazza.it/cast-receiver/index.html` (HTTP 200,
   CSP allows the gstatic CAF SDK). Register at the apex; www 301s to it.
3. **Point castbridge at it** — send the registered app id as the `appId` media-load arg. From
   nstream, thread it through `bridge.cast_load(app_id=...)` (a future `cast_receiver_app_id`
   config knob would wire this; deliberately not added while unvalidated).
4. **Consume the capability report** in castbridge (parse the `capabilities` custom message,
   surface it like `MediaStatus.error`/`activeTrackIds`) and gate nstream's Tier-2 remux on it:
   remux only when the receiver reports it _cannot_ play the release's real audio. Keep the remux
   path (kb: run-disabled-not-removed) — non-passthrough displays still need it.
5. **Field-validate** on a Dolby-capable and a non-Dolby display, then (only then) consider
   flipping the default app id. Add a fallback to `CC1AD845` when the custom id fails to launch.

## Local check (before hosting)

The SDK only runs on a real Cast device, so there is no meaningful local unit test. A syntax/DOM
sanity check: open `index.html` in a browser — it will log a CAF "not running on a receiver"
error, which confirms the SDK loaded and the script parsed. Real behaviour is only observable on
a registered, hosted receiver cast to a device.
