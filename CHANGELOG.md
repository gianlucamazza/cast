# Changelog

Notable changes to the Cast extension + castbridge native backend.
Format: [Keep a Changelog](https://keepachangelog.com/); versions follow semver.

## [Unreleased]

### Fixed

- Hardware-found (Philips TPM191E): media-load no longer hangs forever on
  receivers that never ack the app-transport CONNECT — the CONNECT is now
  fire-and-forget like every production sender. A load retry after such a hang
  could crash the daemon (use-after-free: tearing down the old client fired
  its on-closed chain, which destroyed the successor client); explicit
  teardown is now silent and deferred resets only target the dying client.
  A timed-out load also drops its half-open session so retries start clean.

## [0.3.1] - 2026-06-10

First listed AMO submission. No functional changes over 0.3.0 (AMO version
numbers are unique per add-on, and 0.3.0 was consumed by the unlisted
channel); release workflow builds before signing and fails fast on missing
AMO secrets.

## [0.3.0] - 2026-06-10

### Added

- `SECURITY.md` (security model + private vulnerability reporting),
  `CONTRIBUTING.md`, and a pull-request template.
- Repo-wide format configs: `.prettierrc`, `.editorconfig`, and a Chromium
  `.clang-format` for `native/castbridge/`; CI enforces both formatters.
- Dependabot for GitHub Actions and npm; all workflow actions pinned by
  commit SHA.
- Input validation at the trust boundaries: the daemon rejects non-http(s)
  `poster` URLs and malformed explicit device IPs; the background page drops
  castability reports with malformed shapes.

### Fixed

- Background page: per-call reply timers are cleared on response; session state
  survives MV3 event-page suspension via `storage.session`; media-status events
  can no longer leave dead state branches.
- Popup: the device-takeover confirmation now expires after 10 seconds.
- Daemon: async replies are addressed by a stable connection id, so a reply can
  never be misdelivered to a client that reused the same file descriptor; the
  `stop` action's mirror teardown thread is tracked and joined (no detached
  thread holding a stack reference); relay refuses oversized socket paths
  instead of silently truncating; Lounge session ids are URL-encoded.

### Changed

- `daemon.cc` request handling split into per-action handlers.
- CI: new `native-sanity` job (script syntax, `openscreen.pin` format) and an
  extension/native-host id sync check; `regen-patch.sh --verify-only` reports
  patch/pin drift against the fork branch.
- The native-messaging wrapper is generated from a single shared template
  (`install/castbridge-nm-host.sh.in`) by both `install-host.sh` and the
  PKGBUILD.

## [0.2.0] - 2026-06-10

Baseline for this changelog. URL casting (default receiver, Movie/TvShow
now-playing metadata), YouTube casting via MDX/Lounge, window/screen mirroring
(VAAPI H.264 Cast Streaming on Hyprland/Wayland), mDNS discovery, MV3
extension with popup, badge, context menus, and in-page YouTube cast button.

## [0.1.0]

Initial release: URL casting and screen mirroring prototype.
