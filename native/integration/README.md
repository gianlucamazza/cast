# Native build integration

`castbridge` links the openscreen Cast libraries, so it must be built **inside an
openscreen fork checkout**: openscreen at the pinned commit plus the Wayland/
H.264 desktop-mirror patch (the same fork that provides `cast_sender`).

Two steps, from a clean machine:

```bash
bash native/integration/setup-openscreen.sh   # provision the fork (once)
bash native/integration/build.sh              # add + build castbridge
```

`setup-openscreen.sh` provisions the checkout **reproducibly** from versioned
inputs in `openscreen.pin`, with two interchangeable strategies:

- **default** — clone the private fork (`fork` @ `fork_branch`) straight at the
  patched branch. Fast, no `git am`; needs SSH access to the fork.
- **`--from-upstream`** — clone upstream openscreen at the `pin` and apply the
  versioned patch (`patches/0001-*.patch`) with `git am`. No fork access needed;
  use this in CI or when sharing with others.

Both then `gclient sync` the third_party deps and `gn gen out/Default` with the
recorded `gn_args`, ending at the same patched tree.

`build.sh` is the per-change entry point. It:

1. verifies the checkout exists, is configured, and carries the mirror patch
   (else points you back at `setup-openscreen.sh`),
2. symlinks `native/castbridge/` into `<fork>/cast/castbridge` (single source of
   truth — editing through the fork checkout edits this repo directly), and adds
   that link plus `out/Default/compile_commands.json` to the fork's
   `.git/info/exclude` so they don't show up as untracked,
3. adds `cast/castbridge:castbridge` to the root `gn_all` group,
4. grants our target visibility on the few deps that whitelist only
   `cast_sender` (`discovery:dnssd`, `platform:standalone_impl`,
   `cast/common:certificate_boringssl`) — idempotent edits, each verified,
5. runs `ninja -C out/Default cast/castbridge:castbridge` and checks the binary
   actually came out.

The resulting binary is `<fork>/out/Default/castbridge` (sibling of
`cast_sender`, which `MirrorController` spawns for mirroring).

## Fork checkout hygiene

`build.sh` mutates the fork checkout in two ways. This is expected and is **not**
meant to be committed to the fork:

- It symlinks our sources at `<fork>/cast/castbridge` and writes a generated
  compile DB. Both are ignored locally via `.git/info/exclude` (step 2) — we
  never edit the fork's tracked `.gitignore`, which would itself show as a diff.
- Step 4 edits four tracked files in place (`BUILD.gn`, `discovery/BUILD.gn`,
  `platform/BUILD.gn`, `cast/common/BUILD.gn`) with a `sed` that adds our
  visibility/`gn_all` lines. These show as modifications in `git status` and
  can't be ignored. To reset the checkout to a clean tree:

  ```bash
  git -C "$OPENSCREEN_DIR" checkout -- \
    BUILD.gn discovery/BUILD.gn platform/BUILD.gn cast/common/BUILD.gn
  ```

The working checkout sits on branch `skill-cast/screen-mirror`, a local alias of
`wayland-h264-sender` (the `fork_branch` published on the fork; see
`openscreen.pin`) at the same commit. `setup-openscreen.sh` provisions
`wayland-h264-sender` directly.

## Pinned inputs

`openscreen.pin` records everything needed to rebuild the checkout: the `fork`
URL + `fork_branch`, the upstream `pin` commit, the `patch` path, the expected
`patched` tree, and the `gn_args`.

The **fork branch is the single source of truth** for the sender. The `patch` and
the `patched` tree hash are **generated artifacts**, not hand-maintained — after
any change to the sender on the fork branch, regenerate them:

```bash
bash native/integration/regen-patch.sh
```

It re-cuts `patches/0001-*.patch` from `pin..fork_branch`, applies it onto the
pin in a throwaway worktree, and asserts the result is byte-identical to the fork
branch tree before writing the patch + `patched=`. `setup-openscreen.sh` enforces
the same invariant on every provision (both strategies), so a drifted patch/pin
fails loudly instead of silently building a stale tree. To bump the upstream
`pin`, rebase the fork branch onto the new commit, update `pin`, then run
`regen-patch.sh`.

## Configuration

- `OPENSCREEN_DIR` — path to the fork checkout. Defaults to
  `../openscreen-build/openscreen`, a sibling of this repo, derived from the
  script's own location (no absolute path baked in). Set this to point at any
  other layout (CI, packaging).
- `OUT_DIR` — gn output dir relative to the checkout (default `out/Default`).
- `NINJA` — ninja binary (auto-detected; falls back to depot_tools).

ninja auto-regenerates the gn graph via the buildtools `gn` recorded in
`build.ninja`, so a separate `gn` on PATH is not required.

All of "obtaining a fork checkout from scratch" is automated by
`setup-openscreen.sh`; there is no manual sequence to follow. The fresh
`gclient sync` downloads ~GB of deps and the first build is slow; subsequent
`build.sh` runs are incremental.

## Editor / LSP

The castbridge sources include their headers by fork-relative path
(`cast/castbridge/…`, `platform/…`, `json/…`), which only exist inside the fork
checkout. An editor's clang language server (clangd) doesn't know those include
paths, so it can't open the first header and falls back to spurious diagnostics
(*file not found*, then a cascade of *`std::string` aka `int`*). These are **not
real errors** — `ninja` builds the same files cleanly; only the editor view is
wrong.

To fix the editor view, generate a `.clangd` pointing at the fork's headers
(run once, after `setup-openscreen.sh`):

```bash
bash native/integration/gen-clangd.sh
```

It writes `native/castbridge/.clangd` (with the fork's `-I` paths) and exports
`out/Default/compile_commands.json` from ninja. Both are machine-specific and
git-ignored. Reopen the C++ files afterwards. Honors `$OPENSCREEN_DIR`.
