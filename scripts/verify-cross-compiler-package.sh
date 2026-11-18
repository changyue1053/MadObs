#!/usr/bin/env bash
# Same-host GNU/Clang, default null-provider package consumption; not other ABIs.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cmake=${CMAKE:-cmake}
for compiler in gcc g++ clang clang++; do
  command -v "$compiler" >/dev/null || { echo "Missing compiler: $compiler" >&2; exit 2; }
  command -v "$compiler"
  "$compiler" --version
 done
work=$(mktemp -d "${TMPDIR:-/tmp}/madobs-cross-package.XXXXXX")
echo "Cross-compiler package evidence: $work"
for direction in gcc-to-clang clang-to-gcc; do
  if [[ $direction == gcc-to-clang ]]; then
    producer_cc=gcc; producer_cxx=g++; consumer_cc=clang; consumer_cxx=clang++
  else
    producer_cc=clang; producer_cxx=clang++; consumer_cc=gcc; consumer_cxx=g++
  fi
  producer_cc=$(command -v "$producer_cc"); producer_cxx=$(command -v "$producer_cxx")
  consumer_cc=$(command -v "$consumer_cc"); consumer_cxx=$(command -v "$consumer_cxx")
  pair="$work/$direction"
  echo "BEGIN $direction producer=$producer_cxx consumer=$consumer_cxx"
  "$cmake" -G 'Unix Makefiles' -S "$root" -B "$pair/build" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DMADOBS_BUILD_SHARED=OFF \
    -DMADOBS_BUILD_JNI=OFF -DCMAKE_C_COMPILER="$producer_cc" -DCMAKE_CXX_COMPILER="$producer_cxx"
  "$cmake" --build "$pair/build" --parallel 4
  "$cmake" --install "$pair/build" --prefix "$pair/installed"
  cp -a "$pair/installed" "$pair/relocated"
  sha256sum "$pair/relocated/lib/libmadobs_runtime.a"
  for sample in private_c private_cpp runtime_c api_only; do
    "$cmake" -G 'Unix Makefiles' -S "$root/samples/$sample" -B "$pair/$sample" \
      -DCMAKE_BUILD_TYPE=Release -DMadObs_DIR="$pair/relocated/lib/cmake/MadObs" \
      -DCMAKE_C_COMPILER="$consumer_cc" -DCMAKE_CXX_COMPILER="$consumer_cxx"
    "$cmake" --build "$pair/$sample" --parallel 4
    if [[ $sample == api_only ]]; then executable=madobs_api_consumer; else executable=consumer; fi
    cat "$pair/$sample/CMakeFiles/$executable.dir/link.txt"
    "$pair/$sample/$executable"
    ldd "$pair/$sample/$executable"
    echo "PASS $direction $sample linked and executed"
  done
  echo "PASS $direction all four consumers"
done
