#!/usr/bin/env bash
# A genuinely runtime-free C consumer, runnable with an existing host C compiler.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/include/madobs"
cp include/madobs/observer.h include/madobs/privacy.h "$work/include/madobs/"
cp ios-validation/Consumer/Tests/APIOnly.c "$work/consumer.c"
"${CC:-cc}" -std=c11 -Wall -Wextra -Wpedantic -Werror -DMADOBS_API_ONLY_MAIN \
  -I"$work/include" "$work/consumer.c" -o "$work/api-only"
"$work/api-only"
echo 'PASS: independent copied-header C API-only consumer (host only, not iOS)'
