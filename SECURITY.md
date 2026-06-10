# Security Policy

## Reporting a vulnerability

Please report vulnerabilities privately via
[GitHub private vulnerability reporting](https://github.com/gianlucamazza/cast/security/advisories/new)
— do **not** open a public issue. You should get a first response within a week.

Only the latest released version is supported with fixes.

## Security model

What the project assumes and where the trust boundaries are:

- **Process chain**: web page → content script → background page → native
  messaging relay (`castbridge --nm-host`) → daemon (`castbridge --daemon`)
  → Chromecast. Each hop narrows trust.
- **Web content is untrusted.** The content script (`content/detect.js`) is
  read-only detection; it intentionally runs on all http(s) pages because
  casting media from arbitrary sites is the core feature. Its reports are
  shape-validated in the background page, and the daemon independently
  validates URLs, YouTube video ids, and device addresses at the IPC boundary
  (`native/castbridge/daemon.cc`).
- **The IPC socket is same-user only.** The daemon listens on an AF_UNIX
  socket inside a `0700` runtime directory; isolation relies on standard
  Linux file permissions. There is no privilege escalation anywhere — every
  component runs as the invoking user.
- **The local network is semi-trusted.** Chromecast devices on the LAN are
  assumed not hostile; mDNS data is parsed defensively but device identity is
  not authenticated beyond the Cast protocol itself.
- **Subprocesses are spawned without a shell** (`execv`/`execvp` with argument
  vectors): `cast_sender` for mirroring, `curl` for the YouTube Lounge API,
  `hyprctl` with a fixed command line for window resolution.
- **Supply chain**: the openscreen fork is pinned by commit + verified patch
  tree hash (`native/integration/openscreen.pin`, checked by
  `regen-patch.sh --verify-only`); GitHub Actions are pinned by commit SHA;
  npm dependencies are dev-only (`web-ext`) and locked.
