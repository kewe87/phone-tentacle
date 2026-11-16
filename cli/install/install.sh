#!/bin/sh
# Installs tentacle from this repository's GitHub Releases (tags vX.Y.Z, made by .github/workflows/release.yml) and
# checks the archive against the release's SHA256SUMS:
#   curl -fsSL https://raw.githubusercontent.com/kewe87/phone-tentacle/main/cli/install/install.sh | sh
# Env: TENTACLE_VERSION=0.4.0 (default: the latest release), TENTACLE_BIN_DIR (default ~/.local/bin),
#      TENTACLE_ARCHIVE=/path/tentacle_<os>_<arch>.tar.gz to install a local build (SHA256SUMS next to it,
#      or TENTACLE_CHECKSUMS=/path), TENTACLE_FFMPEG=yes|no to answer the ffmpeg question without a prompt.
set -eu

REPO=kewe87/phone-tentacle
BIN_DIR=${TENTACLE_BIN_DIR:-$HOME/.local/bin}

say() { printf '%s\n' "$*"; }
die() { printf 'tentacle install: %s\n' "$*" >&2; exit 1; }

case $(uname -s) in
  Darwin) os=darwin ;;
  Linux) os=linux ;;
  *) die "unsupported OS $(uname -s) (on Windows use install.ps1)" ;;
esac
case $(uname -m) in
  x86_64 | amd64) arch=amd64 ;;
  arm64 | aarch64) arch=arm64 ;;
  *) die "unsupported CPU $(uname -m)" ;;
esac
# macOS: one universal file for Apple silicon and Intel
if [ "$os" = darwin ]; then name=tentacle_darwin_universal.tar.gz; else name=tentacle_${os}_${arch}.tar.gz; fi

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if [ -n "${TENTACLE_ARCHIVE:-}" ]; then
  [ -f "$TENTACLE_ARCHIVE" ] || die "no file $TENTACLE_ARCHIVE"
  cp "$TENTACLE_ARCHIVE" "$tmp/$name"
  sums=${TENTACLE_CHECKSUMS:-$(dirname "$TENTACLE_ARCHIVE")/SHA256SUMS}
  [ -f "$sums" ] || die "no checksums file $sums"
  cp "$sums" "$tmp/SHA256SUMS"
  say "installing tentacle from $TENTACLE_ARCHIVE"
else
  version=${TENTACLE_VERSION:-}
  version=${version#v}
  if [ -n "$version" ]; then
    url=https://github.com/$REPO/releases/download/v$version
  else
    url=https://github.com/$REPO/releases/latest/download # the newest published release
    version=latest
  fi
  say "downloading tentacle $version ($os/$arch)"
  curl -fsSL -o "$tmp/$name" "$url/$name" || die "download failed: $url/$name"
  curl -fsSL -o "$tmp/SHA256SUMS" "$url/SHA256SUMS" || die "download failed: $url/SHA256SUMS"
fi

want=$(awk -v n="$name" '$2 == n || $2 == "*"n { print $1 }' "$tmp/SHA256SUMS")
[ -n "$want" ] || die "$name is not listed in SHA256SUMS"
got=$(sha256 "$tmp/$name")
[ "$want" = "$got" ] || die "checksum mismatch for $name (expected $want, got $got)"

tar -xzf "$tmp/$name" -C "$tmp" tentacle || die "bad archive $name"
mkdir -p "$BIN_DIR"
install -m 755 "$tmp/tentacle" "$BIN_DIR/tentacle"
say "installed $BIN_DIR/tentacle ($("$BIN_DIR/tentacle" version))"

case ":$PATH:" in
  *":$BIN_DIR:"*) ;;
  *) say "warning: $BIN_DIR is not on your PATH; add it, e.g.: echo 'export PATH=\"$BIN_DIR:\$PATH\"' >> ~/.profile" ;;
esac

# ffmpeg decodes the mirrored screen (screenshots need it).
if ! command -v ffmpeg >/dev/null 2>&1; then
  installer=""
  if command -v brew >/dev/null 2>&1; then installer="brew install ffmpeg"
  elif command -v apt-get >/dev/null 2>&1; then installer="sudo apt-get install -y ffmpeg"
  elif command -v dnf >/dev/null 2>&1; then installer="sudo dnf install -y ffmpeg"
  fi
  answer=${TENTACLE_FFMPEG:-}
  if [ -n "$installer" ] && [ -z "$answer" ] && [ -r /dev/tty ]; then
    printf 'ffmpeg is needed for screenshots. Run "%s" now? [y/N] ' "$installer"
    read -r answer </dev/tty || answer=n
  fi
  case $answer in
    y | Y | yes) [ -n "$installer" ] && sh -c "$installer" ;;
    *) say "ffmpeg not found: install it for screenshots (${installer:-see https://ffmpeg.org/download.html})" ;;
  esac
fi

cat <<EOF

Next:
  1. The board has to be on your Wi-Fi (its setup hotspot Tentacle-XXXX, see the README).
  2. Double-press BOOT on the board, then:  tentacle pair   (compare the code, press BOOT once more)
  3. On the iPhone: Control Center > Screen Mirroring > Tentacle XXXX
  4. Let your agent use it:
       claude mcp add tentacle -- tentacle mcp
       codex mcp add tentacle -- tentacle mcp
       tentacle skill install      (Claude Code skill)
EOF
