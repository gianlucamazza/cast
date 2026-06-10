#!/usr/bin/env bash
# install-host.sh — register the Cast native messaging host for LibreWolf/Firefox.
#
# Installs a small wrapper (~/.local/bin/castbridge-nm-host) that execs the built
# `castbridge --nm-host` relay, and the native-messaging manifest pointing at it.
# Idempotent, no sudo.
set -euo pipefail

HOST_NAME="it.gianlucamazza.castbridge"
REPO_DIR="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
TEMPLATE="$REPO_DIR/install/$HOST_NAME.json.in"

OPENSCREEN_DIR="${OPENSCREEN_DIR:-$HOME/Workspace/tooling/openscreen-build/openscreen}"
CASTBRIDGE_BIN="${CASTBRIDGE_BIN:-$OPENSCREEN_DIR/out/Default/castbridge}"

[[ -x "$CASTBRIDGE_BIN" ]] || {
	echo "castbridge binary not found/executable: $CASTBRIDGE_BIN" >&2
	echo "build it first: bash native/integration/build.sh" >&2
	exit 1
}

# 1) Wrapper exec'd by the browser (native manifest path must be an executable).
# Generated from the shared template so this and the PKGBUILD never drift.
mkdir -p "$HOME/.local/bin"
WRAPPER="$HOME/.local/bin/castbridge-nm-host"
wrapper_src="$(cat "$REPO_DIR/install/castbridge-nm-host.sh.in")"
printf '%s\n' "${wrapper_src//@BIN@/$CASTBRIDGE_BIN}" >"$WRAPPER"
chmod +x "$WRAPPER"
echo "wrapper: $WRAPPER -> $CASTBRIDGE_BIN --nm-host"

# 2) Native-messaging manifest, for LibreWolf and Firefox.
# Use bash substitution (not sed) so special characters in the path can never
# break the delimiter or be interpreted.
template="$(cat "$TEMPLATE")"
manifest="${template//@PATH@/$WRAPPER}"
for dir in "$HOME/.librewolf/native-messaging-hosts" "$HOME/.mozilla/native-messaging-hosts"; do
	mkdir -p "$dir"
	printf '%s\n' "$manifest" >"$dir/$HOST_NAME.json"
	echo "installed: $dir/$HOST_NAME.json"
done

echo
echo "Load the extension in LibreWolf: about:debugging#/runtime/this-firefox"
echo "  -> Load Temporary Add-on -> $REPO_DIR/extension/manifest.json"
echo "The daemon is auto-started by the relay; an optional systemd unit is in install/."
