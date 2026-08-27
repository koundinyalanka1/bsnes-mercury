#!/bin/bash
# Mirror YAGE scripts/build_libretro_cores.sh :: build_bsnes_mercury_ndk
# for this working tree. Output: target-libretro/out/<abi>/libbsnes_mercury_performance_libretro_android.so
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JNI_DIR="$ROOT/target-libretro/jni"
OUT_DIR="$ROOT/target-libretro/out"
API_LEVEL=24
PROFILE=performance
ABIS="${ABIS:-armeabi-v7a arm64-v8a x86_64}"

if [ -n "${ANDROID_NDK_HOME:-}" ]; then
  NDK="$ANDROID_NDK_HOME"
elif [ -n "${ANDROID_HOME:-}" ]; then
  NDK="$(ls -d "$ANDROID_HOME"/ndk/[0-9]* 2>/dev/null | sort -V | tail -1)"
else
  echo "ERROR: set ANDROID_NDK_HOME or ANDROID_HOME" >&2
  exit 1
fi

find_exec() {
  local base="$1" c
  for c in "$base" "$base.exe" "$base.cmd"; do
    if [ -f "$c" ]; then printf '%s\n' "$c"; return 0; fi
  done
  return 1
}

ndk_build="$(find_exec "$NDK/ndk-build" || true)"
if [ -z "$ndk_build" ]; then
  echo "ERROR: ndk-build not found in $NDK" >&2
  exit 1
fi

HOST_TAG=""
case "$(uname -s)" in
  Darwin*)
    if [ "$(uname -m)" = "arm64" ] && [ -d "$NDK/toolchains/llvm/prebuilt/darwin-arm64" ]; then
      HOST_TAG="darwin-arm64"
    else
      HOST_TAG="darwin-x86_64"
    fi
    ;;
  Linux*) HOST_TAG="linux-x86_64" ;;
  MINGW*|MSYS*|CYGWIN*) HOST_TAG="windows-x86_64" ;;
  *) HOST_TAG="windows-x86_64" ;;
esac
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
LLVM_STRIP="$(find_exec "$TOOLCHAIN/bin/llvm-strip" || true)"
LLVM_OBJDUMP="$(find_exec "$TOOLCHAIN/bin/llvm-objdump" || true)"
LLVM_NM="$(find_exec "$TOOLCHAIN/bin/llvm-nm" || true)"
READELF="$(find_exec "$TOOLCHAIN/bin/llvm-readelf" || true)"

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

# Same as YAGE: drop -fvisibility=* so link.T can export retro_*.
# -fno-stack-protector is required for libco; -O3 overrides ndk-build -Oz on arm32.
SIZE_CFLAGS="-ffunction-sections -fdata-sections"
bsnes_cflags="$SIZE_CFLAGS -fno-stack-protector -O3"
SIZE_LDFLAGS="-Wl,--gc-sections -Wl,--icf=safe -Wl,--exclude-libs,ALL"
PAGE_LDFLAGS="-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384"

assert_bsnes_thread_agnostic() {
  local so="$1" abi="$2"
  [ "$abi" = "arm64-v8a" ] || return 0
  if [ -z "$LLVM_OBJDUMP" ]; then
    echo "  ⚠ llvm-objdump not found; skipped thread-affinity assertion"
    return 0
  fi
  local disasm sfc_syms all_tls emu_tls lib_count
  disasm="$(mktemp)"
  "$LLVM_OBJDUMP" -d "$so" > "$disasm" 2>/dev/null || true
  sfc_syms="$(grep -c '<_ZN12SuperFamicom' "$disasm" || true)"
  if [ "$sfc_syms" -lt 1000 ]; then
    rm -f "$disasm"
    echo "  ✗ only $sfc_syms SuperFamicom symbols in $so"
    return 1
  fi
  all_tls="$(awk '/^[0-9a-f]+ </ { fn = $2 } /TPIDR_EL0/ { print fn }' "$disasm" | sort -u)"
  rm -f "$disasm"
  emu_tls="$(printf '%s\n' "$all_tls" | grep -E '12SuperFamicom|7GameBoy|4nall|co_switch|co_active|co_create' || true)"
  if [ -n "$emu_tls" ]; then
    echo "  ✗ emulator code reads the thread pointer:"
    printf '%s\n' "$emu_tls" | sed 's/^/      /'
    return 1
  fi
  lib_count="$(printf '%s\n' "$all_tls" | grep -c . || true)"
  echo "  ✓ no thread-pointer reads in emulator code ($lib_count in libc++)"
}

echo "NDK: $NDK"
echo "ndk-build: $ndk_build"
echo "jobs: $JOBS"
echo "ABIs: $ABIS"
echo "source: $ROOT"

mkdir -p "$OUT_DIR"
for abi in $ABIS; do
  echo ""
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  echo "  Building $abi"
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  rm -rf "$ROOT/target-libretro/obj" "$ROOT/target-libretro/libs"
  (cd "$JNI_DIR" && "$ndk_build" \
      PROFILE="$PROFILE" \
      APP_ABI="$abi" \
      APP_PLATFORM="android-$API_LEVEL" \
      APP_SHORT_COMMANDS=true \
      APP_CFLAGS="$bsnes_cflags" \
      APP_LDFLAGS="$SIZE_LDFLAGS $PAGE_LDFLAGS" \
      NDK_PROJECT_PATH=".." \
      NDK_APPLICATION_MK="Application.mk" \
      -j"$JOBS")

  local_unstripped="$ROOT/target-libretro/obj/local/$abi/libretro.so"
  local_built="$ROOT/target-libretro/libs/$abi/libretro.so"
  if [ ! -f "$local_built" ]; then
    echo "  ✗ no .so at $local_built"
    exit 1
  fi
  if [ -f "$local_unstripped" ]; then
    assert_bsnes_thread_agnostic "$local_unstripped" "$abi"
  fi

  mkdir -p "$OUT_DIR/$abi"
  dest="$OUT_DIR/$abi/libbsnes_mercury_performance_libretro_android.so"
  cp "$local_built" "$dest"
  if [ -n "$LLVM_STRIP" ]; then
    "$LLVM_STRIP" --strip-all --keep-section=.note.gnu.build-id "$dest"
    echo "  stripped $dest"
  fi
  if [ -n "$LLVM_NM" ]; then
    if ! "$LLVM_NM" -D "$dest" | grep -q ' retro_run$'; then
      echo "  ✗ retro_run missing from .dynsym"
      exit 1
    fi
    echo "  ✓ retro_run exported"
  fi
  ls -l "$dest"
done

echo ""
echo "Built:"
ls -l "$OUT_DIR"/*/libbsnes_mercury_performance_libretro_android.so
