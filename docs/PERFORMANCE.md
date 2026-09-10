# Renderer optimization for YAGE — 2026-09-10

The supplied TV logs show a CPU-bound SNES session: 48.2–51.7 emulated
frames/second, 18.3–19.8 ms inside the core, and a 60.10 Hz target (16.64 ms).
The last SNES session in `full_logcat_tv.txt` agrees with `tv_logs.txt`.
The worker is enabled on the four-core TV. Reported audio underruns, overflows,
and blit failures are zero; those counters alone do not establish audio quality.

The changes reduce actual rendering work and synchronization overhead. They do
not add frameskip, change CPU clocks, simplify DSP emulation, or change inputs.

## Changes

- Grow the scanline queue from 3 to 16 entries and consume published work in
  batches. Release completed queue entries and their VRAM references together
  under one lock. Wake the worker on an empty-to-nonempty transition; wake the
  producer when a full queue gains space or the queue drains. Work remains
  bounded within the current frame, and frame completion still drains it.
- Track VRAM changes in a 512-byte bitmap with one bit per 16-byte tile block.
  Each immutable VRAM snapshot carries its changes. Invalidate only affected
  2/4/8-bpp decoded tiles when consuming consecutive generations. Restore,
  reset, worker restart, and discontinuous generations retain full invalidation.
- Read window registers directly from the immutable scanline snapshot in the
  worker. The old implementation first read the concurrently mutable live
  registers and then overwrote those values with snapshot values.

The additional queue entries and dirty maps cost approximately 27 KiB. The
three existing 64 KiB VRAM snapshots and the single active decoded-tile cache
remain. The optimization is confined to the performance PPU; libretro's public
API, DSP, CPU, input, memory mappings, and serialized state format are unchanged.

## Measured results

Local macOS ARM64, Clang `-O3`, baseline commit `41893ca`, full rendering,
frameskip zero. Each result is the median of three 300-frame synthetic runs;
baseline/optimized ordering alternates. Other build and sanitizer jobs had
finished before these measurements. This benchmark measures rendering,
synchronization, and framebuffer hashing, not whole-machine emulation or TV FPS.

| PPU mode | Baseline ms/frame | Optimized ms/frame | Reduction |
| --- | ---: | ---: | ---: |
| 0 | 1.4461 | 1.1590 | 19.9% |
| 1 | 0.9447 | 0.8056 | 14.7% |
| 2 | 0.8473 | 0.7133 | 15.8% |
| 3 | 0.8463 | 0.5620 | 33.6% |
| 4 | 0.8976 | 0.7248 | 19.3% |
| 5 | 1.3017 | 1.1232 | 13.7% |
| 6 | 0.9157 | 0.8048 | 12.1% |
| 7 | 0.5261 | 0.4349 | 17.3% |

[Raw runs](ppu-benchmark-results.json) include hashes and timings for both inline
and threaded rendering. Scheduling affects the absolute timings; these results
must not be read as a prediction of the same percentage improvement on the TV.

## Validation

- Every framebuffer hash matches baseline across all eight PPU modes, for both
  inline and threaded rendering.
- Additional 120-frame stress runs match baseline and inline output with
  per-scanline palette, window, scroll and forced-blank VRAM writes, repeated
  snapshot-slot reuse, mosaic, overscan, hires, interlace, and Mode 7 EXTBG.
- ThreadSanitizer passes the 20-frame-per-mode stress suite without a race report.
- A generated SNES ROM exercises the real libretro lifecycle, controller polling,
  WRAM, SRAM mapping, save/restore, and recreation of the calling OS thread.
  Baseline, optimized threaded, and optimized inline runs produce identical
  video/audio hashes, WRAM hashes, and memory-map descriptors. Each measured
  continuation delivers 60 video frames and 31,987 stereo audio frames.
  Audio in this minimal ROM is silent; this verifies transport and timing,
  not game-specific sound quality.
- Android ARMv7, ARM64, and x86-64 builds succeed. All 25 existing public exports
  match the supplied previous artifacts; all load segments have 16 KiB alignment.
  The unstripped ARM64 disassembly contains 714 defined SuperFamicom symbols and
  no emulator thread-pointer reads, preserving YAGE's coroutine migration check.
  [Build sizes, checksums, and validation](android-build-info.json).

RetroAchievements mapping checks cover WRAM and SRAM on the generated cartridge;
no achievement service session or special-chip commercial cartridge was tested.
No TV run or listening test was performed. Sustained 60 FPS on low-end devices
remains a hardware measurement, especially for coprocessor-heavy games.

## Build and use

```sh
scripts/build_android_yage.sh /path/to/android-ndk
```

Tested with NDK `28.2.13676358`, Android API 24. The output is:

```text
target-libretro/out/optimized/armeabi-v7a/libbsnes_mercury_performance_libretro_android.so
target-libretro/out/optimized/arm64-v8a/libbsnes_mercury_performance_libretro_android.so
target-libretro/out/optimized/x86_64/libbsnes_mercury_performance_libretro_android.so
```

Use the corresponding ABI file in YAGE's `android/app/src/main/jniLibs/<abi>/`.
YAGE itself and its source-build manifest were not modified. Its source build
script must use this revised source to reproduce these libraries; rebuilding
from the old manifest revision will replace them with the previous implementation.
The Android build retains `-O3`, `-fno-stack-protector`, and 16 KiB page alignment.

For fidelity and throughput comparisons, select `bsnes_speed_profile=full` and
`bsnes_frameskip=0` so the pre-existing automatic speed ladder cannot change
rendering cadence or DSP quality while measuring. Existing option behavior is
unchanged: the old automatic ladder can still select simplified DSP under load,
and its frameskip value 1 does not omit frames. These are separate from the
rendering optimization implemented here.

## Reproduce local checks

Build the performance core and matching test harness together. The root Makefile
does not track every header or compiler-flag change, so use `-B` after changing
headers, profiles, or sanitizer flags. On macOS ARM64:

```sh
make -B -j8 platform=osx arch=arm CC=clang CXX=clang++ \
  TARGET=/tmp/bsnes-test.dylib \
  NEW_GCC_FLAGS='-DHAVE_POSIX_MEMALIGN=1 -fno-stack-protector'
clang++ -O3 -std=c++11 -DPROFILE_PERFORMANCE -D__LIBRETRO__ \
  -DHAVE_POSIX_MEMALIGN=1 -I. -Ilibco tests/ppu_benchmark.cpp \
  /tmp/bsnes-test.dylib -o /tmp/ppu-test
/tmp/ppu-test 120
/tmp/ppu-test 120 stress
python3 tests/libretro_smoke.py /tmp/bsnes-test.dylib enabled
python3 tests/libretro_smoke.py /tmp/bsnes-test.dylib disabled
```

For baseline comparisons, compile the same harness against the baseline headers
and baseline library in a separate source directory. Its private PPU layout must
match the library. With both executables built:

```sh
python3 tests/compare_ppu_benchmarks.py /tmp/ppu-baseline /tmp/ppu-test
```

For the race check, rebuild the core with `-fsanitize=thread` in
`NEW_GCC_FLAGS` and `LDFLAGS='-fPIC -dynamiclib -fsanitize=thread'`, link the harness
with `-fsanitize=thread`, and run its stress mode with
`TSAN_OPTIONS=halt_on_error=1`. Rebuild without the sanitizer afterward.
