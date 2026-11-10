# Eglantine-mkw60

An experimental fork of [Eglantine](https://github.com/Eglantine-Community/Eglantine) (Switch 2 emulator, derived from
Eden/yuzu) with one goal: **make Mario Kart World run faster** – ideally at 60 FPS in races – by reworking how the
emulated GPU's work is processed on the host CPU.

> Work in progress. Everything here is measured on one machine (Linux, i9-10900K, RX 7800 XT, Mesa RADV). New code paths
> are behind environment switches and off by default. The original Eglantine README is in
> [README.upstream.md](README.upstream.md).

Plans, measurements and design notes are in [`docs/`](docs/); start with
[`docs/mkw60-roadmap.md`](docs/mkw60-roadmap.md).

## Highlight: hide the minimap (≈60 FPS in the open world)

In the open world, the minimap is a **second 3D view of the world** (its own 448×440 render pass, thousands of draws per
second through the whole emulated GPU chain). With it, frame rates in the open world often drop to ~40 FPS; without it they
mostly stay at 60.

The game already turns this view off by itself in tunnels. Using the debug server tools added in this fork (memory snapshots,
a code trace of which guest code runs, a memory/screenshot watcher) and Ghidra, the switch was found: the game keeps
"minimap view on" in one field of a scene object and sets it at exactly one instruction. A loader patch makes that
instruction always store 0, so the game drops the view just as in a tunnel – no rendering hack, no frozen image.

- **Setting:** *Graphics → Advanced → "Mario Kart World: hide minimap"* (global or per game; restart the game after
  changing it).
- **Environment variable:** `EDEN_NX2_MKW_MINIMAP=0` hides, `=1` shows (wins over the setting).
- The log says `Mario Kart World: minimap 3D view off (main+0x…)` when it is active. The patch looks for the instruction
  pattern, not a fixed address, and does nothing unless it finds exactly one match.

## Highlight: real-time game speed at any frame rate

In its 60 FPS mode, Mario Kart World advances one game step per frame. Whenever the emulator drops below 60 FPS, the
whole game runs in slow motion (at 45 FPS it runs at 75 % speed). The usual workaround, the game's 30 FPS mode, costs
smoothness everywhere, even where 60 FPS would be possible.

The game already has the right mechanism built in. Its frame loop (found with Ghidra) has a dynamic mode that keeps a
time account: each frame adds its cost and subtracts one vblank (16.67 ms) per game step; once the account is a full
vblank behind, that frame advances two steps instead of one. So the game stays in real time at any frame rate between
30 and 60 – as long as it is told the real frame time. In the emulator it was not:

- The game's own cost measure (CPU time and GPU timestamps) does not see the time spent waiting for the emulated GPU.
  It read ~13 ms while frames really took ~17.5 ms, so the game never switched and still ran in slow motion.
- Upstream's "dynamic FPS" patch uses the whole frame time instead, but in a two-step frame the game itself waits
  until 33 ms have passed, and the present is held for two vblanks. Once in two-step mode, it practically stayed there.

The real-time mode uses the whole frame time, removes the game's own wait for the second vblank (one branch in the
vsync function, found by instruction pattern) and presents every frame as soon as it is ready (at most one per vblank).

Measured on Peach Stadium (time trial against a ghost, same machine as above):

| Mode | FPS | Game speed |
|---|---|---|
| forced 60 FPS | 56–57 | 94–95 % (slow motion, down to 74 % in heavy spots) |
| **real-time dynamic** | **56.7** | **99.8 %** (6 % two-step frames) |
| forced 30 FPS | 30 | 100 % |

- **Setting:** *Graphics → Advanced → "Mario Kart World: real-time dynamic FPS"* (restart the game after changing it).
- **Environment variable:** `EDEN_NX2_MKW_FRAME_MODE=5` (wins over the settings; `=1`/`=2` force 60/30 FPS).
- The log says `Mario Kart World: real-time dynamic FPS mode enabled (main+0x…)` when it is active.
- **Do not combine with "CPU Clocks: Boost / Overclock".** These run the guest clock faster (×1.7 / ×2), so every
  frame looks that much longer to the game: it advances two steps per frame too often and runs too fast (up to double
  speed with Overclock). Use the default ("Normal").
- Long stalls (well over 50 ms) are not caught up; the game limits its time account to ±66.7 ms by design.

Also recommended for playing: `EDEN_NX2_RESNAPSHOT=0` (do not re-read queued command lists after a resumed semaphore
acquire; −0.6 ms per frame on Peach Stadium, no visual issues seen).

## Why

Eglantine runs the emulated GPU's command stream on two host threads (the "GPU split"):

- the **front** thread reads the game's command lists, runs GPU macros and forwards register writes,
- the **back** thread turns them into Vulkan work: ~75,000–85,000 draws per second in a race, each needing its buffers,
  textures and descriptors resolved (`GraphicsPipeline::ConfigureImpl`).

Measurements in this fork (wait statistics, Chrome traces, `perf`) show that the GPU is only ~50–65 % busy. Frames are
limited by the CPU chain front → back → GPU, and **in races the back thread is the bottleneck** (busy ~90 % while the game
waits for the GPU). Shortening waits on the front thread does not help; the back thread has to do its work faster.

## The plan

The draw preparation on the back thread is split into

1. a **serial snapshot** in draw order: everything that reads guest memory at draw time or changes cache structure
   (texture handles, image views, render targets, uniform copies, uploads), and
2. a **preparation** that only reads caches and writes into its own context (buffer lookups, bindings, descriptor entries,
   command recording) – this part can run on other threads.

| Stage | What | State |
|---|---|---|
| B0 | Baseline measurements in races | done |
| B1 | Register replica: draw preparation reads registers through `DrawRegs()` | done |
| B2 | Caches safe for concurrent readers: dirty-flag snapshot, per-preparer binding contexts, descriptor reservation, deferred LRU touches, preparer lock against structural cache changes | done |
| P1 | `ConfigureImpl` split into snapshot + preparation (one thread) | done, 0.3–0.6 % of draws need the serial fallback |
| P2 | Shadow preparer on a second thread recomputes the preparation and compares | done, ~3 million comparisons, 0 differences |
| P3 | Real preparer threads; the back thread no longer waits for them | **paused after S3** (see below) |

P3 details: [`docs/mkw60-p3-plan.md`](docs/mkw60-p3-plan.md). S1–S3 work and are tested (register change log, upload
pre-check, blocks recorded behind a placeholder in the command stream, own command stream state per block). Measured on one
thread they cost ~1.6 ms per frame, and preparer threads (S4) would take only ~3.3 ms per frame off the back thread, an
estimated net gain of ~0.5–0.9 ms. That is too little for the effort and risk, so P3 is paused; the code stays behind its
switches and costs nothing without them.

Instead, profiling single hot instructions (`perf annotate`) found cheaper wins (9.11.2026):

| Change | Gain on the Peach Stadium |
|---|---|
| Runtime-switch polling with a thread-local counter instead of a shared atomic in the uniform bind (~520,000 calls/s) | −0.3 ms/frame |
| Draw takes the cache locks before its invalidation (re-entered cheaply) | −0.1 ms/frame |
| **GpuSplit: front ring fields and `back_engine` in separate 128-byte blocks (false sharing)** | **−1.5 ms/frame** |

## Current numbers

Time trial on Peach Stadium with the Nintendo ghost, forced 60 FPS mode, mean over ~2 laps (20 traces of 5 s), compared
position by position:

| Build | Frame time |
|---|---|
| B0 (fork before the rework, 8.11.) | 19.3–19.4 ms (~52 FPS) |
| `mkw60` after cleaning up (8.11.), no switches | 19.8–20.3 ms (~50 FPS) |
| **current `mkw60` (9.11.), no switches** | **18.2 ms (~55 FPS)** |

Measuring lesson learned the hard way: a video playing in a browser in the background cost ~2 ms per frame.

## Building

Prebuilt AppImage and Windows builds are on the [releases page](../../releases) (built by the repository's CI).

Same dependencies as Eglantine. Release build as used for all measurements:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=ON -DYUZU_TESTS=OFF \
      -DCMAKE_C_FLAGS=-march=native -DCMAKE_CXX_FLAGS=-march=native
cmake --build build
```

`-DMKW60_PROBES=ON` compiles in the measurement probes (off by default, so they cost nothing in the hot paths).

## Switches

All are environment variables, read at start. Most exist for testing a single step and are off by default.

| Variable | Effect |
|---|---|
| `EDEN_NX2_MKW_FRAME_MODE=1` / `=2` | Mario Kart World: force 60 / 30 FPS mode (1 is used for measuring) |
| `EDEN_NX2_MKW_FRAME_MODE=5` | Mario Kart World: real-time dynamic mode (same as the setting "real-time dynamic FPS") |
| `EDEN_NX2_MKW_FRAME_MODE=3` / `=4` | Mario Kart World: leave the mode word to the game (switchable live through the debug server) with the game's own frame cost / the whole frame time; for analysis |
| `EDEN_NX2_MKW_MINIMAP=0` / `=1` | Mario Kart World: hide / show the minimap's 3D view (overrides the setting "hide minimap") |
| `EDEN_NX2_RESNAPSHOT=0` | upstream switch: no re-read of queued command lists after a resumed acquire (recommended) |
| `EDEN_NX2_DEBUG_SERVER=127.0.0.1:7777` | upstream debug server; this fork adds the tools `snapshot_memory` (all writable guest memory to disk) and `code_trace` (which guest instructions run in a time window); `EDEN_NX2_DEBUG_WRITE=1` allows `write_memory` |
| `EDEN_NX2_PENDING_WAKE_US` | front thread waits for a wake-up instead of yield + 1 ms sleep when channels are suspended (default 200, 0 = old behaviour) |
| `EDEN_NX2_WAIT_STATS=1` | wait statistics every 10 s in the log; with it, `touch /tmp/eden_trace` writes a 5 s Chrome trace to `~/eden-trace-*.json` (open in ui.perfetto.dev) |
| `EDEN_NX2_FLUSH_OPT` | merge a draw's cache invalidation ranges (default 1) |
| `EDEN_NX2_UBO_DIRECT` | compare and copy uniforms straight from guest memory (default 1) |
| `EDEN_NX2_DRAW_REPLICA=1..4` | B1 register replica (3 = draw preparation reads it) |
| `EDEN_NX2_DIRTY_SNAPSHOT`, `EDEN_NX2_PREP_CONTEXTS`, `EDEN_NX2_PREP_BLOCK`, `EDEN_NX2_DESC_WRITER`, `EDEN_NX2_LRU_DEFER` | B2 pieces (per-preparer state) |
| `EDEN_NX2_PREP_SPLIT=1` | P1 snapshot + preparation |
| `EDEN_NX2_PREP_SHADOW=N` | P2 shadow preparer on every Nth draw (`_LOCKED=1`: with cache locks) |
| `EDEN_NX2_PREP_REGLOG=1` / `=2` | P3 S1: preparers replay a register change log (2 = also compare with a full copy) |

Probe switches (`EDEN_NX2_*_PROBE`, `EDEN_NX2_CACHE_GUARD`) only work in a `-DMKW60_PROBES=ON` build.

## Documents

- [`docs/mkw60-roadmap.md`](docs/mkw60-roadmap.md) – goal, stages, log of results
- [`docs/mkw60-b1-plan.md`](docs/mkw60-b1-plan.md), [`docs/mkw60-b2-plan.md`](docs/mkw60-b2-plan.md) – register replica, thread-safe caches
- [`docs/mkw60-b3-snapshot-design.md`](docs/mkw60-b3-snapshot-design.md) – snapshot + preparation design, P1/P2 results
- [`docs/mkw60-p3-plan.md`](docs/mkw60-p3-plan.md) – preparer threads, steps S1–S5
- [`docs/mkw60-m2-design.md`](docs/mkw60-m2-design.md) – earlier design study

## Credits

- The Eglantine, Eden, Sudachi and yuzu developers – this is their emulator.
- [SmileyTatsu](https://github.com/SmileyTatsu/Eglantine) – the three ASTC decode/upload commits are theirs (origin in each
  commit title).
- ZendNevs – the Mario Kart World 30 FPS patch, parts of which are carried here.

Licensed like Eglantine (GPL-3.0-or-later, see [LICENSE.txt](LICENSE.txt) and [LICENSES/](LICENSES/)). This project is not
affiliated with Nintendo; you need your own legally dumped game and keys.
