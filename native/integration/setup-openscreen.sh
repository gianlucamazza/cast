#!/usr/bin/env bash
# setup-openscreen.sh — provision the openscreen fork checkout that castbridge
# builds against, reproducibly from the pinned commit + versioned patch.
#
# Idempotent. After this completes, run build.sh to add and build castbridge.
#
#   OPENSCREEN_DIR=~/Workspace/skill-cast/openscreen bash setup-openscreen.sh
#
# Layout it creates (siblings under the workspace dir):
#   <workspace>/depot_tools        depot_tools (gn/ninja/gclient)
#   <workspace>/.gclient           gclient solution for openscreen
#   <workspace>/openscreen         the checkout (== $OPENSCREEN_DIR)
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
PIN_FILE="$HERE/openscreen.pin"
[[ -f "$PIN_FILE" ]] || {
	echo "missing $PIN_FILE" >&2
	exit 1
}

# Read key=value pairs from the pin file (ignore comments/blank lines).
read_pin() { grep -E "^$1=" "$PIN_FILE" | head -1 | cut -d= -f2-; }
PIN="$(read_pin pin)"
PATCH_REL="$(read_pin patch)"
PATCHED="$(read_pin patched)"
GN_ARGS="$(read_pin gn_args)"
PATCH="$HERE/$PATCH_REL"
[[ -n "$PIN" && -f "$PATCH" ]] || {
	echo "pin/patch not resolved from $PIN_FILE" >&2
	exit 1
}

OPENSCREEN_DIR="${OPENSCREEN_DIR:-$HOME/Workspace/skill-cast/openscreen}"
OUT_DIR="${OUT_DIR:-out/Default}"
WORKSPACE="$(dirname "$OPENSCREEN_DIR")"
DEPOT_TOOLS="${DEPOT_TOOLS:-$WORKSPACE/depot_tools}"

# 1) depot_tools (provides gclient + the buildtools gn/ninja).
if [[ ! -d "$DEPOT_TOOLS" ]]; then
	echo "==> cloning depot_tools -> $DEPOT_TOOLS"
	git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git "$DEPOT_TOOLS"
fi
export PATH="$DEPOT_TOOLS:$PATH"
export DEPOT_TOOLS_UPDATE="${DEPOT_TOOLS_UPDATE:-0}"

# 2) gclient solution (unmanaged: we control the openscreen checkout ourselves).
mkdir -p "$WORKSPACE"
if [[ ! -f "$WORKSPACE/.gclient" ]]; then
	echo "==> writing $WORKSPACE/.gclient"
	cat >"$WORKSPACE/.gclient" <<'EOF'
solutions = [
  {
    "name": "openscreen",
    "url": "https://chromium.googlesource.com/openscreen.git",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {},
  },
]
EOF
fi

# 3) openscreen checkout at the pin.
if [[ ! -d "$OPENSCREEN_DIR/.git" ]]; then
	echo "==> cloning openscreen -> $OPENSCREEN_DIR"
	git clone https://chromium.googlesource.com/openscreen.git "$OPENSCREEN_DIR"
fi

if [[ -n "$(git -C "$OPENSCREEN_DIR" status --porcelain --untracked-files=no)" ]]; then
	# A clean tree is required to checkout/am. cast/castbridge/ is untracked and
	# harmless; tracked modifications are not (build.sh's gn edits) — reset them.
	echo "==> openscreen tree has tracked modifications; resetting to a clean state"
	git -C "$OPENSCREEN_DIR" checkout -- .
fi

HEAD_NOW="$(git -C "$OPENSCREEN_DIR" rev-parse HEAD)"
if [[ "$HEAD_NOW" == "$PATCHED" ]]; then
	echo "==> patch already applied (HEAD == $PATCHED); skipping checkout/am"
else
	echo "==> fetching + checking out pin $PIN"
	git -C "$OPENSCREEN_DIR" fetch --tags origin "$PIN" || git -C "$OPENSCREEN_DIR" fetch origin
	git -C "$OPENSCREEN_DIR" checkout --detach "$PIN"

	# 4) sync third_party deps at the pin (does not touch our checkout: managed=False).
	echo "==> gclient sync (this downloads ~GB of deps on a fresh tree)"
	(cd "$OPENSCREEN_DIR" && gclient sync --no-history --shallow)

	# 5) apply the Wayland/H.264 mirror patch.
	echo "==> applying $PATCH_REL"
	git -C "$OPENSCREEN_DIR" am "$PATCH" || {
		git -C "$OPENSCREEN_DIR" am --abort || true
		echo "patch failed to apply on $PIN — the pin or patch is stale" >&2
		exit 1
	}
	NEW_HEAD="$(git -C "$OPENSCREEN_DIR" rev-parse HEAD)"
	[[ "$NEW_HEAD" == "$PATCHED" ]] ||
		echo "warning: patched HEAD $NEW_HEAD != recorded $PATCHED (content may still be fine)" >&2
fi

# 6) configure the gn output dir.
if [[ ! -f "$OPENSCREEN_DIR/$OUT_DIR/args.gn" ]]; then
	echo "==> gn gen $OUT_DIR with: $GN_ARGS"
	(cd "$OPENSCREEN_DIR" && gn gen "$OUT_DIR" --args="$GN_ARGS")
else
	echo "==> $OUT_DIR/args.gn already present; leaving as-is"
fi

echo
echo "openscreen fork ready at $OPENSCREEN_DIR"
echo "next: bash native/integration/build.sh"
