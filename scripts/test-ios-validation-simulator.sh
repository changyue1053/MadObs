#!/usr/bin/env bash
# Explicit separate opt-in; never invoked by the build-only script.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ ${MADOBS_RUN_SIMULATOR:-0} == 1 ]] || { echo 'Set MADOBS_RUN_SIMULATOR=1 only for an authorized local simulator run' >&2; exit 1; }
[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]]
export DEVELOPER_DIR=/Applications/Xcode_26.2.app/Contents/Developer
[[ $(xcodebuild -version) == $'Xcode 26.2\nBuild version 17C52' ]]
out="$PWD/build/ios-validation"
test -f "$out/simulator/stage/lib/libmadobs_runtime.a"
[[ ! -e "$out/simulator/result.xcresult" ]]
runtime=com.apple.CoreSimulator.SimRuntime.iOS-26-2
xcrun simctl list runtimes available -j | python3 -c 'import json,sys; assert any(x["identifier"]=="com.apple.CoreSimulator.SimRuntime.iOS-26-2" and x["isAvailable"] for x in json.load(sys.stdin)["runtimes"])'
uuid=$(xcrun simctl create "MadObs-local-validation-$$" com.apple.CoreSimulator.SimDeviceType.iPhone-16 "$runtime")
cleanup() { xcrun simctl shutdown "$uuid" >/dev/null 2>&1 || true; xcrun simctl delete "$uuid" >/dev/null 2>&1 || true; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
xcrun simctl boot "$uuid"
xcrun simctl bootstatus "$uuid" -b
[[ $(xcrun simctl spawn "$uuid" uname -m) == arm64 ]]
env -u SDKROOT xcodebuild -project ios-validation/Consumer/MadObsValidation.xcodeproj \
  -scheme MadObsValidation -destination "platform=iOS Simulator,id=$uuid" \
  -destination-timeout 120 -derivedDataPath "$out/simulator/consumer" \
  -resultBundlePath "$out/simulator/result.xcresult" -parallel-testing-enabled NO \
  MADOBS_STAGE="$out/simulator/stage" ARCHS=arm64 ONLY_ACTIVE_ARCH=YES \
  CODE_SIGNING_ALLOWED=YES CODE_SIGN_IDENTITY=- CODE_SIGNING_REQUIRED=NO test

# Exit status alone is insufficient: require both exact XCTest cases, no failures/skips.
python3 scripts/check-ios-validation-results.py
