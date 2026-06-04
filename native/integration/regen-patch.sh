#!/usr/bin/env bash
# regen-patch.sh — regenerate the versioned Wayland/H.264 mirror patch (and the
# `patched=` tree hash in openscreen.pin) from the fork branch.
#
# The fork branch (`fork_branch` in openscreen.pin) is the single source of
# truth for the sender. The patch under patches/ exists only for the
# `setup-openscreen.sh --from-upstream` path (CI / no fork access); it is a
# GENERATED artifact, not hand-maintained. Run this after every change to the
# sender on the fork branch, then commit the updated patch + pin.
#
# It is self-verifying: it applies the freshly cut patch onto the pinned upstream
# commit in a throwaway worktree and asserts the resulting tree is byte-identical
# to the fork branch tree. If they differ it fails without touching anything.
#
#   bash native/integration/regen-patch.sh
#
# Honors $OPENSCREEN_DIR (the working checkout it reads history from); defaults
# to the sibling fork checkout, like setup-openscreen.sh / build.sh.
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
PIN_FILE="$HERE/openscreen.pin"
[[ -f "$PIN_FILE" ]] || {
	echo "missing $PIN_FILE" >&2
	exit 1
}

read_pin() { grep -E "^$1=" "$PIN_FILE" | head -1 | cut -d= -f2-; }
PIN="$(read_pin pin)"
FORK_BRANCH="$(read_pin fork_branch)"
PATCH_REL="$(read_pin patch)"
PATCH="$HERE/$PATCH_REL"
SUBJECT="standalone_sender: Wayland desktop/window Cast Streaming sender"

[[ -n "$PIN" && -n "$FORK_BRANCH" && -n "$PATCH_REL" ]] || {
	echo "pin/fork_branch/patch not resolved from $PIN_FILE" >&2
	exit 1
}

REPO_DIR="$(cd "$HERE/../.." && pwd)"
OPENSCREEN_DIR="${OPENSCREEN_DIR:-$(dirname "$REPO_DIR")/openscreen-build/openscreen}"
git -C "$OPENSCREEN_DIR" rev-parse --git-dir >/dev/null 2>&1 || {
	echo "no openscreen checkout at $OPENSCREEN_DIR (set \$OPENSCREEN_DIR or run setup-openscreen.sh)" >&2
	exit 1
}
OS() { git -C "$OPENSCREEN_DIR" "$@"; }

# 1) make sure we have the pin commit and the latest branch tip locally.
OS cat-file -e "${PIN}^{commit}" 2>/dev/null || OS fetch --tags origin "$PIN" || OS fetch origin
if OS remote get-url fork >/dev/null 2>&1; then
	OS fetch fork "$FORK_BRANCH"
	BRANCH_REF="fork/$FORK_BRANCH"
else
	# No fork remote configured: fall back to a local branch of the same name.
	OS rev-parse --verify "refs/heads/$FORK_BRANCH" >/dev/null 2>&1 || {
		echo "no 'fork' remote and no local branch '$FORK_BRANCH' in $OPENSCREEN_DIR" >&2
		exit 1
	}
	BRANCH_REF="$FORK_BRANCH"
fi

# 2) sanity: the pin must be an ancestor of the branch (patch = pin..branch).
OS merge-base --is-ancestor "$PIN" "$BRANCH_REF" || {
	echo "pin $PIN is not an ancestor of $BRANCH_REF — bump the pin first" >&2
	exit 1
}

TREE="$(OS rev-parse "${BRANCH_REF}^{tree}")"

# 3) cut a deterministic squashed patch (pin -> branch tree). Author/date come
#    from the branch tip so the patch file is reproducible run-to-run; the commit
#    hash after `git am` is NOT (committer date varies), so we verify the TREE.
TIP_DATE="$(OS log -1 --format=%cI "$BRANCH_REF")"
TIP_AN="$(OS log -1 --format=%an "$BRANCH_REF")"
TIP_AE="$(OS log -1 --format=%ae "$BRANCH_REF")"
SQUASH="$(
	cd "$OPENSCREEN_DIR"
	export GIT_AUTHOR_NAME="$TIP_AN" GIT_AUTHOR_EMAIL="$TIP_AE" GIT_AUTHOR_DATE="$TIP_DATE"
	export GIT_COMMITTER_NAME="$TIP_AN" GIT_COMMITTER_EMAIL="$TIP_AE" GIT_COMMITTER_DATE="$TIP_DATE"
	git commit-tree "$TREE" -p "$PIN" -m "$SUBJECT"
)"
TMP_PATCH="$(mktemp)"
OS format-patch -1 --stdout "$SQUASH" >"$TMP_PATCH"

# 4) verify: apply onto the pin in a throwaway worktree, compare the tree.
WT="$(mktemp -d)"
cleanup() {
	OS worktree remove --force "$WT" 2>/dev/null || rm -rf "$WT"
	[[ -n "$TMP_PATCH" ]] && rm -f "$TMP_PATCH"
}
trap cleanup EXIT
OS worktree add --detach "$WT" "$PIN" >/dev/null
git -C "$WT" am "$TMP_PATCH" >/dev/null || {
	git -C "$WT" am --abort 2>/dev/null || true
	echo "FAIL: freshly cut patch does not apply onto pin $PIN" >&2
	exit 1
}
GOT="$(git -C "$WT" rev-parse 'HEAD^{tree}')"
[[ "$GOT" == "$TREE" ]] || {
	echo "FAIL: applied tree ($GOT) != branch tree ($TREE) — refusing to write" >&2
	exit 1
}

# 5) commit the verified artifacts: patch file + pin's patched= tree hash.
mkdir -p "$(dirname "$PATCH")"
mv "$TMP_PATCH" "$PATCH"
TMP_PATCH="" # consumed; cleanup must not rm the installed patch
if grep -qE '^patched=' "$PIN_FILE"; then
	sed -i "s#^patched=.*#patched=$TREE#" "$PIN_FILE"
else
	printf 'patched=%s\n' "$TREE" >>"$PIN_FILE"
fi

echo "regenerated $PATCH_REL"
echo "patched=$TREE  (verified: pin + patch == $BRANCH_REF tree)"
