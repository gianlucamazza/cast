#!/usr/bin/env node
// Verify every _locales/<lang>/messages.json defines the same set of keys as the
// default locale (en). Keeps translations from silently drifting. Exits non-zero
// on any missing/extra key. No dependencies.
import { readdirSync, readFileSync } from "fs";
import { fileURLToPath } from "url";
import path from "path";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const localesDir = path.join(root, "extension", "_locales");
const DEFAULT = "en";

function keysOf(lang) {
  const file = path.join(localesDir, lang, "messages.json");
  return new Set(Object.keys(JSON.parse(readFileSync(file, "utf8"))));
}

const langs = readdirSync(localesDir, { withFileTypes: true })
  .filter((d) => d.isDirectory())
  .map((d) => d.name);

if (!langs.includes(DEFAULT)) {
  console.error(`Missing default locale "${DEFAULT}" in ${localesDir}`);
  process.exit(1);
}

const base = keysOf(DEFAULT);
let failed = false;

for (const lang of langs.filter((l) => l !== DEFAULT)) {
  const cur = keysOf(lang);
  const missing = [...base].filter((k) => !cur.has(k));
  const extra = [...cur].filter((k) => !base.has(k));
  if (missing.length || extra.length) {
    failed = true;
    console.error(`Locale "${lang}" differs from "${DEFAULT}":`);
    if (missing.length) console.error(`  missing: ${missing.join(", ")}`);
    if (extra.length) console.error(`  extra:   ${extra.join(", ")}`);
  }
}

if (failed) process.exit(1);
console.log(`i18n parity OK — ${langs.length} locales, ${base.size} keys each.`);
