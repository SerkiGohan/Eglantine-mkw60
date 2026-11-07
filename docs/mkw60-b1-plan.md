# B1: Making draw inputs visible – plan

Goal of B1: everything a draw reads during preparation comes from a clearly separated **draw context**
instead of being scattered across `maxwell3d->regs`, `maxwell3d->state`, the dirty flags and guest memory.
B1 does not change speed yet and runs on one thread. It is the precondition for B3
(preparation on other threads) and measures what such a context costs.

## Inventory (as of fork `f341673756`)

### Registers

The preparation reads ~80 register groups (shader configuration, blend, depth/stencil, rasterizer,
viewport/scissor, vertex streams and attributes, index buffer, render targets/zeta, tessellation,
transform feedback, TIC/TSC tables, sampler binding, constant buffer selection …).

| File | direct `maxwell3d->regs` | via `regs` reference | dirty flags |
|---|---|---|---|
| `renderer_vulkan/vk_rasterizer.cpp` (draw, dynamic state) | 20 | 188 | 9 |
| `renderer_vulkan/fixed_pipeline_state.cpp` (pipeline key) | 1 | 75 | 6 |
| `renderer_vulkan/vk_graphics_pipeline.cpp` (`ConfigureImpl`) | 1 | 9 | 0 |
| `renderer_vulkan/vk_pipeline_cache.cpp` | 2 | 3 | 0 |
| `buffer_cache/buffer_cache.h` | 9 | 9 | 20 |
| `texture_cache/texture_cache.h` | 12 | 30 | 15 |
| `shader_cache.cpp` | 8 | 13 | 2 |

~40 registers change per draw (B0/M2 measurement). A copy of the whole register set per draw
(14 KB) would be too expensive (~1.1 GB/s); a **replica kept up to date through the changes** costs ~26 MB/s.

### Dirty flags

Register writes set `maxwell3d->dirty.flags`; the preparation **reads and clears** them
(e.g. dynamic state, vertex buffers, render targets). For the ordered command recording, the state of the
**globally previous draw** is what counts. Therefore: the back thread determines the dirty mask per draw
(globally, in order) and passes it along in the context; preparers read it but clear nothing shared.

### Constant buffer bindings

`maxwell3d->state.shader_stages[5].const_buffers[18]` (address, size, enabled). They rarely change per draw
→ also kept up to date through changes.

### Guest memory

24 read sites in the preparation, the most important:
- Texture handles from constant buffers (`ConfigureImpl`, `read_handle`, `gpu_memory->Read<u32>`).
- Storage buffer pointers from constant buffers (`BufferCache::StorageBufferBinding`, `Read<u64>`).
- Uniform data (fast copy into the ring buffer, `BindHostGraphicsUniformBuffer`).
- TIC/TSC descriptors (`DescriptorTable`), vertex/index data when synchronizing, texture uploads.

**Hazard:** between draws the game writes ~1 million constant buffer words per second (~14 per draw) into
guest memory via `ProcessCBMultiData`. If a preparer reads later, it can see the values of a later
draw. Whether that really happens (same addresses within a few draws) has not been measured yet.

## Steps

1. **Measure the hazard** (measurement only, switch `EDEN_NX2_CB_HAZARD_PROBE`): for the last K draws
   (K = 8/32/128), remember the guest memory ranges read (uniform ranges, handle addresses,
   storage buffer pointers). On every constant buffer write, count whether it hits a range that one
   of these draws read. The result decides whether handles/uniforms have to be read serially
   (then ~70 ms/s stay serial) or whether the constant buffer writes have to go into the context too.
2. **Register replica with check mode** (switch `EDEN_NX2_DRAW_REPLICA=1`): a second register set,
   kept up to date only through the changes the back thread logs between two draws. Check mode
   (`=2`): before every draw compare the replica with the real set, mismatches go to the log. Measures the cost
   of keeping it up to date and proves that nothing is lost.
3. **Redirect the accesses** (mechanical, no change in behaviour): in the preparation paths the code reads
   registers, constant buffer bindings and dirty masks through a `DrawContext` instead of directly from `maxwell3d`.
   One thread: the context points to the real state. With `EDEN_NX2_DRAW_REPLICA=1` it points to the replica
   → if the image stays the same, the redirection is complete.
4. **Measure and record:** cost (perf, time trial on Mario Bros. Circuit), image comparison by the user.

## Done when

- With the replica MKW looks the same (the user checks, check mode reports 0 mismatches).
- Cost of replica + context measured (goal: < 3 % of the back thread).
- Hazard from constant buffer writes quantified and decision for B3 made.

## Result of step 1 (7.11.2026, time trial on Mario Bros. Circuit, fork `1825b1e0c6`, log `~/mkw-perf/fork/b1-cb-hazard-mario.log`)

| | per second |
|---|---|
| Constant buffer writes | ~0.9–1.05 million (~3.5–4.1 MiB), ~50 % with a changed value |
| Hits on memory that the **directly previous** draw read | handles ~250,000 (changed ~130,000), SSBO pointers ~155,000 (~40,000), uniforms ~80,000–97,000 (~56,000–68,000) |
| Hits 2–32 draws back | uniforms ~50,000, handles ~30,000, pointers ~7,000 |
| Hits 33–128 draws back (ring at the same place again) | uniforms ~80,000, handles ~70,000 |
| no hit | ~170,000–290,000 |

(64-byte chunks: neighbouring bytes count too, so the numbers are rather too high – the conclusion holds anyway.)

**Decision for B3:** everything read from guest memory (texture handles, SSBO pointers, uniform copy into the ring)
is read by the back thread **at draw time** into the snapshot. Only steps that keep working with these values run
in parallel (image views/samplers, buffer bindings, pipeline, descriptors, dynamic state).
Estimate: of ~257 ms/s of draw work, ~80–100 stay serial, ~150–170 can be parallelized.

