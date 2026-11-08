# P3: real preparer threads – plan

As of 8.11.2026, fork `42ec56cb1c` (after the cleanup). Basis: `docs/mkw60-b3-snapshot-design.md` (snapshot + preparation,
results P1/P2/P2b), `docs/mkw60-b2-plan.md` (contexts, descriptor reservation, LRU deferral).

## Starting point

- **Peach Stadium, time trial with ghost:** B0 19.3–19.4 ms, cleaned-up fork 19.8–19.9 ms (+0.5 ms base cost of the rework).
  In races the back is the bottleneck (busy 89–93 % during the slot wait), the front waits ~5.5 ms/frame in the drain for the back.
- **What P1/P2b showed:** the preparation (resolving buffers, bindings, descriptors, image entries) depends only on the snapshot
  and its context, runs on a second thread **concurrently** with the back thread and reaches the same result
  (3.07 million comparisons, 0 mismatches). Structural changes of the caches wait for readers through `Common::Nx2PrepLock`.
- **What P2b does not have yet:** the shadow preparation was discarded. The real draw still ran completely at the back. Missing:
  registers without a 14 KB copy, the upload decision *before* the preparation, recording of the commands by the preparer, insertion into
  the command stream in draw order.

## Expected gain (honestly)

- Parallelizable according to B2/P1: ~120–135 ms/s wall clock ≈ **75–85 ms/s CPU** of ~840 ms/s at the back (~10 %).
- At the back, ~16.5 ms of work per frame at ~51 FPS → **−1.5 ms at the back**. Since the back is only partly on the critical path, rather
  **−1 to −1.5 ms per frame** on Peach (19.8 → ~18.5 ms, ~54 FPS). The cost of distributing (ring, state per block) comes off that.
- P3 **alone does not** bring 60 FPS on Peach Stadium. After that: make the serial chunks cheaper (uniform copy ~65, image views/handles ~75,
  flush work/render targets ~60 ms/s) and part F (front).

## Flow with P3

```
back thread (serial, draw order)                      preparers 1..N                   VulkanWorker
--------------------------------                      --------------                   ------------
draw k: flush work, pipeline lookup, handles,
  image views, render targets + render pass,
  uniform copy, upload/creation check
  → heavy? yes: old path completely here
  → no: snapshot into the block, placeholder
        in the scheduler stream (once per block)
block full / non-draw command / submission
  → block to a preparer ───────────────────────────→  catch up registers,
continue with draw k+1 ... (does not wait)            resolve buffers, bindings,
                                                       descriptors, dyn. state,
                                                       draw commands into own list
                                                       → block done ───────────────→  placeholder: waits
                                                                                       until the block is done,
                                                                                       runs its commands
```

**Core idea:** the back thread never waits for preparers. At the position of the block it puts a **placeholder** into the command stream
(`Scheduler::Record`, a lambda with a pointer to the block). The VulkanWorker runs the commands later anyway; when it reaches the
placeholder, it waits until the block is done and then plays back its commands. That keeps the order automatically, without
relinking lists.

**Blocks instead of single draws:** a preparer gets several consecutive draws (start: 16). State (bound pipeline,
dynamic state, bindings) carries over within the block; only at the start of a block is everything set again (as tested in B2 piece 4).
A block ends when: full, at every non-draw command at the back (compute, clear, copy, barrier), render pass change, submission/flush.

## Steps

Every step has a switch, can be tested on its own and has no effect without the switch.

### S1 – registers without a copy: change log (`EDEN_NX2_PREP_REGLOG=1`)

- The back thread writes the changed registers (index, value) of every draw into an ordered log (B1: ~19 per draw, from
  `Nx2SyncReplica`/dirty bits, which already exist).
- Every preparer has its **own replica** and replays the log up to its draw (including the draws of other preparers)
  → ~1.6 million register writes/s per preparer, cheap.
- Test: the shadow reader (P2b) reads its replayed replica instead of the 14 KB copy → still 0 mismatches.

### S2 – upload decision in advance (`EDEN_NX2_PREP_PRECHECK=1`)

- In the serial part, check all bound ranges (index, vertex, SSBO, texel) **read-only**: CPU-modified pages
  (`memory_tracker`), buffer present (`page_table`/`FindBuffer` without creating). Hit → `heavy` → draw completely on the old path.
- Replaces `SynchronizeBuffer` in the preparation with "nothing to do" (check: otherwise counter "preparation would have had to upload").
- Test with P1 on one thread: the counter stays 0, heavy share ~0.3–0.6 % (as in P1), image the same. Measure the cost (replaces part of
  SynchronizeBuffer ~36 ms/s, must not get more expensive).

### S3 – the preparation records into its own list, one thread (`EDEN_NX2_PREP_RECORD=1`)

- `GraphicsPipeline::ConfigureDraw` (without `RequestRenderpass`, which stays serial), `UpdateDynamicStates` and the draw command write
  through a context `Recorder` into the block's command list instead of directly into the scheduler. Placeholder in the scheduler as above.
- Per block its own state cache (pipeline, dynamic state, descriptor buffer, rescaling) instead of `Scheduler`/`StateTracker`.
- Writing places per context instead of shared: `last_descriptor_payload`/`last_descriptor_buffer_*` (per pipeline → per context),
  descriptor buffer ring (reserve the range serially), the AMD logic op workaround in `UpdateDynamicStates` (writes registers).
- Always heavy (old path): queries/occlusion, transform feedback, inline indices, indirect draws, mesh draws, pipeline not built
  yet.
- Still everything on the back thread (preparation right after the block) → the image must be the same; measure the cost of the block
  boundaries (block size 1/4/16/64 via `EDEN_NX2_PREP_BLOCK`).

### S4 – preparer threads (`EDEN_NX2_PREP_THREADS=N`)

- Ring of blocks, N threads (start N = 1, then 2), fixed round-robin assignment.
- Preparers hold `Nx2PrepLock` shared (exists from P2b); LRU touches in their own log, the back thread applies them.
- The VulkanWorker waits at the placeholder (spin briefly first, then sleep); counter "worker waited" in the wait stats.
- Safety net: if a preparer still finds something to upload/missing (race), it counts it and uploads it **without** aborting through
  the serial path (pointer to the block, the back thread does it before the next block) – the image may then be briefly wrong,
  the counter must stay 0.
- Test: image the same, no crash, 20 min of play; A/B Peach with ghost (A B A B) against the cleaned-up fork.

### S5 – fine-tuning

- Block size, N, kind of waiting; evaluate the wait stats "back" and "worker waited".
- Then decide: make the serial chunks cheaper (uniform copy, image views) or part F.

## Risks

| Risk | Countermeasure |
|---|---|
| The preparation reads state that the back thread changes in the meantime (caches, registers) | registers only from its own replica (S1); caches read-only, structural changes through `Nx2PrepLock`; P2b: 0 mismatches |
| A new writing place in the preparation is overlooked | run the cache guard (`PROBES=ON` + `EDEN_NX2_CACHE_GUARD=1`) on the preparer zones in S3 |
| Wrong state at the start of a block (pipeline/dyn. state not set) | S3 with block size 1 first (every draw sets everything), then larger |
| The worker waits too often → the GPU gets work later | counter; hand off the block earlier; increase N |
| Crashes in the worker (placeholder without a finished block at shutdown/device loss) | a block always has an end (also "dropped"), shutdown empties the ring |
| Gain smaller than expected | after S3 measure how much time really disappears at the back; below ~40 ms/s CPU stop and tackle the serial chunks |

## Decision points

- **After S2:** does the advance check cost more than `SynchronizeBuffer` in the preparation? If so, limit the check to index/vertex
  and detect SSBO uploads (rare) as heavy through the counter.
- **After S3:** how many ms/s remain at the back when the preparation is taken out (switch: run the preparation in the placeholder itself on
  the worker = free upper bound for S4)? Below ~40 ms/s CPU → S4 is not worth it.

## Result of S1 (8.11.2026 ~16:45, fork `f2fa9f310e`, race, `EDEN_NX2_PREP_REGLOG=2` + all P2b switches, `PREP_SHADOW=4`)

- Image normal (user). The shadow reader reads the registers only from its replayed replica.
- 25 windows of 10 s each (~3.3 million comparisons), ~12,500–16,700 jobs/s in the race: **mismatches with the full copy 0**, skipped 0, no new base copy after the first,
  prep shadow still 0 mismatches (count, entries, bindings), serial needed 0.
- Replayed ~220–260 registers per job (every 4th draw) → ~55–65 registers per draw in the race (B1 measured ~19 on Mario Bros. Circuit).
  For P3 that means ~5 million register writes/s per preparer – cheap compared with the 14 KB copy (~1.2 GB/s on every draw).
- **S1 done.** Next step: S2 (upload decision in advance).

## Result of S2 (8.11.2026 ~17:05, fork `dad492bea6`, race, `EDEN_NX2_PREP_SPLIT=1 EDEN_NX2_PREP_PRECHECK=1` + all B2 switches, no shadow)

- Image normal (user). 32 windows of 10 s each (~22.6 million checked draws), up to ~96,000 draws/s.
- **Advance verdict = actual behaviour in every window:** heavy 0.30–1.07 % (vertex buffer uploads ~100–620/s, SSBO ~35–100/s,
  index 0–6/s, new buffers 0–4/s), **MISSED 0**, **needlessly heavy 0**, special draws and texel 0.
- Cost 223–274 ns/draw ≈ **12–21 ms/s** wall clock (incl. two clock reads). Decision point after S2 met: cheaper than
  `SynchronizeBuffer` in the preparation (~36 ms/s), which can be dropped for clean draws in P3. Possible later: pass the ranges computed
  in the check on to the preparation (save the duplicate `GpuToCpuAddress`).
- **S2 done.** Next step: S3 (the preparation records into its own command list, placeholder in the scheduler stream).

## Result of S3a (8.11.2026 ~17:20, fork `e601015ca8`, race, `EDEN_NX2_PREP_RECORD=1` + PRECHECK + SPLIT + B2 switches)

- Image normal (user), no hang, no new errors in the log.
- 35 windows of 10 s each: blocks 47,000–68,000/s (= all clean draws), **closed early 0**, **worker waited 0** (one thread:
  the block is always done before the worker reaches it).
- Precheck with quads as heavy: 0.8–1.6 % heavy (of which special draws/quads ~200–235/s), MISSED still 0.
- This proves: placeholder + block list give the same command order. Scheduler state is still shared → S3b.

## Result of S3b (8.11.2026 ~20:40–21:10, fork `3c521ce83a` … `7b0915e079`)

- Own command stream state per block (pipeline, rescaling, descriptor chunk, StateTracker cache), the main stream is "stale" after a block
  and only set again when needed; several clean draws per block (`EDEN_NX2_PREP_BLOCK_DRAWS=16`).
- **Crash in the 1st test** (SIGSEGV in the placeholder, chunk pointer 0x25): the paused block was kept in a scheduler member, and
  `Scheduler::Wait` (closes open blocks) also runs on the present thread → it closed the back thread's block while the back thread
  was continuing it. Fix `ee8783ada1`: paused block thread-local. After that ~7 min without a crash.
- With the B2 test switches (context switch after every draw): image normal, 4–6 draws/block, main stream reset ~60,000×/s (the
  test mode calls `InvalidateState` on every draw).
- **Without the B2 test switches** (one context; `DRAW_REPLICA=3 DIRTY_SNAPSHOT DESC_WRITER LRU_DEFER PREP_SPLIT PREP_PRECHECK PREP_RECORD
  PREP_BLOCK_DRAWS=16`): image normal, ~12,500–13,300 blocks/s, **5.5–6.7 draws/block**, closed early 0, worker waited ~330–410/s,
  main stream reset ~1,000/s.
- **What ends blocks** ("Prep record block end by"): `SamplesStreamer::PauseCounter` ~7,200–7,800/s (the occlusion counter is
  paused between draws), full (16) ~3,700–4,300/s, `BufferCacheRuntime::CopyBuffer` ~550–650/s, submit/wait ~330/s,
  `BindIndexBuffer` (heavy draws) ~120–290/s, `StartCounter` ~130/s.
- Idea for larger blocks: do not treat serial commands between draws (pausing/starting counters, copies) as a block end, but put them
  into the block as the "preamble" of the next draw (block = sequence of draw slots made of preamble + prepared commands). Only worth it
  once the cost per block boundary can be measured.
