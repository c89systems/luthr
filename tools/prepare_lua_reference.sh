#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cache="$root/.cache/vendor"
while read -r name digest source; do
  archive="$cache/$name"
  if [[ -f "$archive" ]]; then
    printf '%s  %s\n' "$digest" "$archive" | sha256sum --check --strict
    continue
  fi
  mkdir -p -- "$cache"
  pending=$(mktemp "$cache/$name.download.XXXXXX")
  trap 'rm -- "$pending"' EXIT
  curl --fail --location --proto '=https' --tlsv1.2 --retry 2 \
    --connect-timeout 15 --max-time 120 --output "$pending" "$source"
  printf '%s  %s\n' "$digest" "$pending" | sha256sum --check --strict
  mv -- "$pending" "$archive"
  trap - EXIT
done <<'ARCHIVES'
lua-5.5.1.tar.gz 1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce https://www.lua.org/ftp/lua-5.5.1.tar.gz
lua-5.5.1-tests.tar.gz da07b543872dc0bb2ff12aabd0c248578d78df3eb6b67efdc537a46d455c7f31 https://www.lua.org/tests/lua-5.5.1-tests.tar.gz
lpeg-1.1.0.tar.gz 4b155d67d2246c1ffa7ad7bc466c1ea899bbc40fef0257cc9c03cecbaed4352a https://www.inf.puc-rio.br/~roberto/lpeg/lpeg-1.1.0.tar.gz
luaunit-3.5.tar.gz bd0085dc35c847afab0dbafc3669490ddc81b85623068ee0debe7667ffc03454 https://codeload.github.com/bluebird75/luaunit/tar.gz/refs/tags/LUAUNIT_V3_5
ARCHIVES
