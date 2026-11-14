#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 3 ]]; then
  printf 'usage: exports.sh <nm> <shared-library> <export-map>\n' >&2
  exit 2
fi
actual=$("$1" -D --defined-only "$2" | awk '{ print $NF }' | sort -u)
expected=$(sed -n 's/^[[:space:]]*\([A-Za-z_][A-Za-z0-9_]*\);$/\1/p' "$3" | sort -u)
if [[ "$actual" != "$expected" ]]; then
  printf 'Public export mismatch.\nExpected:\n%s\nActual:\n%s\n' "$expected" "$actual" >&2
  exit 1
fi
