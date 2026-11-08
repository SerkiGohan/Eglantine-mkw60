# B2: Thread-safe cache read paths – plan

Goal of B2: the parts of the draw preparation that are to move to other threads in B3 may only **read** the buffer and
texture cache. Everything that secretly writes during a lookup is collected, deferred or done serially.
B2 still runs on one thread. It ends with a stress test using a second reading thread.

Basis: B1 (registers through `DrawRegs()`, cost ~1 %), M2 measurement (97–99 % of draws upload nothing and create nothing),
CB hazard (guest memory has to be read at draw time).

## What costs what? (perf, time trial on Mario Bros. Circuit, 7.11. 13:39, `~/mkw-perf/perf/graph-20261107-133937/`)

Back 806 ms CPU/s, of which `RasterizerVulkan::Draw` 258, `ConfigureImpl` (all variants) ~195:

| Part (inclusive, ms/s) | | Guest memory? | writes during lookup |
|---|---|---|---|
| `BindHostStageBuffers` (uniform copy + SSBO/texel bindings) | 55 | yes (uniform copy) | uniform ring, `fast_bound`/`binding_sizes`, LRU, descriptor queue |
| `DoUpdateGraphicsBuffers` (resolve index/vertex/SSBO/texel) | 23 | no (bindings already resolved) | `channel_state` bindings |
| `SynchronizeBuffer` (check/upload CPU-modified pages) | 22 | yes, on upload | upload, page tracking |
| `BindHostGeometryBuffers` | 15 | no | LRU, vertex bindings (compares with the previous draw) |
| `RefreshContents` / `PrepareImage` (texture up to date?) | 13.5 / 12 | yes, on upload | LRU, flags, upload |
| `GetSamplerId` + `DescriptorTable::Read` (TSC/TIC) | ~10 + ~9 | **yes** (TIC/TSC entries) | cache `descriptors`/`bound_ids` per channel |
| `VisitImageView` | ~7 | yes (TIC) | `channel_state->image_views`, `CreateImageView` (rare) |
| `PushImageDescriptors` | 10 | no | descriptor queue |
| `CurrentGraphicsPipeline` (key + lookup) | 18 | no | current pipeline, rarely a new build |
| `UpdateDynamicStates` | 7 | no | dirty flags (read + clear) |
| `UpdateRenderTargets` | 7.5 | no | render target cache, framebuffer (rarely new) |
| Address translation `GpuToCpuAddress` (self, spread out) | 25 | – | – |
| Locks (`pthread_mutex`, `SpinRecursiveMutex`) | ~30 | – | – |

Observations:
1. **Almost everything "expensive" has a guest memory part.** Uniform copy, TIC/TSC entries, handles and SSBO pointers have to
   be read at draw time (B1 step 1). So the parallel share is smaller than the ~150–170 ms/s from B1.
2. Many "bind" functions save work by comparing with the **previous draw** (`fast_bound_uniform_buffers`,
   `uniform_buffer_binding_sizes`, vertex bindings, current pipeline, render targets). In parallel, a preparer only knows
   the previous draw of its block → the first draw of each block binds everything again (measure the cost in B3).
3. `Draw` holds both cache locks (`vk_rasterizer.cpp` ~l. 529, `std::scoped_lock{buffer_cache.mutex, texture_cache.mutex}`).

## Places that assume a single thread

| Place | File | Problem with 2 readers | Solution |
|---|---|---|---|
| `SlotVector::insert` → `Reserve` moves all objects | `common/slot_vector.h` | references of other threads dangle | rule: **insert/erase only in serial phases** (preparers idle). No rework of `SlotVector` needed. |
| LRU `Touch` (`TouchBuffer`, `PrepareImage`) | `buffer_cache.h` ~l. 2315, `texture_cache.h` ~l. 3651 | linked list gets relinked | touch log per thread, applied serially at the end of the block (only for cleanup, order does not matter) |
| `DescriptorTable::Read` remembers entries | `texture_cache/descriptor_table.h` | shared cache per channel | TIC/TSC reads belong in the serial snapshot anyway → the cache stays serial |
| `channel_state->image_views.try_emplace`, `CreateImageView`, `FindSampler` | `texture_cache.h` ~l. 829, ~l. 2949 | insert into a hash table | serial (in the snapshot: TIC → image view ID) |
| `RefreshContents`/`SynchronizeBuffer` with upload | `texture_cache.h` ~l. 1667, `buffer_cache.h` | upload + flags | serial; the snapshot checks "must upload?" and marks the draw as heavy |
| uniform ring, staging pool, `GuestDescriptorQueue` | `vk_buffer_cache.h`, `vk_staging_buffer_pool`, `vk_update_descriptor` | shared ring pointer | own ring/own queue per preparer |
| comparison with the previous draw (`fast_bound…`, `binding_sizes`, vertex bindings) | `buffer_cache.h` ~l. 1184–1343, ~l. 1090–1170 | result depends on order | own binding state per preparer (copy a subset of `channel_state`) |
| reading and clearing `dirty.flags` | `vk_rasterizer.cpp`, `buffer_cache.h`, `texture_cache.h` | shared | determine the mask serially and pass it along (B1 plan) |
| `Scheduler::Record` | `vk_scheduler.h` | one command stream | B3: secondary command buffers per preparer |
| `frame_tick`, `any_buffer_uploaded`, `has_deleted_images` | both caches | shared flags | only set serially; preparers report back |

## Split between serial and parallel (proposal)

**Serial on the back thread (snapshot):** register replica (B1), dirty mask, handles, SSBO pointers, reading TIC/TSC and
translating them into image view/sampler IDs (with cache), "must upload?" check (buffers and textures), copying uniform data into
the ring, pipeline key.
**Parallel (preparers):** buffer IDs → Vulkan bindings and offsets (`FindBuffer` only reads if nothing is built), writing
descriptors (`PushImageDescriptors`, buffer descriptors), dynamic state, recording commands.

## Steps

1. **Measure the split (measurement only, `EDEN_NX2_SPLIT_COST_PROBE=1`):** in `ConfigureImpl` and `Draw`, measure the time for
   the steps that stay serial according to the proposal and for the rest – with **few** clock reads per draw (≤ 3, not per
   zone; lesson from the zone profile). Result: ms/s serial vs. parallelizable. **Decision point:** if less than ~80 ms/s
   can be parallelized, B3 in this form is not worth it (back < 10 % lighter); then rethink the split before building.
2. **Guard (`EDEN_NX2_CACHE_GUARD=1`):** counters at every writing place of the table above (insert, erase, touch, upload,
   caches). For the draws that are to be prepared in parallel, every place must show **0** in the parallel part
   (except the ones deliberately kept per thread). Shows what still has to be reworked.
3. **Move side effects out of the parallel part:** LRU touch log, own binding state and own descriptor queue per preparer
   context (first a single context, on the back thread), a snapshot function that collects the serial values. Switch
   `EDEN_NX2_PREP_SPLIT=1`: a draw runs as "snapshot, then preparation from the snapshot" on one thread. The image must stay the
   same (the user checks), measure the cost.
4. **Stress test with 2 readers:** a second thread additionally runs the parallel part for every draw as a "shadow"
   (without using the result) and compares its bindings/descriptors with those of the back thread. 0 mismatches and no
   crash over a lap and 10 min of open world → B2 done.

## Done when

- Split measured and decision recorded (step 1).
- The guard shows no shared writes in the parallel part any more.
- Shadow reader: 0 mismatches, no crash.

## Result of step 1 (7.11.2026 ~13:55, time trial on Mario Bros. Circuit against the Nintendo ghost, fork `eef8bcee5a`, `EDEN_NX2_PREP_COST_PROBE=1`, log `~/mkw-perf/fork/b2-prep-cost-mario.log`)

Race (10 lines of 10 s each, ~80,000–95,000 draws/s), wall-clock time inside the draws, exclusive, ms/s:

| serial | ms/s | parallelizable | ms/s |
|---|---|---|---|
| uniform copy | 63–70 | resolving buffers (`update buffers` without SynchronizeBuffer) | 51–58 |
| `flush work` (FlushWork + FlushCaching per draw) | 46–50 | rest of `Draw` (PrepareDraw, locks, index/query) | 31–39 |
| image views (read TIC, look up, PrepareImage) | 38–45 | dynamic state | 18–21 |
| SynchronizeBuffer | 34–52 | rest of `ConfigureImpl` | 15–17 |
| handles/SSBO pointers/samplers | 30–37 | pipeline lookup | 13–16 |
| render targets | 12–14 | writing descriptors | 11–14 |
| texture uploads | 2–22 | stage bindings without uniforms | 8–10 |
| descriptor tables | ~2 | ConfigureDraw, recording, acquire | ~9 |
| **total** | **~240–270 (≈ 60 %)** | **total** | **~150–185 (≈ 40 %)** |

Draw total ~400–445 ms/s (4.6–5.5 µs/draw), measurement overhead estimated at ~20–24 ms/s.

**Caution, absolute numbers too high:** perf (user CPU cycles only) saw `RasterizerVulkan::Draw` at ~258 ms/s, the probe measures
wall-clock time (with kernel time, preemption/blocking in locks, measurement overhead). The **ratio** is reliable: ~40 % of the
draw time can be parallelized → on a perf basis **~105 ms/s CPU**, by wall clock up to ~175 ms/s.

**Decision:** threshold (~80 ms/s) just exceeded → B2 continues as planned, but the gain of B3 alone would be
limited (back ~13–20 % lighter). Besides that, **making serial parts cheaper** pays off, independent of B3:
`flush work` ~48 ms/s per draw (FlushCaching/invalidation on every draw), `SynchronizeBuffer` ~36 ms/s (page check,
almost never an upload), uniform copy ~65 ms/s. Open question: how much of the wall-clock difference to perf is blocking (lock in
`Draw`, submission in `flush work`)?

## Result of step 2 (7.11.2026 ~14:30, time trial on Mario Bros. Circuit, fork `0959967001`, `EDEN_NX2_CACHE_GUARD=1`, log `~/mkw-perf/fork/b2-cache-guard-mario.log`)

Median over 13 lines in the race (~85,000 draws/s), writes per draw:

| Place | serial | **parallel** | where (parallel) | Consequence for step 3 |
|---|---|---|---|---|
| buffer binding state (`channel_state`) | 0 | **34.5** | update buffers | own binding state per preparer |
| descriptor queue | 6.7 | **9.7** | push descriptors 7.6, bind stage buffers 1.1, acquire 1 | own queue per preparer; uniform descriptors are created serially → staging reference in the snapshot, the preparer writes the entry |
| scheduler record | 0.2 | **4.6** | update buffers 1.5, configure draw 1.2, dynamic state 0.8, draw record 1 | own command buffer per preparer (B3: secondary command buffers) |
| dirty flags cleared | 0.2 | **5.7** | dynamic state 3.2, update buffers 2.3, pipeline lookup 0.2 | record the dirty mask serially per draw, preparers only read |
| LRU touch | 0.68 | **0.07** | update buffers, bind stage buffers | touch log |
| pipeline state (current pipeline) | 0 | **0.2** | pipeline lookup | current pipeline per preparer |
| slot/LRU insert/erase | ~300/s | **~4–6/s** | update buffers (= creating buffers) | heavy draw → serial fallback |
| staging requests, TIC/TSC cache, new image views/samplers, image changed, dirty set | | 0 | – | stay in the serial part, as planned |

**Result:** the parallel part writes exactly the expected things, there was no surprise. Structural changes (creating buffers) happen
in the parallel part only ~5 times per second → a serial fallback is enough. Everything else is state that can be kept per preparer:
bindings, descriptors, commands, dirty mask, current pipeline, LRU log. Not covered by the guard: `PipelineCache::graphics_key`
(refilled on every draw, a member) and `GraphicsPipeline::MarkUsed` – both also per preparer or logged.

## Result of step 3, piece 1: dirty mask (7.11.2026 ~15:35, Mario Bros. Circuit, fork `8ccd315e29`, `EDEN_NX2_DIRTY_SNAPSHOT=1`, log `~/mkw-perf/fork/b2-dirty-snapshot-mario.log`)

- **0 draws with a direct change** of the shared flags (all 17 lines, menu + race) → redirection through `DrawDirty()`/`StateTracker::DirtyFlags()` complete.
- A draw clears ~5 flags (menu up to 14). Practically none are set (≤ 0.06/draw, only while loading).
- Flags that a draw only **sometimes** consumes (for B3: the serial part has to know in advance what a preparer consumes):
  - `VertexBuffer1…10` (no. 17–26): only cleared if the draw uses the slot (~1–2 %), otherwise they stay set → can be determined from the registers (number/use of the vertex streams).
  - `IndexBuffer` (48): only for indexed draws (non-indexed ~30/s).
  - `DepthBiasGlobal` (50): `TouchDepthBias` checks with `||` – if `DepthBias` is set, `DepthBiasGlobal` stays set (short circuit). Rebuild for B3 or turn into a fixed rule.
- **Image (user): completely normal** in menus and on Mario Bros. Circuit → piece 1 done.
- Also found (not covered by the guard, for piece 4): own state in the `StateTracker` (topology, stencil values, `stencil_reset`) and `ShaderCache::last_shaders_valid`.

## Result of step 3, piece 2: binding contexts of the buffer cache (7.11.2026, Mario Bros. Circuit, fork `7f70593b06`, log `~/mkw-perf/fork/b2-prep-contexts-mario.log`)

- 2a (`d8013c2fd9`): graphics binding state as `GraphicsBindings` (access `BufferCache::Gfx()`), a pure refactoring.
- 2b (`7f70593b06`): up to 4 contexts per channel; writes from outside the preparation go to all contexts (`ForEachGfx`:
  binding/unbinding constant buffers, deleting buffers, compute resets uniform bindings, vertex buffers stale). A context that
  takes over forgets its host uniform bindings and resolves index/vertex buffers again (dirty flags).
- **Hardest test: `EDEN_NX2_PREP_CONTEXTS=2 EDEN_NX2_PREP_BLOCK=1`** (switch after **every** draw, ~80,000–93,000 switches/s in the race):
  **image perfect** (user, menu + Mario Bros. Circuit), no errors in the log. Draws/s in the race as usual (~80,000–93,000).
- Consequence for B3: the binding state of the buffer cache can be kept cleanly per preparer; rebuilding it on takeover
  is cheap enough that even blocks of 1 go unnoticed.

## Result of step 3, piece 3: descriptor reservation per draw (7.11.2026 ~16:10, Mario Bros. Circuit, fork `e76da1d9d8`, log `~/mkw-perf/fork/b2-desc-writer-mario.log`)

- `EDEN_NX2_DESC_WRITER=1`: after `Acquire`, `ConfigureImpl` reserves exactly `num_descriptor_entries` (`UpdateDescriptorQueue::Reserve`) and
  writes through its own pointer; only the reservation and the frame wrap stay shared (in B3 an atomic addition).
- The first attempt crashed while loading (`8fe19463a7`): reservation in every `Acquire` (also compute pipelines, never finished) and
  a `thread_local` pointer for **all** queues → the BC encoder pass (ASTC → BC3) wrote into a foreign range, RADV crash in the worker.
  Lesson: always bind thread-local redirections to their owner.
- With the fix, **together with the dirty copy and a context switch after every draw** (`EDEN_NX2_DIRTY_SNAPSHOT=1 EDEN_NX2_PREP_CONTEXTS=2
  EDEN_NX2_PREP_BLOCK=1 EDEN_NX2_DESC_WRITER=1`): **image perfect** (user), ~15 entries/draw in the race, **0 draws with more or fewer
  entries than reserved**, 0 without reservation, dirty copy still 0 direct changes.
- Still open for B3: uniform entries are created in the serial part (uniform copy) in between the entries of the parallel part
  (order per stage) → pass them as finished entries in the snapshot. `GraphicsPipeline::last_descriptor_payload` (comparison with
  the last draw of the same pipeline) → piece 4.

## Result of step 3, piece 4: pipeline/scheduler state per context, LRU log (7.11.2026, Mario Bros. Circuit, fork `f27dacd19b`, log `~/mkw-perf/fork/b2-piece4-mario.log`)

- On a context switch additionally: pipeline key per context (stale from the context's own last draw), look up the current pipeline and shader stages
  again, `Scheduler::InvalidateState` (bound pipeline, rescaling, descriptor buffer, StateTracker: dynamic state,
  topology, stencil), dirty flags `Shaders`, `VertexInput`, `Blending`, `ViewportSwizzles` set. `EDEN_NX2_LRU_DEFER=1`: LRU touches
  collected per draw and applied at the end.
- **All four pieces together, switch after every draw** (`EDEN_NX2_DIRTY_SNAPSHOT=1 EDEN_NX2_PREP_CONTEXTS=2 EDEN_NX2_PREP_BLOCK=1
  EDEN_NX2_DESC_WRITER=1 EDEN_NX2_LRU_DEFER=1`): **no rendering errors, not noticeably slower** (user). Draws/s in the race 73,000–86,000
  (as usual), descriptors 0 mismatches, dirty copy 0 direct changes.
- LRU: ~1.6 million deferred touches/s (~20 per draw, of which only ~0.75 real relinks – the rest are repeats within the same frame);
  0–1 dropped per 10 s (buffer deleted in the same draw). For B3: filter out same-frame touches before logging.
- **Step 3 is done with this.** Known for B3: pass uniform entries from the serial part in the snapshot;
  `GraphicsPipeline::last_descriptor_payload`/`last_descriptor_buffer_*` per preparer or locked; the AMD logic op workaround writes
  into the replica (per preparer).

## Serial chunk "flush work" (7.11.2026 ~20:30, Mario Bros. Circuit, fork `d0b8ec3fb1`, `EDEN_NX2_FLUSH_PROBE=1`, log `~/mkw-perf/fork/flush-probe-mario.log`)

Race, ~85,000–99,000 draws/s:

| | ms/s (wall clock) |
|---|---|
| `FlushWork` (wake the worker every 32, submit every 128 draws) | ~3 |
| `FlushCaching` total | **~53–58** |
| of which buffer cache `WriteMemory` | ~28–30 |
| of which query cache `InvalidateRegion` | ~7 |
| of which texture cache `WriteMemory` | ~6 |
| of which pipeline cache `InvalidateRegions` | ~5.5 |
| rest (accumulator, address translation) | ~6 |

- **~90 % of draws** invalidate something first: on average **~4.7 ranges with ~160 bytes together** – the game's constant buffer updates
  (`ProcessCBMultiData` → `WriteBlockCached`), which the accumulator (`InvalidationAccumulator`, 32-byte grid) only merges if they
  are directly adjacent. That is ~420,000 ranges/s, each one goes through four caches (~120 ns per range).
- Outside draws ~4,500 invalidations/s (DMA, copies).
- Starting points: (1) merge nearby ranges in the same memory block (fewer passes; invalidating too much is harmless in the buffer cache,
  in the texture cache theoretically not), (2) a fast path in the buffer cache if the range is already CPU-modified and not GPU-modified.

## Flush optimization A/B (8.11.2026 ~00:10, Mario Bros. Circuit with ghost, fork `f99ae01943`, `knob_sweep.py flush_opt 0 3 0 3 0 3 0 1 2 0 3`, log `~/mkw-perf/fork/flush-opt-ab-mario.log`)

Cost per draw (flush measurement, weighted over all windows with > 20,000 draws):

| Mode | ranges per invalidation | FlushCaching | buffer cache | query | texture | pipeline |
|---|---|---|---|---|---|---|
| 0 off | 5.25 | 728 ns | 443 ns | 86 | 78 | 61 |
| 1 merge | 4.57 → **2.71** | **476 ns** | 231 ns | 54 | 58 | 47 |
| 2 fast path | 5.13 | 547 ns | 259 ns | 83 | 76 | 61 |
| 3 both | 4.91 → 3.00 | 501 ns | 239 ns | 58 | 63 | 50 |

- Merging saves **~250 ns per draw ≈ 23 ms/s** (~2.5–3 % of the back thread), the fast path alone ~180 ns, both together no more
  than merging alone (after merging there is little left for the fast path).
- Frame times from the timelines (0: 20.96 ms, 3: 21.85 ms, 1: 22.1, 2: 16.9) vary with the track section more than the expected
  gain (~0.25 ms/frame) – not measurable like this in a race. Window `09-2` was in the finish/lap loop (the game waits 9.3 ms/f).

## Serial chunk "uniform copy" (8.11.2026, Mario Bros. Circuit with ghost, fork `54279eb8c6`, `EDEN_NX2_UBO_PROBE=1`, log `~/mkw-perf/fork/ubo-probe-mario.log`)

- perf (`graph-20261107-200035`): `BindHostGraphicsUniformBuffer` ~90 ms CPU/s, of which ~45 ms/s `memcpy`/`memcmp` in libc and ~13 ms/s
  `ReadBlockUnsafe` (guest memory → intermediate buffer), ~6 ms/s page checks (`IteratePages`), ~4 ms/s staging ring.
- **~460,000–550,000 uniform binds/s, ~230–275 MiB/s, ~520 bytes per bind.** Sizes: ≤ 64 B 15 %, ≤ 256 B 39 %, ≤ 1 KiB 25 %, ≤ 4 KiB 20 %.
- The reuse (`BindSmallUniformReusing`: same bytes in the same command buffer → rebind the old staging copy) **hits 63–66 %**,
  0.5 % same in another command buffer, 34–37 % changed. So the comparison pays off.
- Approach "UBO direct": compare and copy directly from guest memory instead of through the intermediate buffer (one copy less per bind).
