# Roadmap: Eglantine-mkw60

Goal: Mario Kart World on this PC (i9-10900K, RX 7800 XT) at 60 FPS – first in races, in the long run also in the
open world. Way: parallelize the back GPU thread (part B), then the front one (part F).
Every stage ends with a measurement in the same run (A/B via switch) and an entry below.

Basis and measurements: `docs/mkw60-m2-design.md`.

## Way of working

- Every change is its own commit with a rationale. Measurement code and rework stay separate.
- New behaviour always comes with a switch (default off), so that A/B is possible in the same run and a bug
  can be switched off at once.
- Measurements use `perf` (sampling) and the wait statistics. The zone measurement (`EDEN_NX2_GPU_PROFILE`)
  distorts the back thread by ~27 % and is no good for breakdowns.
- Fixed measuring spots: Peach Stadium open world (standing still), Dino Dino Jungle tunnel/outside (standing still),
  time trials on a light and a heavy track.
- The code is written with AI assistance (Claude); the README says so as well once the fork is published.

## Part B: back thread

| Stage | Content | Done when |
|---|---|---|
| **B0** baseline | Measure races (time trial light/heavy), settle on a measuring method without zone distortion (perf script + wait statistics), record the starting values | table with FPS and thread load per measuring spot |
| **B1** making draw inputs visible | `DrawSnapshot`: everything a draw reads during preparation (register subset, constant buffer bindings, handles and uniform data at draw time). `ConfigureImpl` only works with it – still on one thread | same image, cost of the snapshot measured |
| **B2** thread-safe read paths | Caches for concurrent lookups: stable storage instead of a moving `slot_vector`, lookups without side effects (collect LRU/used marks), list of all places that assume a single thread | stress test with 2 readers without a crash |
| **B3** parallel preparers | A pool of N threads turns snapshots into finished draws (bindings, descriptors); the back inserts them in order; draws with upload/creation (1–3 %) run serially | switch on: same image, back measurably lighter |
| **B4** fine-tuning | Block size, thread count, fallback, locks | A/B result documented |

## Part F: front thread (after B)

Starting point: ~24 ms of real work per frame on Peach Stadium – parsing commands, ~1.5 million macro calls/s
(99 % repeats, ~50 % without effect), ~9–10 million register writes/s (over half with the same value).
Ideas (to be measured before building): pass macro results on directly as compact state changes instead of register
by register; pre-parse command lists in parallel; process channels separately where no semaphores connect them.

## Log

| Date | Stage | Result |
|---|---|---|
| 7.11.2026 | M1 draw reuse | 93 % of draws unique → rejected |
| 7.11.2026 | M2 measurement of cache changes | 97–99 % of draws read-only → B possible in principle |
| 7.11.2026 | M2 design A | register half only ~50 ms/s → rejected |
| 7.11.2026 | B0 race baseline | Time trial (Nintendo ghost), fixed 60 FPS, wait statistics: Mario Bros. Circuit 46–55 FPS (18.2–21.9 ms), Peach Stadium track 41–54 FPS (18.4–24.4 ms). GPU 52–66 %. While the game waits for the GPU: **back 89–91 % busy, front 35–44 %**; critical path ~57–63 % drain (the front waits for the back, of which acquire ~51–58 %). → In races the back thread is the bottleneck; ~2–8 ms per frame are missing for 60 FPS. Timelines `~/mkw-perf/traces/b0-*-20261107-*`. |
| 7.11.2026 | B1 step 1 CB hazard | The game constantly overwrites memory the previous draw read (handles/pointers/uniforms, ~50 % with a new value) → guest memory reads stay serial in the snapshot, only the resolution afterwards runs in parallel |
