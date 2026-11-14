#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 2 || ! -f "$1" || -z "$2" || "$2" == / ]]; then
  printf 'usage: clean.sh <Makefile> <owned-fixture-directory>\n' >&2
  exit 2
fi
fixture=$2
mkdir -p "$fixture"
cp "$1" "$fixture/Makefile"
mkdir -p "$fixture/build/.hidden" "$fixture/dist/.hidden" "$fixture/.cache/.hidden" "$fixture/shared-cache"
printf 'preserve\n' >"$fixture/shared-cache/marker"
CPKT_TOOLCHAIN_CACHE="$fixture/shared-cache" CPKT_DEPENDENCY_CACHE="$fixture/shared-cache" make -C "$fixture" clean
for name in build dist .cache; do
  [[ ! -e "$fixture/$name" ]]
done
[[ $(cat "$fixture/shared-cache/marker") == preserve ]]
ln -s "$fixture/shared-cache" "$fixture/.cache"
make -C "$fixture" clean
[[ ! -e "$fixture/.cache" && ! -L "$fixture/.cache" ]]
[[ $(cat "$fixture/shared-cache/marker") == preserve ]]
