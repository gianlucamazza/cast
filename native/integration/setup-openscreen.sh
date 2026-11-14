#!/usr/bin/env bash
# setup-openscreen.sh — provision the openscreen checkout that castbridge builds
# against, reproducibly.
#
# Two strategies (hybrid), both end at the same patched tree:
#   default        clone the private fork at its patched branch (fast, no git am;
#                  needs SSH access to the fork — falls back to the upstream
#                  strategy automatically when the fork is not reachable)
#   --from-upstream  clone upstream openscreen at the pin and apply the versioned
#                  patch (no fork access needed — CI and everyone else)
#
# Idempotent. After this completes, run build.sh to add and build castbridge.
#
#   bash setup-openscreen.sh                 # fork strategy, default location
#   OPENSCREEN_DIR=~/src/openscreen bash setup-openscreen.sh --from-upstream
#
# Layout it creates (siblings under the workspace dir; by default the workspace
# is ../openscreen-build, a sibling of this repo):
#   <workspace>/depot_tools        depot_tools (gn/ninja/gclient)
#   <workspace>/.gclient           gclient solution for openscreen
#   <workspace>/openscreen         the checkout (== $OPENSCREEN_DIR)
set -euo pipefail

MODE=fork
[[ "${1:-}" == "--from-upstream" || "${FROM_UPSTREAM:-}" == "1" ]] && MODE=upstream

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
FORK="$(read_pin fork)"
FORK_BRANCH="$(read_pin fork_branch)"
FORK_COMMIT="$(read_pin fork_commit)"
PATCH="$HERE/$PATCH_REL"
[[ -n "$PIN" && -f "$PATCH" ]] || {
	echo "pin/patch not resolved from $PIN_FILE" >&2
	exit 1
}

# Default to the fork checkout sibling of this repo (../openscreen-build/openscreen);
# override with $OPENSCREEN_DIR for any other layout (CI, packaging, a workspace
# where repos and build trees live apart).
REPO_DIR="$(cd "$HERE/../.." && pwd)"
OPENSCREEN_DIR="${OPENSCREEN_DIR:-$(dirname "$REPO_DIR")/openscreen-build/openscreen}"
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
# With updates off, gclient never runs update_depot_tools — the only caller of
# bootstrap_python3 — and depot_tools' python wrappers then refuse to run.
# ensure_bootstrap does the bootstrap without a git update.
if [[ ! -f "$DEPOT_TOOLS/python3_bin_reldir.txt" ]]; then
	echo "==> bootstrapping depot_tools"
	"$DEPOT_TOOLS/ensure_bootstrap"
fi

# `git am` creates a commit: give it a local identity and never sign, so the
# build works without a git identity and never prompts a signing agent. The
# result is checked by TREE (patched=), so the commit metadata is irrelevant.
git_am() {
	git -C "$OPENSCREEN_DIR" -c user.name="castbridge build" \
		-c user.email=build@castbridge.invalid -c commit.gpgsign=false \
		am --no-gpg-sign "$@"
}

# 2) gclient solution (unmanaged: we control the openscreen checkout ourselves;
#    the DEPS are identical for fork and upstream, so the solution url stays
#    upstream and only the third_party deps get synced).
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

# 3) get the checkout to the patched tree.
# The fork is private: the fast path only works with SSH access to it. Probe
# reachability non-interactively and fall back to upstream+patch — both
# strategies end at the same tree (verified by the patched= invariant below).
if [[ "$MODE" == fork ]] && ! GIT_TERMINAL_PROMPT=0 GIT_SSH_COMMAND="ssh -oBatchMode=yes" \
	git ls-remote --exit-code "$FORK" "refs/heads/$FORK_BRANCH" >/dev/null 2>&1; then
	echo "==> fork $FORK not reachable (private; needs SSH access) — falling back to upstream+patch"
	MODE=upstream
fi
if [[ "$MODE" == fork ]]; then
	echo "==> strategy: clone fork ($FORK @ ${FORK_COMMIT:-$FORK_BRANCH})"
	if [[ ! -d "$OPENSCREEN_DIR/.git" ]]; then
		git clone --branch "$FORK_BRANCH" "$FORK" "$OPENSCREEN_DIR"
	fi
	git -C "$OPENSCREEN_DIR" remote get-url fork >/dev/null 2>&1 ||
		git -C "$OPENSCREEN_DIR" remote add fork "$FORK"
	git -C "$OPENSCREEN_DIR" fetch fork "$FORK_BRANCH"
	# The branch keeps moving; build the commit this release was cut from.
	if git -C "$OPENSCREEN_DIR" checkout --detach "${FORK_COMMIT:-fork/$FORK_BRANCH}" &&
		[[ -z "$PATCHED" || "$(git -C "$OPENSCREEN_DIR" rev-parse 'HEAD^{tree}')" == "$PATCHED" ]]; then
		:
	else
		echo "==> fork ${FORK_COMMIT:-$FORK_BRANCH} does not give tree $PATCHED; falling back to upstream+patch" >&2
		MODE=upstream
	fi
fi
if [[ "$MODE" == upstream ]]; then
	echo "==> strategy: upstream + patch (pin $PIN)"
	if [[ ! -d "$OPENSCREEN_DIR/.git" ]]; then
		git clone https://chromium.googlesource.com/openscreen.git "$OPENSCREEN_DIR"
	fi
	if [[ -n "$(git -C "$OPENSCREEN_DIR" status --porcelain --untracked-files=no)" ]]; then
		echo "==> openscreen tree has tracked modifications; resetting"
		git -C "$OPENSCREEN_DIR" checkout -- .
	fi
	if [[ "$(git -C "$OPENSCREEN_DIR" rev-parse 'HEAD^{tree}')" == "$PATCHED" ]]; then
		echo "==> patch already applied (tree == $PATCHED); skipping checkout/am"
	else
		# The pin lives upstream (and in the fork's history); fetch it from whichever
		# remote this checkout came from.
		git -C "$OPENSCREEN_DIR" cat-file -e "${PIN}^{commit}" 2>/dev/null ||
			git -C "$OPENSCREEN_DIR" fetch https://chromium.googlesource.com/openscreen.git "$PIN"
		git -C "$OPENSCREEN_DIR" checkout --detach "$PIN"
		echo "==> applying $PATCH_REL"
		git_am "$PATCH" || {
			git -C "$OPENSCREEN_DIR" am --abort || true
			echo "patch failed to apply on $PIN — the pin or patch is stale" >&2
			exit 1
		}
	fi
fi

# 3b) Invariant: whichever strategy ran, the resulting tree must equal the pin's
# patched= hash. Catches a fork branch advanced without re-cutting the patch, or
# a stale patch. patched= is a TREE object (deterministic) — not a commit, whose
# hash varies with the `git am` committer date. Regenerate both with
# regen-patch.sh whenever the sender changes.
if [[ -n "$PATCHED" ]]; then
	GOT_TREE="$(git -C "$OPENSCREEN_DIR" rev-parse 'HEAD^{tree}')"
	if [[ "$GOT_TREE" != "$PATCHED" ]]; then
		echo "checkout tree $GOT_TREE != pin patched=$PATCHED" >&2
		echo "the fork branch and the pin/patch are out of sync" >&2
		echo "regenerate them: bash native/integration/regen-patch.sh" >&2
		exit 1
	fi
fi

# 4) sync third_party deps for the current checkout (idempotent; the heavy step).
echo "==> gclient sync (downloads ~GB of deps on a fresh tree)"
(cd "$OPENSCREEN_DIR" && gclient sync --no-history --shallow)

# 5) configure the gn output dir. The patch wires cast/castbridge into gn_all, so
# the castbridge sources must be linked in before gn can load the graph (build.sh
# keeps re-linking them; this only covers a fresh checkout).
if [[ ! -e "$OPENSCREEN_DIR/cast/castbridge" ]]; then
	ln -sfn "$REPO_DIR/native/castbridge" "$OPENSCREEN_DIR/cast/castbridge"
fi
# build.ninja, not args.gn: a failed `gn gen` leaves args.gn behind.
if [[ ! -f "$OPENSCREEN_DIR/$OUT_DIR/build.ninja" ]]; then
	echo "==> gn gen $OUT_DIR with: $GN_ARGS"
	# The DEPS-pinned gn from the checkout, not depot_tools' python wrapper.
	GN="$OPENSCREEN_DIR/buildtools/linux64/gn"
	[[ -x "$GN" ]] || GN="gn"
	(cd "$OPENSCREEN_DIR" && "$GN" gen "$OUT_DIR" --args="$GN_ARGS")
else
	echo "==> $OUT_DIR already configured; leaving as-is"
fi

echo
echo "openscreen ready at $OPENSCREEN_DIR ($MODE)"
echo "next: bash native/integration/build.sh"
