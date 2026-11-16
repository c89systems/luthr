#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 2 ]]; then
  printf 'usage: toolchain-required.sh <source> <owned-build-directory>\n' >&2
  exit 2
fi
if output=$(cmake -S "$1" -B "$2" -G Ninja 2>&1); then
  printf 'Configure unexpectedly accepted an undeclared host toolchain\n' >&2
  exit 1
fi
if [[ "$output" != *'Luthr requires CMAKE_TOOLCHAIN_FILE='* ]]; then
  printf 'Configure failed for an unrelated reason:\n%s\n' "$output" >&2
  exit 1
fi
