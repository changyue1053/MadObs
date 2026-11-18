#!/usr/bin/env bash
# Offline packaging check. Leaves isolated temporary outputs for inspection.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cmake="${CMAKE:-cmake}"
work="$(mktemp -d "${TMPDIR:-/tmp}/madobs-package-XXXXXXXX")"
printf 'Package verification workspace: %s\n' "$work"
"$cmake" -S "$root" -B "$work/api-build" -DBUILD_TESTING=OFF -DMADOBS_BUILD_RUNTIME=OFF
"$cmake" --install "$work/api-build" --prefix "$work/api"
"$cmake" -S "$root/samples/api_only" -B "$work/api-consumer" -DCMAKE_BUILD_TYPE=Release -DMadObs_DIR="$work/api/lib/cmake/MadObs"
"$cmake" --build "$work/api-consumer" --parallel 4
"$work/api-consumer/madobs_api_consumer"
"$cmake" -S "$root" -B "$work/native-build" -DBUILD_TESTING=OFF -DMADOBS_BUILD_SHARED=OFF -DCMAKE_BUILD_TYPE=Release
"$cmake" --build "$work/native-build" --parallel 4
"$cmake" --install "$work/native-build" --prefix "$work/native"
cp -a "$work/native" "$work/relocated"
for kind in private_c private_cpp runtime_c; do
  "$cmake" -S "$root/samples/$kind" -B "$work/$kind" -DCMAKE_BUILD_TYPE=Release -DMadObs_DIR="$work/relocated/lib/cmake/MadObs"
  "$cmake" --build "$work/$kind" --parallel 4
  "$work/$kind/consumer"
done
mkdir "$work/wrong-version"
cat > "$work/wrong-version/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(wrong_version LANGUAGES C)
find_package(MadObs 9.9.9 EXACT REQUIRED)
CMAKE
if "$cmake" -S "$work/wrong-version" -B "$work/wrong-build" -DMadObs_DIR="$work/relocated/lib/cmake/MadObs" >"$work/wrong-version.log" 2>&1; then
  echo 'ERROR: incompatible exact package version was accepted' >&2
  exit 1
fi
grep -q '9.9.9' "$work/wrong-version.log"
echo 'PASS: API-only, relocated native C/C++11 consumers and wrong-version rejection'
