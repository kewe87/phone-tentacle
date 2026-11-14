#!/usr/bin/env node
// Runs the binary install.js put next to this file.
const { spawnSync } = require("child_process");
const path = require("path");

const exe = path.join(__dirname, process.platform === "win32" ? "tentacle.exe" : "tentacle");
const r = spawnSync(exe, process.argv.slice(2), { stdio: "inherit" });
if (r.error) {
  console.error(`tentacle: ${r.error.message} (reinstall: npm install -g phone-tentacle)`);
  process.exit(1);
}
process.exit(r.status ?? 1);
