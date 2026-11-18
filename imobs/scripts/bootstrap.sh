#!/usr/bin/env bash
# imobs 一键构建验证：Release 全测 + ASan/UBSan 全测 + TSan 并发测 + 热路径 bench（带门禁）。
# 用法：./scripts/bootstrap.sh
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== [1/4] RelWithDebInfo: build + ctest =="
cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build build/rel -j >/dev/null
ctest --test-dir build/rel --output-on-failure

echo "== [2/4] ASan+UBSan: build + ctest =="
cmake -S . -B build/asan -DIMOBS_SAN=asan >/dev/null
cmake --build build/asan -j >/dev/null
ctest --test-dir build/asan --output-on-failure

echo "== [3/4] TSan: build + 并发相关用例 =="
cmake -S . -B build/tsan -DIMOBS_SAN=tsan >/dev/null
cmake --build build/tsan -j >/dev/null
ctest --test-dir build/tsan --output-on-failure -R "event_ring|metrics|clock|kernel|ffi|e2e|thin_server"

echo "== [4/4] 热路径 bench（IMOBS_BENCH_GATE=1：4 线程 P99 >= 1us 即失败）=="
IMOBS_BENCH_GATE=1 ./build/rel/bench_event_ring
echo "== bootstrap 全绿 =="
