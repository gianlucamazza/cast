#!/usr/bin/env node
// Verify the identifiers shared between the extension and the native host stay
// in sync: the native host name (extension/manifest is the consumer, the
// install manifest the producer) and the extension id allowed to use it. These
// are hardcoded in three places; drift means a silent "nohost" at runtime.
// Exits non-zero on any mismatch. No dependencies.
import { readFileSync } from "fs";
import { fileURLToPath } from "url";
import path from "path";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const read = (p) => readFileSync(path.join(root, p), "utf8");

const HOST_MANIFEST = "install/it.gianlucamazza.castbridge.json.in";

const extManifest = JSON.parse(read("extension/manifest.json"));
const hostManifest = JSON.parse(read(HOST_MANIFEST).replace("@PATH@", "/x"));
const background = read("extension/background/background.js");

const extId = extManifest.browser_specific_settings?.gecko?.id;
const hostName = hostManifest.name;
const allowed = hostManifest.allowed_extensions || [];
const hostNameInBg = (background.match(/const HOST_NAME = "([^"]+)"/) || [])[1];
const hostNameFromFile = path.basename(HOST_MANIFEST, ".json.in");

let failed = false;
function check(ok, msg) {
  if (!ok) {
    console.error(`ID MISMATCH: ${msg}`);
    failed = true;
  }
}

check(
  hostNameInBg === hostName,
  `background.js HOST_NAME "${hostNameInBg}" != host manifest name "${hostName}"`,
);
check(
  hostNameFromFile === hostName,
  `host manifest filename "${hostNameFromFile}" != its name field "${hostName}"`,
);
check(
  extId && allowed.includes(extId),
  `extension id "${extId}" not in allowed_extensions [${allowed.join(", ")}]`,
);

if (failed) process.exit(1);
console.log(`OK: host "${hostName}" / extension "${extId}" ids are in sync`);
