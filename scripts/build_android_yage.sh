#!/usr/bin/env bash
# Build drop-in YAGE libraries without modifying the frontend or its core manifest.
set -euo pipefail
repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
ndk_dir="${1:-${ANDROID_NDK_HOME:-}}"
if [[ ! -x "$ndk_dir/ndk-build" ]]; then
  echo "Usage: $0 /path/to/android-ndk (or set ANDROID_NDK_HOME)" >&2
  exit 1
fi
"$ndk_dir/ndk-build" -C "$repo_dir/target-libretro/jni" \
  PROFILE=performance APP_ABI="armeabi-v7a arm64-v8a x86_64" \
  APP_PLATFORM=android-24 NDK_PROJECT_PATH=.. NDK_APPLICATION_MK=Application.mk \
  NDK_OUT=../out/obj-optimized NDK_LIBS_OUT=../out/libs-optimized \
  APP_CFLAGS="-O3 -fno-stack-protector" \
  APP_LDFLAGS="-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384" \
  -j"${JOBS:-4}"
for abi in armeabi-v7a arm64-v8a x86_64; do
  dest="$repo_dir/target-libretro/out/optimized/$abi"
  mkdir -p "$dest"
  cp "$repo_dir/target-libretro/out/libs-optimized/$abi/libretro.so" \
    "$dest/libbsnes_mercury_performance_libretro_android.so"
done
