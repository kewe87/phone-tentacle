// postinstall: downloads the tentacle binary (binaryVersion, else this package's version) from GitHub Releases and checks its
// SHA-256 against the release's SHA256SUMS (tag vX.Y.Z, .github/workflows/release.yml). TENTACLE_ARCHIVE=/path installs a
// local build (SHA256SUMS next to it).
const { createHash } = require("crypto");
const { execFileSync } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const pkg = require("./package.json");
const version = pkg.binaryVersion || pkg.version; // binaryVersion: an npm release that reuses older binaries
const goos = { darwin: "darwin", linux: "linux", win32: "windows" }[process.platform];
const goarch = { x64: "amd64", arm64: "arm64" }[process.arch];
if (!goos || !goarch) throw new Error(`tentacle: unsupported platform ${process.platform}/${process.arch}`);
// macOS: one universal file for Apple silicon and Intel
const name = goos === "darwin" ? "tentacle_darwin_universal.tar.gz" : `tentacle_${goos}_${goarch}.${goos === "windows" ? "zip" : "tar.gz"}`;
const base = `https://github.com/kewe87/phone-tentacle/releases/download/v${version}`;

async function fetchBytes(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`tentacle: download failed (${res.status}): ${url}`);
  return Buffer.from(await res.arrayBuffer());
}

async function main() {
  const local = process.env.TENTACLE_ARCHIVE;
  const archive = local ? fs.readFileSync(local) : await fetchBytes(`${base}/${name}`);
  const sums = local
    ? fs.readFileSync(process.env.TENTACLE_CHECKSUMS || path.join(path.dirname(local), "SHA256SUMS"), "utf8")
    : (await fetchBytes(`${base}/SHA256SUMS`)).toString("utf8");
  const line = sums.split("\n").find((l) => [name, `*${name}`].includes(l.trim().split(/\s+/)[1]));
  if (!line) throw new Error(`tentacle: ${name} is not in SHA256SUMS`);
  const got = createHash("sha256").update(archive).digest("hex");
  if (line.trim().split(/\s+/)[0].toLowerCase() !== got) throw new Error(`tentacle: checksum mismatch for ${name}`);

  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "tentacle-"));
  const file = path.join(tmp, name);
  fs.writeFileSync(file, archive);
  execFileSync("tar", ["-xf", file, "-C", tmp]); // bsdtar on Windows 10+ reads zip too
  const exe = goos === "windows" ? "tentacle.exe" : "tentacle";
  const dest = path.join(__dirname, "bin", exe);
  fs.copyFileSync(path.join(tmp, exe), dest);
  fs.chmodSync(dest, 0o755);
  fs.rmSync(tmp, { recursive: true, force: true });
  try {
    execFileSync("ffmpeg", ["-version"], { stdio: "ignore" });
  } catch {
    console.warn("tentacle: ffmpeg not found; install it for screenshots (brew/apt/dnf install ffmpeg, winget install Gyan.FFmpeg)");
  }
}

main().catch((e) => {
  console.error(e.message);
  process.exit(1);
});
