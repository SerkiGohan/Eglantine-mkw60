# Eglantine-mkw60

An experimental fork of [Eglantine](https://github.com/Eglantine-Community/Eglantine) (Switch 2 emulator, derived from
Eden/yuzu) with one goal: **make Mario Kart World run faster** – ideally at 60 FPS in races – by reworking how the
emulated GPU's work is processed on the host CPU.

> Work in progress. Everything here is measured on one machine (Linux, i9-10900K, RX 7800 XT, Mesa RADV). New code paths
> are behind environment switches and off by default. The original Eglantine README is in
> [README.upstream.md](README.upstream.md).

Plans, measurements and design notes are in [`docs/`](docs/); start with
[`docs/mkw60-roadmap.md`](docs/mkw60-roadmap.md).

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
| P3 | Real preparer threads; the back thread no longer waits for them | in progress (S1 of S5 done) |

P3 details: [`docs/mkw60-p3-plan.md`](docs/mkw60-p3-plan.md). Expected gain on the heaviest track (Peach Stadium):
about 1–1.5 ms per frame. Reaching 60 FPS there will also need cheaper serial work and a faster front thread.

## Current numbers

Time trial on Peach Stadium with the Nintendo ghost, forced 60 FPS mode, mean over ~2 laps (20 traces of 5 s), compared
position by position:

| Build | Frame time |
|---|---|
| B0 (fork before the rework) | 19.3–19.4 ms (~52 FPS) |
| current `mkw60`, no switches | 19.8–19.9 ms (~50 FPS) |

The rework is not active yet without switches; the ~0.5 ms are its base cost until P3 takes work off the back thread.
Measuring lesson learned the hard way: a video playing in a browser in the background cost ~2 ms per frame.

## Building

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
