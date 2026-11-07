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
