#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
input=${1:-"$root/.cache/modules/archives"}
manifest="$root/tests/modules/lonejson-0.50.0.sha256"
cache="$root/.cache/modules"
if [[ ! -d "$input" ]]; then
  printf 'Module archives missing: %s\nUse make prepare-modules MODULE_ARCHIVE_DIR=/path/to/lonejson-0.50.0-artifacts\n' "$input" >&2
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
for name in lonejson-0.50.0 liblonejson-0.50.0-x86_64-linux-gnu liblonejson-0.50.0-x86_64-linux-musl; do
  rm -rf -- "$cache/$name"
done
while read -r digest name; do
  (cd -- "$cache" && cmake -E tar xzf "archives/$name")
done < "$manifest"
# Restaging source/archive timestamps must invalidate existing native outputs.
touch "$cache/lonejson-0.50.0/src/lua/"*.[ch] \
  "$cache/lonejson-0.50.0/include/lonejson.h"
for target in x86_64-linux-gnu x86_64-linux-musl; do
  touch "$cache/liblonejson-0.50.0-$target/include/lonejson.h" \
    "$cache/liblonejson-0.50.0-$target/lib/liblonejson.a"
done
