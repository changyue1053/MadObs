#!/usr/bin/env bash
# Linux-only. Requires bwrap; no fallback to a weaker "isolation" claim.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
package="$(cd "${1:?Pass installed MadObs prefix}" && pwd)"
cmake="${CMAKE:-$(command -v cmake)}"
cmake_root="$(cd "$(dirname "$cmake")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/madobs-isolated-consumer-XXXXXXXX")"
command -v bwrap >/dev/null
bwrap --tmpfs / --ro-bind /usr /usr --ro-bind /bin /bin --ro-bind /lib /lib \
  --ro-bind /lib64 /lib64 --ro-bind /etc /etc --tmpfs /tmp --dir /workspace \
  --ro-bind "$package" /package --ro-bind "$root/samples/encrypted_logs_c" /src \
  --ro-bind "$cmake_root" /cmake --bind "$work" /out --proc /proc --dev /dev \
  --setenv TMPDIR /tmp --setenv ORIGINAL_ROOT "$root" \
  --setenv ORIGINAL_PROVIDER_SOURCE "${MADOBS_PROVIDER_SOURCE:-}" \
  --setenv ORIGINAL_PROVIDER_BUILD "${MADOBS_PROVIDER_BUILD:-}" \
  --chdir /out -- /bin/bash -c '
    set -e
    test ! -e "$ORIGINAL_ROOT"
    if test -n "$ORIGINAL_PROVIDER_SOURCE"; then test ! -e "$ORIGINAL_PROVIDER_SOURCE"; fi
    if test -n "$ORIGINAL_PROVIDER_BUILD"; then test ! -e "$ORIGINAL_PROVIDER_BUILD"; fi
    echo "Original source/build/workspace paths invisible"
    /cmake/bin/cmake -S /src -B /out/build -DCMAKE_BUILD_TYPE=Release -DMadObs_DIR=/package/lib/cmake/MadObs
    /cmake/bin/cmake --build /out/build --parallel 4
    /out/build/consumer
    echo "Isolated relocated encrypted C consumer exit 0"
  '
