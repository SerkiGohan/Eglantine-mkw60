# Splitting draw preparation: snapshot + preparation – design

As of 7.11.2026, after B2 step 3. Core of B3 and precondition for B2 step 4 (shadow reader).
Basis: `docs/mkw60-b1-plan.md` (register replica, CB hazard), `docs/mkw60-b2-plan.md` (serial/parallel cost,
guard, contexts).

## Goal

`GraphicsPipeline::ConfigureImpl` (and the rest of `RasterizerVulkan::Draw`) is split into two halves:

1. **Snapshot** (serial, back thread, in draw order): everything that reads **guest memory at draw time**
   or **changes cache structure**. Result: a `DrawSnapshot` that contains only values.
2. **Preparation** (later on other threads): turns the snapshot into bindings, descriptor entries and commands.
   Only reads caches, only writes into its own preparer context (B2 step 3).

First on **one thread** with the switch `EDEN_NX2_PREP_SPLIT=1`: draw = snapshot, then the preparation right away. The image must
stay the same. After that (step 4) a second thread computes the preparation as a shadow and compares.

## Today's flow of ConfigureImpl (as of fork `ccd3450a4e`)

| # | Section | Zone | serial? | why |
|---|---|---|---|---|
| 1 | `SynchronizeDescriptors` (TIC/TSC table) | sync descriptors | yes | registers + guest memory |
| 2 | `SetUniformBuffersState` | – | no | only masks into the context |
| 3 | per stage: read SSBO pointers (`BindGraphicsStorageBuffer`) | read handles | **yes** | `Read<u64>` from a constant buffer |
| 4 | per stage: read texture handles, `GetSamplerId` | read handles | **yes** | `Read<u32>` from a constant buffer, TSC cache |
| 5 | `FillImageViews` (read TIC → image view, `PrepareImage`) | fill image views | **yes** | guest memory, `try_emplace`, upload check |
| 6 | texel buffers from image views (`BindGraphicsTextureBuffer`) | texture buffers | no | reads the image view |
| 7 | `UpdateGraphicsBuffers` (resolve index/vertex/uniform/SSBO/texel) | update buffers | **mixed** | address translation + `FindBuffer` (rarely creation), `SynchronizeBuffer` (upload) |
| 8 | `BindHostGeometryBuffers` | update buffers | mixed | bindings; `SynchronizeBuffer` |
| 9 | `Acquire` + `Reserve` descriptors | descriptor acquire | reservation | B2 piece 3 |
| 10 | per stage `BindHostStageBuffers`: uniforms | uniforms | **yes** | copy guest memory → staging ring, descriptor entry |
| 11 | per stage `BindHostStageBuffers`: SSBO/texel | bind stage buffers | no | descriptor entries; `SynchronizeBuffer` |
| 12 | per stage `PushImageDescriptors` | push descriptors | no* | descriptor entries (*`MarkModification` for written images: serial) |
| 13 | `UpdateRenderTargets`, `CheckFeedbackLoop` | render targets | **yes** | find/create render targets, render pass |
| 14 | `ConfigureDraw` (descriptor upload, bind pipeline, push constants) | configure draw | no | commands |
| 15 | in `Draw`: dynamic state, draw command | dynamic state, draw record | no | commands |

## The snapshot

```cpp
struct DrawSnapshot {
    // header
    GraphicsPipeline* pipeline;        // phase 1: the pipeline lookup stays before the snapshot (15 ms/s)
    bool is_indexed;
    bool heavy;                        // upload/creation needed → whole draw serial (1–3 %)
    Maxwell3D::DirtyState::Flags dirty;// dirty mask of the draw (B2 piece 1)
    DrawState draw_state;              // topology, first index/vertex, count, instances (from draw_manager)

    // Registers: phase 1 DrawRegs() (replica, B1). For B3 every preparer gets its own
    // replica and the change list of every draw (~19 registers/draw measured) in order.
    u32 register_changes_begin, register_changes_end;

    // guest memory values at draw time
    std::array<ConstBufferBinding, 5 × 18> cbufs;      // only active stages/slots
    small_vector<StorageBinding> storage_buffers;      // from SSBO pointers: device address, size, written
    small_vector<ImageViewInOut> views;                 // image view IDs after the TIC lookup
    small_vector<SamplerId> samplers;
    small_vector<DescriptorUpdateEntry> uniform_entries; // finished uniform descriptors (staging ref or buffer)
    // device addresses of the index/vertex buffers (address translation serial, see below)
    DeviceRange index_range;
    std::array<DeviceRange, 32> vertex_ranges;          // only active slots
};
```

Typical size ~1–2 KiB per draw; at ~85,000 draws/s ~100–170 MB/s of writes → reuse in a ring, do not allocate.

### Uploads and creations: check instead of doing

Uploads have to take the data **at draw time**, creations change the cache structure. Neither may go into the preparation.

- For all ranges the draw binds (index, vertex, SSBO, texel), the snapshot checks **read-only** whether they contain CPU-modified
  pages (`memory_tracker`) and whether the buffer already exists (`page_table`). Textures: `PrepareImage` stays serial
  (part of section 5).
- If there is something to do → `heavy = true`: the draw runs fully serially as today (old path). According to M2 this affects 1–3 % of draws.
- The address translation `GpuToCpuAddress` (~25 ms/s) thereby moves into the serial part. In return `FindBuffer` + bindings
  (rest of `update buffers`, ~30 ms/s) stay parallel.
- Safety net: if the preparation still finds a missing buffer or a CPU-modified page (race with
  CPU writes), it aborts and reports "repeat serially". The consumer then repeats the draw in order on the
  old path.

### Uniforms

In Vulkan every uniform binding becomes exactly one descriptor entry (staging copy or buffer binding). The snapshot makes the
copy (serial, 63–70 ms/s) and stores the **finished entries**. The preparation puts them in their place per stage.

## The preparation

`Prepare(const DrawSnapshot&, PrepContext&)` – works only with the snapshot, its own context and read-only cache accesses:

- Look up buffer IDs (`FindBuffer` without creation), bindings into the context (`GraphicsBindings`, B2 piece 2).
- Texel buffers from image views.
- Descriptor entries into the reserved range (B2 piece 3), uniform entries from the snapshot.
- Record commands (bind pipeline, vertex/index buffers, dynamic state, draw) – into the context's **own command list**.
- LRU touches into the log (B2 piece 4).

### Commands: splicing lists instead of secondary command buffers

Today `Scheduler::Record` does not record Vulkan commands but appends lambdas to a `CommandChunk`. Only the VulkanWorker
calls the `vkCmd*`. For parallel preparers it is therefore enough that:

- every preparer context has its own `CommandChunk`s;
- the consumer splices the lists into the scheduler's stream in draw order (relinking pointers, no copying).

That is simpler and cheaper than Vulkan secondary command buffers. The host state (bound pipeline, dynamic state) at the start
of a block is that of the previous block of another context – so a context sets everything again when it takes over. That is exactly
what B2 piece 4 tested (switch after every draw, no rendering errors, not slower).

## What stays serial (estimate from B2 step 1, Mario Bros. Circuit)

| serial | ms/s | parallel | ms/s |
|---|---|---|---|
| uniform copy | ~65 | buffer lookup + bindings | ~30 |
| image views, handles, SSBO pointers, samplers | ~75 | writing descriptors, texel | ~25 |
| flush work, render targets | ~60 | dynamic state | ~20 |
| upload check + address translation | ~40 | ConfigureDraw, recording, rest of draw | ~45 |
| writing the snapshot (new) | ~10–20 | pipeline lookup (later) | ~15 |

→ parallel ~120–135 ms/s wall clock (≈ 75–85 ms/s CPU by the perf ratio). That is rather at the lower end of the B2 estimate.
So it pays off to make the serial chunks cheaper on the side (uniform copy, flush work, image views).

## Phases

| Phase | Content | Switch | done when |
|---|---|---|---|
| P1 | `TakeSnapshot` + `Prepare` as separate functions, one thread, `heavy` → old path | `EDEN_NX2_PREP_SPLIT=1` | image the same, cost measured, share of `heavy` known |
| P2 | Shadow reader: a second thread also computes `Prepare` on a copy (own context, commands discarded) and compares bindings + descriptor entries | `EDEN_NX2_PREP_SHADOW=1` | 0 mismatches, no crash (= B2 step 4) |
| P3 | Real preparers: ring of snapshots, N threads, the consumer splices command lists in order | `EDEN_NX2_PREP_THREADS=N` | image the same, back measurably lighter (B3) |

## Open points

- **Registers for parallel preparers:** own replica per preparer + change lists (B1 measured ~19 registers/draw), or
  copy the registers read by the preparation (~80 groups) into the snapshot. Only needed in P3; measure first which is cheaper.
- `GraphicsPipeline::last_descriptor_payload` / `last_descriptor_buffer_*`: per context instead of per pipeline.
- The AMD logic op workaround in `UpdateDynamicStates` writes registers: per preparer.
- Queries (occlusion queries, `query_cache`) and transform feedback: always `heavy` at first.
- Inline index data (`inline_index_draw_indexes`) and indirect draws: always `heavy` at first.

## Result of P1 (7.11.2026 ~16:50, Mario Bros. Circuit, fork `b7f3f128f7`, log `~/mkw-perf/fork/p1-prep-split-mario.log`)

- `EDEN_NX2_PREP_SPLIT=1` together with all B2 switches (dirty copy, 2 contexts with a switch after every draw, descriptor reservation,
  LRU deferred): **image completely normal** (user).
- The first attempt crashed while loading (`e80932dbab`): end of the uniform entries per stage counted wrongly (`EndCapture` counts from the start of the
  stage) → negative count, `AddEntries` overwrote the descriptors. Fix + `ASSERT` in `b7f3f128f7`.
- ~6.5 uniform entries per draw (out of ~15 entries in total).
- **Only 0.3–0.6 % of draws in the race would be "heavy"** (the preparation uploads / creates after all), almost only buffer uploads (~250–470/s),
  new buffers 0–11/s, textures 0 (texture uploads all happen in the serial part). Menu/loading up to 1.2 %. Better than the M2 estimate (1–3 %).
- Draws/s in the race as usual (75,000–86,000), descriptor reservation still 0 mismatches.
- Open for P2/P3: upload check in the serial part (instead of counting afterwards), cost of the split with perf.

## Result of P2, first version with cache locks (7.11.2026 ~17:30, Mario Bros. Circuit, fork `8202747ba9`, log `~/mkw-perf/fork/p2-shadow-mario.log`)

- `EDEN_NX2_PREP_SHADOW=1` (every 32nd draw) with all switches: **image optimal** (user), no crash. Rare rubberbanding only during
  short drops to 30 FPS (the shadow reader holds the locks, test mode with a switch after every draw).
- ~352,000 compared draws (up to ~2,600/s in the race), queue never full, **"serial needed" 0**.
- **1 mismatch, while loading (58.9 s):** entry 3 of 15 and SSBO binding 0/0. After that **0 mismatches** in all 19 windows
  (count, entries, bindings). Assumption: the shadow reader ran after the real draw; while loading, buffers are created and
  merged (old ones deleted), and it already found the new buffer for the same address. Both are valid for the image, but it shows
  the actual rule for P3: **structural changes (creating/merging/deleting buffers, growing `slot_vector`) only while no
  preparer is reading.**
- This proves: the preparation depends only on the snapshot and its context and reaches the same result on another
  thread.
- Next step P2b: shadow reader **without** cache locks. Needed for that: a read lock for preparers against structural changes
  (reader-writer lock around `CreateBuffer`/`DeleteBuffer`/image creation/`slot_vector` growth), the caches' shared helper vectors
  (`graphics_segments`, `upload_copies`, `Buffer::View` cache) per thread.
