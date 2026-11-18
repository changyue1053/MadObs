#!/usr/bin/env bash
# Configure/build native static C ABI only. Not an AAR/HAR/XCFramework packager.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
platform="${1:-}"
cmake="${CMAKE:-cmake}"
args=(-DBUILD_TESTING=OFF -DMADOBS_BUILD_SHARED=OFF -DCMAKE_BUILD_TYPE=Release)
case "$platform" in
 android)
  : "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to the installed official NDK}"
  tc="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
  args+=("-DCMAKE_TOOLCHAIN_FILE=$tc" "-DANDROID_ABI=${ANDROID_ABI:-arm64-v8a}" "-DANDROID_PLATFORM=${ANDROID_PLATFORM:-android-24}") ;;
 ios)
  command -v xcrun >/dev/null || { echo 'iOS build requires macOS and Xcode (xcrun)' >&2; exit 2; }
  xcrun --sdk iphoneos --show-sdk-path >/dev/null
  args+=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 "-DCMAKE_OSX_DEPLOYMENT_TARGET=${IOS_DEPLOYMENT_TARGET:-13.0}") ;;
 ohos)
  : "${OHOS_NATIVE_HOME:?Set OHOS_NATIVE_HOME to the installed official native SDK}"
  tc="$OHOS_NATIVE_HOME/build/cmake/ohos.toolchain.cmake"
  args+=("-DCMAKE_TOOLCHAIN_FILE=$tc" "-DOHOS_ARCH=${OHOS_ARCH:-arm64-v8a}") ;;
 *) echo 'Usage: build-platform.sh android|ios|ohos' >&2; exit 2 ;;
esac
if [[ -n "${tc:-}" && ! -f "$tc" ]]; then echo "Missing SDK toolchain: $tc" >&2; exit 2; fi
build="${MADOBS_BUILD_DIR:-$root/build-$platform}"
"$cmake" -S "$root" -B "$build" "${args[@]}"
"$cmake" --build "$build" --parallel "${JOBS:-4}"
"$cmake" --install "$build" --prefix "${MADOBS_INSTALL_DIR:-$build/install}"
