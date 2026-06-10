## What

<!-- One or two sentences: what changes and why. -->

## Checklist

- [ ] `npm run lint` + prettier/clang-format checks pass
- [ ] Native changes: `bash native/integration/build.sh` builds and the
      castbridge unit tests pass (see CONTRIBUTING.md)
- [ ] Fork changes: patch + pin regenerated (`regen-patch.sh`)
- [ ] User-facing changes: `CHANGELOG.md` `[Unreleased]` entry added
- [ ] Docs touched if behavior/protocol changed (`README.md`,
      `native/castbridge/README.md`)
