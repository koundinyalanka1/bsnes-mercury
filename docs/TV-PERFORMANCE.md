# Android TV investigation — 2026-09-12

Follow-up to [PERFORMANCE.md](PERFORMANCE.md), driven by two logs from a Sony
BRAVIA BF1 (`tv_logs.txt`, `full_logcat_tv.txt`). Three SNES sessions appear in
them, all on `bsnes-mercury v094 (Performance) d68bcc1`:

| Game | reported fps | `run` ms | budget |
| --- | ---: | ---: | ---: |
| Super Bomberman 5 | 55.7 – 59.3 | 14.8 – 16.4 | 16.64 |
| Super Mario Kart | 56.8 – 59.6 | 15.5 – 16.4 | 16.64 |
| Zelda: A Link to the Past | 60.1 | 14.5 – 15.2 | 16.64 |

One 30-second window reports `underrun=352`. The frontend ran with
`bsnes_speed_profile=fast` and `bsnes_ppu_thread=enabled`.

## What the device is

Four signals in the logcat agree that the TV has a **32-bit-only ARM userspace**:
only `zygote` appears and never `zygote64`; the only WebView relro process is
`WebViewLoader-armeabi-v7a`; `dex2oat` runs with `isa=arm`; and the strings
`arm64`/`aarch64` appear zero times in 99,539 lines. YAGE ships arm64-v8a cores,
but this device cannot load them. armeabi-v7a is forced, not a packaging mistake.

That matters because 32-bit ARM is the expensive target for this core: 14 usable
general-purpose registers instead of 31, `Thread::clock` arithmetic in register
pairs with `umull`, and PIC globals reached through a double GOT indirection.

## What is not the problem

Two popular suspects were measured and cleared.

- **libco cothreads.** Instrumenting `co_switch` shows **526–548 switches per
  frame** — about two per scanline, from `CPU::scanline()`. The earlier
  `smp_pending_clocks` batching already collapsed this. At any plausible switch
  cost it is under 0.05 ms/frame.
- **The bus lookup tables.** `Bus::lookup` (16 MB) and `Bus::target` (64 MB) are
  only reached when the inline WRAM and `fast_read` paths miss. That happens
  **726–3,606 times per frame**, i.e. MMIO and DMA only.

Where the emulation thread actually goes, from a leaf profile of a single-threaded
gameplay run (Zelda, attract demo), after removing the harness's own cost:

| | share of emulation thread |
| --- | ---: |
| `CPU::add_clocks` bookkeeping | ~29% |
| SPC700 + SPC_DSP | ~32% |
| 65816 opcode bodies | ~22% |
| `Sprite::evaluate` | ~6% |
| palette blit in `videoRefresh` | ~4% |

There is no single hotspot. The cost is broad-spectrum interpreter work, which is
the honest reason this is hard: bsnes-mercury does more work per emulated cycle
than a catch-up-scheduler emulator does, and the performance profile only
simplifies the PPU, not the scheduling core.

## Defects found

**`bsnes_frameskip` never took effect.** `retro_load_game()` calls
`apply_speed_step()`, which called `set_frameskip()` with the ladder's value,
overwriting the user option that `update_variables()` had applied during
`retro_init()`. The option was dead for the whole session. The frameskip value is
now owned by `apply_speed_step()`, so it survives every game load.

**`bsnes_dsp_fast` was silently discarded by save states.** `SPC_DSP::init()`
clears `fast_mode`, and `DSP::power()` calls it on every load *and* every
`retro_unserialize()`. So the speed ladder's DSP setting applied after a cold load
but was dropped the moment a state was restored — audio quality depended on
whether you had loaded a savestate. `DSP` now remembers the request and re-applies
it after `init()`.

**A light gun read far outside the palette, and crashed.** `Video::draw_cursor()`
stored `palette[pixelcolor]` -- an already-converted 24-bit RGB value -- into
`ppu.output`, which holds palette *indices*. `videoRefresh()` then fed that value
back through `palette[]`, indexing a 1<<19 entry table with a number up to
16,777,215. With a Justifier connected this reads roughly 67 MB past the array and
takes SIGBUS; with a Super Scope it silently returns garbage pixels. Present in
the unmodified baseline and unrelated to anything else here. The cursor now stores
the index, which is what the rest of the pipeline expects.

**The speed ladder traded audio for almost nothing.** `SPC_DSP::fast_mode`
replaces gaussian interpolation with linear *and disables the echo unit entirely*
— `t_echo_in` is forced to zero, every FIR tap returns early, and `echo_output()`
omits the echo term. Measured cost of keeping full quality, on gameplay, with the
persistence bug fixed: **−0.5%, +1.9%, +1.0%** for the three games. Measured
effect on the output: Zelda's audio RMS falls from 1846.7 to 1725.0 (−6.6%) with
fast mode on, consistent with losing a reverb unit its soundtrack leans on.

The old ladder's step 1 set frameskip 1, which renders *every* frame and so
recovered nothing, while switching echo off. It spent the entire SNES reverb unit
for roughly 1%.

## Changes

Every performance change below removes work unconditionally. None of them
branch on a title, a heuristic, or a measured threshold, and none introduce a
tuned constant; `profile=full` output stays bit-identical to baseline.

Emulation thread, all on the per-memory-access path:

- `CPU::step()` no longer does two 64-bit multiplies and two 64-bit subtractions
  to age the controller threads. A passive controller's thread only burns clock,
  so the debt is accumulated and settled at the SMP's existing synchronization
  points. Super Scope, Justifier and USART override `enter()` and keep the exact
  per-access path; `Input::connect()` marks which case applies and settles any
  outstanding debt across a port swap.
- The SMP debt and the controller debt share one counter, since both settle at
  the same points, removing a load/add/store.
- `CPU::poll_irq()` is inline and call-free instead of `noinline`. Its comparison
  values derive only from `$4207`-`$420a` and the region, so they are computed on
  write rather than rebuilt per access, and the range test collapses to one
  unsigned comparison (see the proof in `timing.cpp`).
- `add_clocks()` had every cold path inlined into it -- the scanline wrap in
  `PPUcounter::tick()`, the event-queue drain, the coprocessor loop, the live
  controller path -- which forced `push {r4-r10,lr}` and a matching pop on a
  function called ~45,000 times per frame. Moving them out of line took the ARM32
  body from 231 instructions to 60.
- `nall::priority_queue::tick()` compares against a cached copy of
  `heap[0].counter` rather than reloading the heap size and chasing the heap
  pointer. A stale cache can only read low, which costs a redundant drain check
  and never misses an event.

Render worker:

- `vram_slot_ref` and `job_count` are atomics. The producer previously took the
  queue mutex once per scanline purely to increment a refcount, and again to test
  for a free slot.
- The job ring holds 256 entries rather than 16. The producer outruns the worker
  through active display and idles through vblank, and at 16 it blocked on that
  burst. The depth is not tuned: `retro_run()` drains at every frame boundary and
  a frame publishes at most 239 scanlines, so 256 cannot fill for any game on
  either region.
- Batching scanlines before handing them over was tried and **reverted**: it
  starves the worker and stalls on the three VRAM snapshot slots. Measured +34%
  on Zelda at a batch of 8. Each scanline is published as it lands.

Frontend interface:

- `RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE` is now implemented. The TV log warns
  `frameskip engaged but the core has never read GET_AUDIO_VIDEO_ENABLE`; YAGE's
  adaptive frame loop was asking to shed individual frames and the core could not
  answer. A dropped frame skips the pixel work and reports a duplicate to
  `video_refresh`, but still runs background mosaic counters and sprite
  evaluation, because the sprite overflow flags at `$213e` are CPU-visible. Audio
  hashes are byte-identical with and without dropping.
- New option `bsnes_dsp_fast` (`auto|disabled|enabled`) pins DSP quality
  independently of the speed ladder.

Speed ladder, now video-only:

| step | before | after |
| --- | --- | --- |
| 1 (`balanced`) | frameskip 1 (no-op) + echo off | frameskip 2 |
| 2 (`fast`) | frameskip 2 + echo off | frameskip 3 |

**This changes what the named profiles mean.** `fast` renders 1 frame in 3 rather
than 1 in 2, and no profile alters audio any more.

## Measured results

Local macOS ARM64, Clang `-O3`, baseline commit `d68bcc1`. Real gameplay, entered
by running each ROM into its attract demo and saving a state there — the previous
harness measured static title screens, which understate the work by about 35%.
Each figure is the median of 4 interleaved A/B runs of 3,500 frames. The column is
emulation-thread ms/frame with the render worker enabled, which is what the TV's
`run=` reports.

| Game | baseline `fast` | final `balanced` | final `full` |
| --- | ---: | ---: | ---: |
| Zelda | 0.4453 | 0.4447 (−0%) | 0.4991 (+12%) |
| Super Mario Kart | 0.5427 | 0.5055 (−7%) | 0.5826 (+7%) |
| Super Bomberman 5 | 0.5038 | 0.4770 (−5%) | 0.5272 (+5%) |

These should be worth at least as much on armeabi-v7a: the removed 64-bit
multiply-and-subtract pairs cost several instructions there rather than one, and
the prologue traffic removed from `add_clocks()` is 16 memory operations per call
on ARM32 against 8 on ARM64. That is reasoning from the generated code, not a
measurement on the device.

Two build-level levers were tested and rejected. `-flto` is **2–6% slower**
despite a smaller binary. `-fvisibility=hidden` removes only 8% of the GOT loads
in `sfc/alt/cpu/cpu.o`, because the hot globals are defined in other translation
units.

## What this does and does not achieve

Scaling the table above onto the logged TV figures:

- At `balanced` (render 1 frame in 2, **full audio including echo**), the three
  games project to **13.6–14.9 ms**. That restores the reverb unit the TV is
  currently running without, and should clear the underruns with real headroom.
- At `full` (render every frame), they project to **15.0–16.2 ms**, against
  roughly 17.0–17.6 ms before this work. Bomberman 5 comes in under the effective
  budget; Zelda and Mario Kart sit right on it.

The budget is also tighter than 16.64 ms. The log pairs `fps=56.9` with
`run=16.4ms`: 1/56.9 is 17.57 ms, so about **1.2 ms per frame is spent outside
`retro_run`** — the GLES blit, audio queueing and sleep granularity. The core has
to come in under roughly 15.4 ms, not 16.64 ms, which leaves `balanced` with very
little margin and puts `full` further out of reach than the raw numbers suggest.

Frameskip-free 60 fps has gone from clearly out of reach to borderline. The
remaining lever is the same one that produced most of the gain above: the worker
still idles 28-56% of the frame while the emulation thread is the critical path,
so work that can be moved across that boundary is worth more than anything left
in the per-access path. `Sprite::evaluate()` (5-6% of the emulation thread) and
the palette blit in `videoRefresh()` (3-4%) are both candidates, and both need
care -- sprite evaluation writes the overflow flags the CPU reads at `$213e`.

The frontend can also now shed frames adaptively through the implemented
`GET_AUDIO_VIDEO_ENABLE` rather than committing to a fixed frameskip, rendering
whenever there is headroom and dropping only frames that overrun.

No TV run and no listening test was performed for any of this. The projections
above are arithmetic on a 20–30× faster machine and need a device measurement
before they are treated as results.

## Holistic audit

A separate pass over the whole core, not just the changed paths. Tooling used:
UBSan, ASan, TSan, a save-state fuzz harness, a load/unload lifecycle probe, and
a measurement of emitted audio rate against the declared one.

### SPC700 ran fast on two opcodes, and the audio rate was wrong because of it

`SMP::power()` built `cycle_table_cpu` as `(cycle_count_table[n] * 24) * cpu.frequency`.
The destination is `uint64`, but the product was formed entirely in 32-bit and
wrapped for any opcode of 9 cycles or more. Exactly two qualify, and they are not
obscure:

| opcode | | cycles | clock advance | effective |
| --- | --- | ---: | ---: | ---: |
| `0xCF` | `MUL YA` | 9 | 344,123,456 instead of 4,639,090,752 | **0.67 cycles** |
| `0x9E` | `DIV YA,X` | 12 | 1,890,487,040 instead of 6,185,454,336 | 3.67 cycles |

`MUL` and `DIV` are staples of SPC music drivers (volume scaling, pitch, envelopes),
so the APU ran ahead of the CPU by an amount that varied with how much the driver
multiplied. Measured audio emitted per video frame, against the 533.13 that the
declared 32040.5 Hz implies:

| | before | after |
| --- | ---: | ---: |
| Zelda | 556.59 (+4.4%) | 533.09 |
| Super Mario Kart | 551.45 (+3.4%) | 533.09 |
| Super Bomberman 5 | 537.88 (+0.9%) | 533.09 |

Every game and both regions now land within **0.01%** of the declared rate. This is
the defect behind the frontend's `reported 32040 Hz (actual 31955 Hz)` line: a core
whose output rate does not match what it advertises forces the frontend to resample
or drop continuously, and music played slightly fast. Forming the product in 64-bit
is the whole fix, and it is performance-neutral to slightly positive.

**This one intentionally changes output.** Video is unchanged for two of the three
games and differs for Mario Kart, because APU timing feeds the CPU/APU port
handshake. It is a correctness fix, not a regression, and it is separable from the
performance work, which stays bit-identical to baseline.

### Malformed save states read out of bounds

`nall::serializer` did no bounds checking on Load: `_data[_size++]` never tested
`_capacity`. `System::unserialize()` reads a 600-byte header *before* validating the
signature, so `retro_unserialize(ptr, 0)` overflowed, and a state with a valid header
but a truncated body overflowed by up to the full ~325 KB state. A partially written
state file -- power loss mid-save -- is enough to reach this. `strcmp` on the stored
profile name also ran off the end when that field was not NUL-terminated.

Load transfers are now bounds-checked and set an `invalid()` flag, `unserialize()`
rejects before trusting the header and again after the body, the profile name is
terminated before comparison, and `retro_unserialize()` rejects anything shorter than
`serialize_size()`. Verified with a fuzz harness under ASan across empty, short,
truncated and corrupt inputs; all are now refused cleanly.

### Clean

- **TSan**: no races across gameplay, the PPU stress suite, and save/restore with the
  worker live.
- **ASan**: clean on gameplay and every controller path. Worth noting that ASan did
  *not* catch the light-gun cursor overflow -- that read lands ~67 MB past a heap
  array, far outside any redzone, so "ASan clean" is not proof for large strides. The
  palette invariant was checked by hand instead: `Screen::render` writes at most
  `brightness<<15 | 0xffff` = `0x7ffff`, exactly the last index of the `1<<19` table,
  and `draw_cursor` was the only writer violating it.
- **UBSan**: only left-shifts of negative values, in blargg's BRR decoder and the
  DSP-1 coefficient math. Technically undefined, defined in practice on every
  compiler that builds this, and unchanged by this work.
- **Lifecycle**: 25 load/unload cycles with controller reconnects hold RSS flat at
  ~93 MB. `co_delete` frees the cothread stacks; no leak.
- **Clock arithmetic**: every other `clock ± clocks * frequency` site in the core
  already casts to `uint64`. `cycle_step_cpu` was the one remaining unguarded
  product; it fits today and is only used on the uncompiled CYCLE_ACCURATE path, and
  is now formed in 64-bit anyway.

### Open, not addressed

**80 MB of the core's 94 MB resident set is two lookup tables.** `Bus::lookup`
(16 MB) and `Bus::target` (64 MB) are read from exactly two places, both guarded by
a null `fast_read`/`fast_write` pointer, so every entry covering a fast-mapped page
is dead weight. The slow path they serve is taken only 726-3,606 times per frame.
Shrinking this is a bus redesign -- byte granularity is genuinely needed inside
`$2000-$5fff`, so a page table alone does not suffice -- with a modest speed payoff
and real regression risk, so it is flagged rather than attempted.

## Verification

Coverage is deliberately not limited to the three logged games, since a fix that
only holds for them is not a fix.

- `bsnes_speed_profile=full` is bit-identical to baseline in **both video and
  audio**, threaded and inline, from cold boot and from a restored state, across
  all three games and **both regions** (NTSC and PAL, which select different
  `frame_clocks` in the rewritten IRQ path).
- Every controller configuration is bit-identical: joypad, none, multitap, mouse,
  and multitap/mouse in both ports, plus mid-run port swaps in and out of each.
  Super Scope and Justifier are identical up to the cursor-pixel fix above, which
  changes video only -- audio hashes match exactly. Two Justifier configurations
  crashed on the baseline and now run.
- Dropping frames through `GET_AUDIO_VIDEO_ENABLE`, at 1-in-2 and 1-in-3, leaves
  audio hashes byte-identical to an undropped run, confirming the emulated machine
  cannot tell a dropped frame from a rendered one.
- The speed profiles now change only the rendered-frame cadence; audio hashes are
  identical across `full`, `balanced` and `fast`.
- `tests/ppu_benchmark` framebuffer hashes match baseline for all eight PPU modes,
  threaded and inline, in both normal and stress modes.
- `tests/libretro_smoke.py` returns identical video, audio and WRAM hashes,
  identical memory-map descriptors, and an **unchanged state size of 320,941
  bytes** — save states remain compatible.
- All three Android ABIs build, export the same 25 `retro_*` symbols as the
  previous artifacts, and keep 16 KiB load-segment alignment.

## Build

```sh
ANDROID_NDK_HOME=/path/to/ndk scripts/build_android_yage.sh /path/to/ndk
```

Tested with NDK `28.2.13676358`, Android API 24. Outputs land in
`target-libretro/out/optimized/<abi>/`; copy the matching ABI into YAGE's
`android/app/src/main/jniLibs/<abi>/`.

## Reproducing the measurements

The harness used here is not in the tree. It loads a state saved from attract-mode
gameplay, reports windowed frame times plus video and audio hashes and audio RMS,
and can answer `GET_AUDIO_VIDEO_ENABLE` to exercise frame dropping. Measuring
against a freshly booted ROM instead reports title-screen figures and will not
reproduce these numbers.
