#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache="$root/.cache/vendor"
archive="$cache/lua-5.5.1.tar.gz"
digest=1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce
if [[ -f "$archive" ]]; then
  printf '%s  %s\n' "$digest" "$archive" | sha256sum --check --strict
  exit 0
fi
mkdir -p -- "$cache"
pending=$(mktemp "$cache/lua-5.5.1.download.XXXXXX")
trap 'rm -- "$pending"' EXIT
curl --fail --location --proto '=https' --tlsv1.2 --retry 2 \
  --connect-timeout 15 --max-time 120 \
  --output "$pending" https://www.lua.org/ftp/lua-5.5.1.tar.gz
printf '%s  %s\n' "$digest" "$pending" | sha256sum --check --strict
mv -- "$pending" "$archive"
trap - EXIT
