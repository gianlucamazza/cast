#!/usr/bin/env bash
# build.sh — sync the castbridge sources into an openscreen fork checkout and
# build the `castbridge` target.
#
# The fork checkout is the native Cast Streaming sender (openscreen at the
# pinned commit + the Wayland/H264 mirror patch). It is an external build
# dependency; point $OPENSCREEN_DIR at it. Our sources live in this repo and are
# copied in on each build, so the repo stays the source of truth.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
SRC_DIR="$REPO_DIR/native/castbridge"

# Default to the fork checkout sibling of this repo (../openscreen-build/openscreen);
# override with $OPENSCREEN_DIR for any other layout (CI, packaging).
OPENSCREEN_DIR="${OPENSCREEN_DIR:-$(dirname "$REPO_DIR")/openscreen-build/openscreen}"
OUT_DIR="${OUT_DIR:-out/Default}"

[[ -d "$OPENSCREEN_DIR" ]] || {
	echo "openscreen checkout not found: $OPENSCREEN_DIR (set \$OPENSCREEN_DIR)" >&2
	echo "provision it first: bash native/integration/setup-openscreen.sh" >&2
	exit 1
}
[[ -f "$OPENSCREEN_DIR/$OUT_DIR/args.gn" ]] || {
	echo "no gn out dir at $OPENSCREEN_DIR/$OUT_DIR (configure the build first)" >&2
	echo "provision it first: bash native/integration/setup-openscreen.sh" >&2
	exit 1
}
# The Wayland/H.264 mirror patch must be applied — its files are what castbridge
# links against. Probe one of them rather than trusting the checkout blindly.
[[ -f "$OPENSCREEN_DIR/cast/standalone_sender/screen_mirror_sender.h" ]] || {
	echo "openscreen checkout is missing the Wayland/H.264 mirror patch" >&2
	echo "(re)provision it: bash native/integration/setup-openscreen.sh" >&2
	exit 1
}

# ninja drives the build; it auto-regenerates the gn graph via the buildtools
# gn recorded in build.ninja, so a separate gn on PATH is not required.
NINJA="${NINJA:-$(command -v ninja || echo "$OPENSCREEN_DIR/../depot_tools/ninja")}"
[[ -x "$NINJA" ]] || {
	echo "ninja not found (looked for: $NINJA)" >&2
	echo "install ninja or set \$NINJA to the binary" >&2
	exit 1
}

# Apply a one-line gn edit, then verify it took. The anchors are upstream gn
# lines; if openscreen reformats them the sed is a silent no-op, so we re-check
# for the inserted marker and fail loudly instead of building a broken graph.
# Args: <file> <already-present-marker> <sed-expr> <description>
wire() {
	local file="$1" marker="$2" expr="$3" desc="$4"
	if grep -qF "$marker" "$file"; then
		return 0
	fi
	sed -i "$expr" "$file"
	if ! grep -qF "$marker" "$file"; then
		echo "failed to wire $desc in $file" >&2
		echo "(the upstream gn anchor changed — update build.sh)" >&2
		exit 1
	fi
	echo "wired $desc"
}

# 1) Link the source tree into the fork checkout. A symlink (not a copy) keeps a
# single source of truth: editing the sources through the fork checkout edits
# this repo directly, so there is no copy to drift or lose edits to.
dest="$OPENSCREEN_DIR/cast/castbridge"
if [[ -L "$dest" ]]; then
	: # already a symlink; leave it (refreshed below anyway)
elif [[ -e "$dest" ]]; then
	rm -rf "$dest" # drop a stale real copy from the old cp-based flow
fi
ln -sfn "$SRC_DIR" "$dest" # absolute target; the link is local-only, never committed
echo "linked sources -> $dest -> $SRC_DIR"

# 1b) Keep the fork's `git status` clean: the linked sources and the generated
# compile DB are build artifacts, not fork content. Ignore them locally via
# .git/info/exclude (never the tracked .gitignore — that would itself show as a
# modification). Idempotent. Note: the pattern has NO trailing slash — a trailing
# slash only matches a directory, but cast/castbridge is now a symlink, which git
# would otherwise report as untracked. Drop any old slashed entry.
exclude="$OPENSCREEN_DIR/.git/info/exclude"
if [[ -f "$exclude" ]]; then
	sed -i '\#^cast/castbridge/$#d' "$exclude" # remove stale slashed pattern
	for pat in 'cast/castbridge' '/out/Default/compile_commands.json'; do
		grep -qxF "$pat" "$exclude" || echo "$pat" >>"$exclude"
	done
fi

# 2) Wire the target into gn_all.
wire "$OPENSCREEN_DIR/BUILD.gn" 'cast/castbridge:castbridge' \
	's#\("cast/standalone_sender:cast_sender",\)#\1\n        "cast/castbridge:castbridge",#' \
	'cast/castbridge:castbridge into gn_all'

# 3) Grant our target visibility on the few deps that whitelist only cast_sender.
wire "$OPENSCREEN_DIR/discovery/BUILD.gn" '"../cast/castbridge:*"' \
	's#\("../cast/standalone_sender:\*",\)#\1\n    "../cast/castbridge:*",#' \
	'dnssd visibility'
wire "$OPENSCREEN_DIR/platform/BUILD.gn" '"../cast/castbridge:*"' \
	's#\("../cast/standalone_sender:cast_sender",\)#\1\n      "../cast/castbridge:*",#' \
	'standalone_impl visibility'
# certificate_boringssl (Phase C, TLS) whitelists only cast_sender.
wire "$OPENSCREEN_DIR/cast/common/BUILD.gn" '"../castbridge:*"' \
	's#\("../standalone_sender:cast_sender",\)#\1\n    "../castbridge:*",#' \
	'certificate_boringssl visibility'

# 4) Regenerate and build.
(cd "$OPENSCREEN_DIR" && "$NINJA" -C "$OUT_DIR" cast/castbridge:castbridge)

# 5) Verify the binary actually came out.
bin="$OPENSCREEN_DIR/$OUT_DIR/castbridge"
[[ -x "$bin" ]] || {
	echo "build reported success but $bin is missing/not executable" >&2
	exit 1
}
echo
echo "built: $bin"
"$bin" 2>/dev/null || true # prints usage (exit 2)
