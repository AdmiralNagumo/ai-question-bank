#!/usr/bin/env bash
set -euo pipefail
version=24.19.0
case "$(uname -m)" in
  x86_64) arch=x64 ;;
  aarch64) arch=arm64 ;;
  *) echo 'Unsupported Node architecture' >&2; exit 1 ;;
esac
runtime_root="${XDG_DATA_HOME:-$HOME/.local/share}/ai-question-bank"
archive="node-v${version}-linux-${arch}.tar.xz"
temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT
mkdir -p "$runtime_root"
cd "$temp_dir"
curl --retry 2 --connect-timeout 10 -fsSL "https://nodejs.org/dist/v${version}/SHASUMS256.txt" -o SHASUMS256.txt
curl --retry 2 --connect-timeout 10 -fsSL "https://nodejs.org/dist/v${version}/${archive}" -o "$archive"
awk -v name="$archive" '$2 == name { print }' SHASUMS256.txt > selected.sha256
test -s selected.sha256
sha256sum -c selected.sha256
tar -xJf "$archive" -C "$runtime_root"
ln -sfn "$runtime_root/node-v${version}-linux-${arch}" "$runtime_root/node"
"$runtime_root/node/bin/node" --version
