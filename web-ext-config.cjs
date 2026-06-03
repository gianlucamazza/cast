// web-ext configuration. Use via `npx web-ext <cmd>`.
module.exports = {
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
