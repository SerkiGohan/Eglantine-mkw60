# M2: Decoupling draw preparation from the back GPU thread – design

As of 7.11.2026, fork `mkw60` (base nx2-experimental `3f4c7a3`). Goal: Mario Kart World at 60 FPS.
All numbers: Peach Stadium (open world), standing still, RX 7800 XT / i9-10900K, fixed 60 FPS for measuring.

## Starting point (measured)

| | Value |
|---|---|
| Frame time | ~36–40 ms (25–28 FPS) |
| Real work at the front (GPU thread) | ~24 ms/frame: parsing commands, ~1.5 million macro calls/s, ~9–10 million register writes/s |
| Real work at the back (GPUBack) | ~26 ms/frame: ~610 ms/s, of which registers/methods ~365, draw preparation ~255, compute/fences/uploads ~80 |
| Draws | ~76,000–83,000/s (~2,700/frame) |
| `ConfigureImpl` | ~220–260 ms/s ≈ 3.4 µs/draw |
| Waiting for each other | front ~280 ms/s in drain, back ~120–180 ms/s idle |
| Free CPU | ~3.2 of 20 logical CPUs in use; VulkanWorker ~18 % |

Findings that shape the design:

1. **The chain is latency-bound.** Every removal of waiting code (acquire drains, writer waits,
   early releases, indirect draws) only moves the waiting. The front thread depends on results
   that are only produced at the back (releases, GPU-written command lists).
   It only gets faster if **every link at the back finishes sooner**.
2. **Draws do not repeat** (M1 measurement): ~93 % have a configuration never seen before, because the game
   puts its data at new addresses every frame. A draw cache is ruled out.
3. **Draw preparation is almost read-only** (M2 measurement): 97–99 % of draws only look things up in the
   buffer/texture cache and copy their uniforms (~5 per draw) into the ring buffer. 1–3 % upload or create
   (buffer uploads ~10–14 MiB/s, texture uploads ~20/s).
4. **~40 registers change per draw** (out of 3,584). A full copy of the state per draw: ~1.1 GB/s;
   only the changes: ~26 MB/s.

## What a draw reads during preparation

- Registers (`maxwell3d->regs`): shaders, pipeline state, vertex/index buffers, render targets, sampler mode.
- `maxwell3d->state.shader_stages[*].const_buffers` (5 × 18 bindings).
- **Guest memory:** texture handles from constant buffers (`read handles`), uniform data (copy into the ring),
  storage buffer pointers from constant buffers, TIC/TSC tables.
- Buffer cache: resolving bindings (`FindBuffer`, page tables), synchronizing (usually nothing to do).
- Texture cache: TIC → image view (`FillImageViews`), samplers, render targets.

### Hazard: guest memory changes between draws

The game (or NVN) writes into guest memory between draws via constant buffer updates (`0x8e3/0x8e4…`,
`ProcessCBMultiData`), for example base vertex/instance into the driver buffer at 0x640. If another thread
prepares draw N while the back has already written the updates for draw N+1, it reads **wrong values**.
Every design therefore has to either
- do the uniform copy and the handle reads **at the time of the draw** on the back thread
  (that is "read handles" + uniform copy, ~40 % of the preparation), or
- pass constant buffer updates to the preparer in order with the draws.

## Design A: another pipeline stage ("Back2")

```
front (parse, macros) → back (registers, methods) → Back2 (caches, draw preparation, compute) → VulkanWorker
```

- The back only runs the state machine and passes draws and all cache-touching methods (uploads,
  inline data, compute, queries) in order through a ring to Back2, as front → back does today.
- Back2 is the only thread that touches the caches → **no concurrency in the caches**.
- Per draw, the ~40 changed registers plus the constant buffer bindings go through the ring
  (Back2 keeps its own copy of the register set, like the front thread does today).
- **Gain:** back ~26 ms → ~13–15 ms of work, Back2 ~12–13 ms.
- **Risk:** the chain gets one stage longer. Releases (queries) go through caches/fences, so they run on Back2.
  An acquire at the front then waits for two stages instead of one. Since the chain is latency-bound, this can
  cancel the gain. **Measure before building:** how much of the release latency arises in the register half,
  how much in the cache half?
- Effort: medium (the pattern exists: `GpuSplit`). Roughly 1–2 weeks.

## Design B: parallel preparers

- The back stays the pacemaker. It collects draws in blocks (e.g. 32) and hands each block to one of N
  preparers. Every preparer has its own register copy, kept up to date through the change ring.
- Preparers record into their own Vulkan command buffers (secondary command buffers inside the render pass).
  The back thread inserts them in order.
- Heavy draws (upload/creation, 1–3 %) and everything between the blocks (compute, uploads) run serially:
  close the block, all preparers wait, run serially, continue.
- The caches have to cope with reads from several threads: `slot_vector` must not move when inserting
  (stable storage, segment lists), LRU/used marks collected per thread or atomic,
  uniform ring per thread.
- **Gain:** the back's draw preparation of ~255 ms/s spread over N cores, the chain does not get longer.
- **Risk:** high. Many places in the cache code silently assume a single thread; bugs show up as
  rare crashes or rendering errors. Secondary command buffers cost a bit on AMD/RADV.
- Effort: large, more like months.

## Recommendation

1. **First measure what A costs in chain length** (split the release latency into the register and cache halves).
   If the cache half's share of the release latency is small, A is a good first step.
2. Build A behind a switch (`EDEN_NX2_BACK2=1`), A/B measure standing still.
3. Only if A pays off and Back2 becomes the bottleneck, extend Back2 to B (parallel preparers behind Back2).

## Open measurements

- Release latency: time from queuing a release at the back until it is written, split into register/cache share.
- Constant buffer updates between draws: how many, where to (same addresses as uniforms read before?).
- Share of compute/uploads between the draws (block boundaries for B).
