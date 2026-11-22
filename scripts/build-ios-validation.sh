#!/usr/bin/env bash
# Build validation only. No distribution, upload, tool download or license acceptance.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]] || { echo 'Requires Apple silicon macOS' >&2; exit 1; }
export DEVELOPER_DIR=/Applications/Xcode_26.2.app/Contents/Developer
[[ $(xcodebuild -version) == $'Xcode 26.2\nBuild version 17C52' ]]
command -v cmake >/dev/null
out="$PWD/build/ios-validation"
[[ ! -e "$out" ]] || { echo 'Requires a fresh build/ios-validation directory; no automatic deletion' >&2; exit 1; }
for variant in device simulator; do
  if [[ $variant == device ]]; then sdk=iphoneos; destination='generic/platform=iOS'; else sdk=iphonesimulator; destination='generic/platform=iOS Simulator'; fi
  sdkroot=$(xcrun --sdk "$sdk" --show-sdk-path)
  [[ $(xcrun --sdk "$sdk" --show-sdk-version) == 26.2 ]]
  test -f "$sdkroot/usr/include/zlib.h"
  test -f "$sdkroot/usr/lib/libz.tbd"
  # Keep the complete public runtime source list, including unused server/crash objects.
  # Target SDK zlib is explicit: no host/Homebrew library may satisfy this dependency.
  cmake -S . -B "$out/$variant/native" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$sdkroot" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
    -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO \
    -DCMAKE_INSTALL_PREFIX="$out/$variant/stage" \
    -DBUILD_TESTING=OFF -DMADOBS_BUILD_RUNTIME=ON -DMADOBS_BUILD_SHARED=OFF \
    -DMADOBS_BUILD_JNI=OFF -DMADOBS_PACKAGE_BORINGSSL=OFF -DIMOBS_SAN= \
    -DIMOBS_BORINGSSL_ROOT= -DIMOBS_BORINGSSL_INCLUDE= -DIMOBS_BORINGSSL_CRYPTO_LIB= \
    -DZLIB_INCLUDE_DIR="$sdkroot/usr/include" -DZLIB_LIBRARY="$sdkroot/usr/lib/libz.tbd"
  cmake --build "$out/$variant/native" --config Release --target imobs_core --parallel 3
  # Stage this repository's compiled archive/headers inside its ignored build tree.
  # This is not installing a tool or publishing a package.
  cmake --install "$out/$variant/native" --config Release
  archive="$out/$variant/stage/lib/libmadobs_runtime.a"
  [[ $(xcrun lipo -archs "$archive") == arm64 ]]
  env -u SDKROOT xcodebuild -project ios-validation/Consumer/MadObsValidation.xcodeproj \
    -scheme MadObsValidation -destination "$destination" \
    -derivedDataPath "$out/$variant/consumer" \
    MADOBS_STAGE="$out/$variant/stage" ARCHS=arm64 ONLY_ACTIVE_ARCH=NO \
    CODE_SIGNING_ALLOWED=NO build-for-testing
 done
printf '%s\n' 'Build-only complete if all commands succeeded; no app, simulator or device was run.'
