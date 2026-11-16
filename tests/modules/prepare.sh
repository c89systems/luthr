#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
input=${1:-"$root/.cache/modules/archives"}
module=${2:-lonejson}
case "$module" in
  lonejson)
    version=0.50.0
    trees=(lonejson-0.50.0 liblonejson-0.50.0-x86_64-linux-gnu liblonejson-0.50.0-x86_64-linux-musl)
    ;;
  libmdf)
    version=0.23.1
    trees=(libmdf-lua-0.23.1 libmdf-0.23.1-x86_64-linux-gnu libmdf-0.23.1-x86_64-linux-musl)
    ;;
  *) printf 'Unknown acceptance module: %s (expected lonejson or libmdf)\n' "$module" >&2; exit 2 ;;
esac
manifest="$root/tests/modules/$module-$version.sha256"
cache="$root/.cache/modules"
if [[ ! -d "$input" ]]; then
  printf 'Module archives missing: %s\nUse make prepare-modules MODULE=%s MODULE_ARCHIVE_DIR=/path/to/release-artifacts\n' "$input" "$module" >&2
  exit 2
fi
input=$(cd -- "$input" && pwd)
# Validate all declared inputs before changing the owned cache.
(cd -- "$input" && sha256sum --check --strict "$manifest")
mkdir -p -- "$cache/archives"
while read -r digest name; do
  if [[ "$input" != "$cache/archives" ]]; then
    cp -- "$input/$name" "$cache/archives/$name"
  fi
done < "$manifest"
(cd -- "$cache/archives" && sha256sum --check --strict "$manifest")
for name in "${trees[@]}"; do
  rm -rf -- "$cache/$name"
done
while read -r digest name; do
  (cd -- "$cache" && cmake -E tar xzf "archives/$name")
done < "$manifest"
# Restaging source/archive timestamps must invalidate existing native outputs.
if [[ "$module" == lonejson ]]; then
  touch "$cache/${trees[0]}/src/lua/"*.[ch] "$cache/${trees[0]}/include/lonejson.h"
  for name in "${trees[@]:1}"; do
    touch "$cache/$name/include/lonejson.h" "$cache/$name/lib/liblonejson.a"
  done
else
  touch "$cache/${trees[0]}/lua/libmdf_core.c"
  for name in "${trees[@]:1}"; do
    touch "$cache/$name/include/libmdf.h" "$cache/$name/include/libmdf/mdf.h" "$cache/$name/lib/libmdf.a"
  done
fi
