# Contributing

Concise by design — see `CLAUDE.md` for the architecture map and
`native/castbridge/README.md` / `native/integration/README.md` for the deep
dives. Comments and docs are in English; keep changes small and avoid
over-engineering.

## Setup

```bash
npm install                                    # web-ext tooling
bash native/integration/setup-openscreen.sh    # openscreen fork (once, ~GB)
bash native/integration/build.sh               # -> castbridge binary
bash install/install-host.sh                   # native-messaging host
npm run start                                  # LibreWolf with the extension
```

## Checks to run before pushing

```bash
npm run lint
npx prettier --check "extension/**/*.{js,json,css,html}" "scripts/*.mjs"
node scripts/check-locales.mjs && node scripts/check-ids.mjs
shellcheck --severity=error install/*.sh native/integration/*.sh
clang-format --dry-run --Werror native/castbridge/*.cc native/castbridge/*.h
bash native/integration/build.sh               # native changes must build
```

Native unit tests (local pre-release gate; CI cannot build the fork):

```bash
ninja -C "$OPENSCREEN_DIR/out/Default" cast/castbridge:castbridge_unittests \
  cast/castbridge:castbridge_controller_unittests
"$OPENSCREEN_DIR"/out/Default/castbridge_unittests
"$OPENSCREEN_DIR"/out/Default/castbridge_controller_unittests
```

## Conventions

- **Style**: prettier for JS/JSON/CSS/HTML (`.prettierrc`), Chromium
  clang-format for C++ (`native/castbridge/.clang-format`), tabs in shell
  scripts (`.editorconfig`).
- **Commits**: conventional commits (`feat(scope): ...`, `fix: ...`,
  `ci: ...`, `docs: ...`). Keep each commit buildable.
- **Changelog**: add an entry under `[Unreleased]` in `CHANGELOG.md` for
  user-facing changes.
- **Fork changes**: anything touching the openscreen fork branch requires
  `bash native/integration/regen-patch.sh` and committing the regenerated
  patch + pin (CI cannot check this; `--verify-only` does, locally).
- **Releases**: see the Release section in `README.md`.

## Security

See `SECURITY.md` for the security model and how to report vulnerabilities
privately.
