#!/usr/bin/env bash
# Explicit separate opt-in; never invoked by the build-only script.
set -Eeuo pipefail
cd "$(dirname "$0")/.."
# Only fixed labels reach the caller; all tool output remains local.
exec 3>&1
mkdir -p build/ios-validation-logs
exec >>build/ios-validation-logs/simulator.log 2>&1
stage=sim-prerequisites
trap 'printf "FAIL: %s\n" "$stage" >&3' ERR
passed() { printf 'PASS: %s\n' "$stage" >&3; }
[[ ${MADOBS_RUN_SIMULATOR:-0} == 1 ]] || { echo 'Set MADOBS_RUN_SIMULATOR=1 only for an authorized local simulator run' >&2; exit 1; }
[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]]
export DEVELOPER_DIR=/Applications/Xcode_26.2.app/Contents/Developer
[[ $(xcodebuild -version) == $'Xcode 26.2\nBuild version 17C52' ]]
out="$PWD/build/ios-validation"
test -f "$out/simulator/stage/lib/libmadobs_runtime.a"
[[ ! -e "$out/simulator/result.xcresult" ]]
runtime=com.apple.CoreSimulator.SimRuntime.iOS-26-2
xcrun simctl list runtimes available -j | python3 -c 'import json,sys; assert any(x["identifier"]=="com.apple.CoreSimulator.SimRuntime.iOS-26-2" and x["isAvailable"] for x in json.load(sys.stdin)["runtimes"])'
passed
stage=sim-create
uuid=$(xcrun simctl create "MadObs-local-validation-$$" com.apple.CoreSimulator.SimDeviceType.iPhone-16 "$runtime")
[[ $uuid =~ ^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$ ]]
passed
cleanup() { xcrun simctl shutdown "$uuid" >/dev/null 2>&1 || true; xcrun simctl delete "$uuid" >/dev/null 2>&1 || true; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
stage=sim-boot
xcrun simctl boot "$uuid"
xcrun simctl bootstatus "$uuid" -b
passed
stage=sim-arm64
[[ $(xcrun simctl spawn "$uuid" uname -m) == arm64 ]]
passed
stage=xctest-execute
env -u SDKROOT xcodebuild -project ios-validation/Consumer/MadObsValidation.xcodeproj \
  -scheme MadObsValidation -destination "platform=iOS Simulator,id=$uuid" \
  -destination-timeout 120 -derivedDataPath "$out/simulator/consumer" \
  -resultBundlePath "$out/simulator/result.xcresult" -parallel-testing-enabled NO \
  MADOBS_STAGE="$out/simulator/stage" ARCHS=arm64 ONLY_ACTIVE_ARCH=YES \
  CODE_SIGNING_ALLOWED=YES CODE_SIGN_IDENTITY=- CODE_SIGNING_REQUIRED=NO test

passed
stage=named-result-parse
# Exit status alone is insufficient: require both exact XCTest cases, no failures/skips.
python3 scripts/check-ios-validation-results.py >&3 2>&3
passed
