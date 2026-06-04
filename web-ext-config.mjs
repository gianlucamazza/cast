// web-ext configuration. Use via `npx web-ext <cmd>` or the npm scripts.
// ESM (.mjs) with a default export: web-ext 8.10+ loads config via dynamic
// import(), and a CommonJS (.cjs) `module.exports` leaks a synthetic
// "module.exports" named export that web-ext rejects as non-camelCase.
export default {
  sourceDir: "./extension",
  artifactsDir: "./.web-ext-artifacts",
  run: {
    firefox: "/usr/bin/librewolf",
    target: ["firefox-desktop"],
    browserConsole: true,
  },
  build: {
    overwriteDest: true,
  },
  ignoreFiles: ["**/*.md"],
};
