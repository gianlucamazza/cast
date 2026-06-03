#!/usr/bin/env bash
# uninstall-host.sh — remove the Cast native messaging host + wrapper.
set -euo pipefail

HOST_NAME="it.gianlucamazza.castbridge"
for dir in "$HOME/.librewolf/native-messaging-hosts" "$HOME/.mozilla/native-messaging-hosts"; do
	f="$dir/$HOST_NAME.json"
	[[ -f "$f" ]] && {
		rm -f "$f"
		echo "removed: $f"
	}
done
rm -f "$HOME/.local/bin/castbridge-nm-host" && echo "removed wrapper"
