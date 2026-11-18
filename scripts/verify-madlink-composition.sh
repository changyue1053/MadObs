#!/usr/bin/env bash
# Opt-in fixed Linux host fixture. No SDK dependency is added to MadObs defaults.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
: "${MADOBS_COMPOSITION_PACKAGE:?Set fixed installed MadObs prefix}"
: "${MADLINK_COMPOSITION_SOURCE:?Set fixed MadLink source snapshot}"
: "${MADLINK_COMPOSITION_ARCHIVE:?Set fixed libimlink.a}"
: "${MADLINK_COMPOSITION_CORE_ARCHIVE:?Set fixed libimlink_core.a}"
: "${MADLINK_COMPOSITION_SSL_ARCHIVE:?Set matching libssl.a}"
: "${MADOBS_COMPOSITION_WORK_ROOT:?Set your own workspace directory outside /tmp}"
command -v bwrap >/dev/null
cmake=${CMAKE:-$(command -v cmake)}
cmake_root=$(cd "$(dirname "$cmake")/.." && pwd)
work=$(mktemp -d "$MADOBS_COMPOSITION_WORK_ROOT/madobs-composition.XXXXXX")
python3 "$root/scripts/composition/prepare.py" \
  --lock "$root/deps/madlink-host-composition.lock.json" \
  --package "$MADOBS_COMPOSITION_PACKAGE" --madlink-source "$MADLINK_COMPOSITION_SOURCE" \
  --madlink-archive "$MADLINK_COMPOSITION_ARCHIVE" --madlink-core-archive "$MADLINK_COMPOSITION_CORE_ARCHIVE" \
  --ssl-archive "$MADLINK_COMPOSITION_SSL_ARCHIVE" --sample "$root/samples/madlink_composition" --output "$work/bundle"
mkdir "$work/output" "$work/tmp"
echo "Verified private fixture: $work"
bwrap --tmpfs / --ro-bind /usr /usr --ro-bind /bin /bin --ro-bind /lib /lib \
  --ro-bind /lib64 /lib64 --ro-bind /etc /etc --proc /proc --dev /dev \
  --ro-bind "$work/bundle" /fixture --bind "$work/output" /out --bind "$work/tmp" /tmp \
  --ro-bind "$cmake_root" /cmake --setenv TMPDIR /tmp \
  --setenv ORIGINAL_MADOBS "$root" --setenv ORIGINAL_MADLINK "$MADLINK_COMPOSITION_SOURCE" \
  --setenv ORIGINAL_PACKAGE "$MADOBS_COMPOSITION_PACKAGE" \
  --setenv ORIGINAL_LINK_ARCHIVE "$MADLINK_COMPOSITION_ARCHIVE" \
  --setenv ORIGINAL_CORE_ARCHIVE "$MADLINK_COMPOSITION_CORE_ARCHIVE" \
  --setenv ORIGINAL_SSL_ARCHIVE "$MADLINK_COMPOSITION_SSL_ARCHIVE" \
  --chdir /out -- /bin/bash -c '
    set -euo pipefail
    for name in ORIGINAL_MADOBS ORIGINAL_MADLINK ORIGINAL_PACKAGE ORIGINAL_LINK_ARCHIVE ORIGINAL_CORE_ARCHIVE ORIGINAL_SSL_ARCHIVE; do
      test ! -e "${!name}"
    done
    echo "Original SDK/source/build paths invisible"
    /usr/bin/g++ --version
    /cmake/bin/cmake --version
    /cmake/bin/cmake -S /fixture/sample -B /out/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
      -DMadObs_DIR=/fixture/package/lib/cmake/MadObs \
      -DMADLINK_SOURCE=/fixture/madlink -DMADLINK_ARCHIVE=/fixture/archives/libimlink.a \
      -DMADLINK_CORE_ARCHIVE=/fixture/archives/libimlink_core.a -DBORINGSSL_SSL_ARCHIVE=/fixture/archives/libssl.a
    /cmake/bin/cmake --build /out/build --parallel 4
    cat /out/build/CMakeFiles/composition.dir/link.txt
    /cmake/bin/ctest --test-dir /out/build --repeat until-fail:20 --output-on-failure
  '
