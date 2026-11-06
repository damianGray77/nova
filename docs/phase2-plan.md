# Phase 2 plan: the in-world GPU

> **Revision 1, APPROVED by Damian as recommended (2026-11-05) and BUILT.** What the engine does, and the first short paired check, are in `docs/design.md` 6.7. One detail settled while building: capacity is set at the start of each tick, so GPU units gained mid-tick (a claim, an ENGULF, a birth) add capacity from the next tick. The text below is the plan as approved.
>
> - **Source of truth:** `docs/design.md`. Phase 2's scope is in 9.1; the GPU itself is decided in 5.1 D–E, 4.5 and 7.1 G.
> - **This file holds:** what the design already fixes, the design calls G1–G9 with recommendations, what gets built, and how it is checked.
> - **Long runs:** per Damian, the 12-hour paired runs wait until everything is built (design.md 9.1); Phase 2 is checked with runs of minutes.

## 0. Scope (design.md 9.1)
- In-world GPU block ops, implemented on the CPU, are enabled.
- Paired abiogenesis: GPU on versus off.
- Watch for the first lineage that uses GPU units.

## 1. What the design already fixes
- **The op (5.1 D):** one selector op (copy, add, xor, compare) applied across CX elements from the read head to the write head. Each element is cheap, setup costs extra, and the GPU runs hot.
- **Ownership:** GPU units are claimed with ALLOC like any resource (ALLOC's selector 2), and are held, heated, throttled and released like millicores (4.5, 6.1).
- **Costs (5.1 E):** within a node, GPU costs scale with the amount moved; no bus model.
- **Deferred (5.1 D):** the divergence penalty and the tensor/NPU unit.
- **Off means error (6.2):** a disabled feature makes its selector set the error flag, so genomes stay meaningful across configs.
- **Not in Phase 2 (7.1 G, 9.1):** real-GPU (CUDA) batching is a Phase 6 optimisation, and CUDA would need confirming as a dependency first.

What exists today: codon 61 decodes to GPU and always fails; ALLOC and RELEASE with selector 2 always fail. There is no GPU ledger or heat yet.

## 2. Design calls

**G1. What GPU does.** `GPU`, with BX picking the op and CX the element count:
- *0 compare:* CX ← how many of the n codon pairs [read head + i] and [write head + i] differ. Writes nothing.
- *1 copy:* [write head + i] ← [read head + i], with the usual copy-error chance per element.
- *2 add:* [write head + i] ← ([write head + i] + [read head + i]) mod 64.
- *3 xor:* [write head + i] ← [write head + i] xor [read head + i].
- Both heads advance by the elements done and wrap exactly as COPY's do. Private code applies per element: reads only from the body or waste, writes only into the body, and a hot page may refuse a write. The op stops at the first element it may not do.
- *Order:* 0 = the op that changes nothing, as decided for DISK (5.1 B: "0 = compare"). Putting copy at 0 would make a GPU replicator need no BX setup, the kind of thumb on the scale the nearest-to-self rule avoids.
- *Partial work:* the op does as many elements as the body's GPU capacity has left this tick (G2). For copy, add and xor, CX ← the elements not done (0 when finished), so a loop can carry on next tick. Compare leaves the mismatch count of what it did. It fails if it did nothing: no GPU units, no capacity left, CX ≤ 0, or the first element refused.
- *Recommendation:* as above.

**G2. Capacity and cost.**
- `world.gpus` = GPU units per node. **Default 0:** no GPU, everything GPU fails exactly as now, and GPU-off runs are byte-identical to Phase 1b (hashes unchanged).
- Each held unit does `gpu.elements` (default 32) element-ops per tick at full speed, scaled by its heat speed factor. A body's capacity is the sum over its units; unused capacity does not carry over.
- The issuing CPU pays `gpu.setup` (default 4) instructions plus `gpu.cpu_per_element` (default 1/16) per element done, so CPU cost scales with the amount moved, as 5.1 E says.
- *Scale:* a COPY loop copies one codon per 2 instructions (COPY, DJNZ). A 32-codon GPU copy costs 6 instructions plus one unit's whole tick: about 10× cheaper in CPU, paid for in GPU units, heat and claiming them. That's the "faster polymerase".
- *For the GPU-on arm:* `world.gpus = 400`, one unit per 10 millicores.
- *Recommendation:* as above, all knobs.

**G3. Holding GPU units.** The same rules as millicores:
- ALLOC (GPU selector) claims CX units chosen at random from the GPU pool, capped by it; RELEASE gives back CX, oldest first, keeping their heat.
- DIVIDE hands the daughter floor(units × CX/64) of her parent's GPU units (5.1: "each non-memory holding").
- ENGULF moves the prey's GPU units, with their heat; death returns them to the pool, with their heat.
- GPU units alone don't keep a body alive: death is still no running CPU or no millicore.
- `gpu.wear` (default 0), like `cpu.wear`. The GPU-on arm uses 0.001, so held GPU units turn over like CPU (the D10 lesson: what costs nothing to hold gets hoarded).
- *Recommendation:* as above.

**G4. GPU heat (4.5).**
- Every GPU unit has a heat. Input per tick: `heat.gpu_gain` × the body's GPU utilisation (elements done ÷ nominal capacity). Same cooling.
- Curve `heat.gpu.*` with the CPU's defaults (warm 40, hot 60, max 100), but `heat.gpu_gain` 9 against the CPU's 6: flat out, a GPU settles at 90, deep in its throttle band (the CPU settles at 60, the throttle onset). That is "the GPU runs hot": full use throttles it hard, so pacing pays.
- Past `max` a unit is freed, hot.
- Hot GPUs raise the copy-error rate of GPU ops (their own error multiplier, like the CPU's).
- *Recommendation:* as above.

**G5. Sensing.** Three SENSE readings are appended after the existing eight (whose numbers don't change): 8 = my GPU units, 9 = mean heat of my GPU units, 10 = free GPU units on the node.
- With no GPU on the node, readings 8–10 fail like any disabled feature, and the shuffled SENSE map stays over 8 readings, keeping GPU-off runs identical to Phase 1b.
- *Recommendation:* as above. (Appending breaks strict nearest-to-self order for the new readings, but renumbering 0–7 would change every existing genome's meaning.)

**G6. The shuffled control.** The GPU op number goes through the selector map too: a 4-entry map drawn in the shuffled arm, redrawn until compare is not left at 0. It is drawn only when the node has GPUs, so GPU-off shuffled runs are unchanged.

**G7. Watching for GPU use (detection).**
- GPU writes count as the parent's writes for `detect.written` DIVIDE links.
- A link is a *GPU link* when most of the daughter's matching codons were written by GPU ops.
- New: `gpu_links` per lineage and per CSV window, plus `gpu_held` (units held at window end) and `gpu_elements` (element-ops done).
- A replay event `gpu_replicator` marks the first time a lineage that has met the gate makes a GPU link. That is the 9.1 milestone "first GPU-using lineage", read as using the GPU to copy, because merely holding GPU units happens by chance (any random ALLOC with selector 2).
- *Recommendation:* as above.

**G8. Paired check.**
- Harness arm `gpu` (`world.gpus = 400`, `gpu.wear = 0.001`) against `baseline`, same seeds, short runs (a few minutes each) now, with the long runs later.
- Plus a seeded smoke test: a hand-written GPU-copy replicator, to show the faster polymerase works and to give the detector a positive control. It is seeded only as a test, never in a trial.

**G9. Snapshots.** GPU state (owners, heat, pool, per-body lists) is saved only when the node has GPUs. The snapshot's embedded config already fixes the shape, so the format version stays 1 and GPU-off snapshots are unchanged.

## 3. What gets built (once approved)
- Soup: the GPU unit ledger (owners, per-body lists kept beside `Body`, which is hashed raw and must not change), pool, heat, wear, and capacity per tick; ALLOC, RELEASE, DIVIDE, ENGULF and death handling; hash and snapshot of GPU state only when GPUs exist.
- Interpreter: the GPU op (G1) with private-code checks, page throttling, copy errors, detector write hooks and the selector map.
- Config: `world.gpus`, `gpu.elements`, `gpu.setup`, `gpu.cpu_per_element`, `gpu.wear`, `heat.gpu_gain`, `heat.gpu.*`, and `seed.gpus` for seeded tests.
- Detection and outputs: GPU links, the `gpu_replicator` event, CSV columns `gpu_held`, `gpu_elements`, `gpu_links`, and `result.txt` fields.
- Harness: the `gpu` arm. Bench: a `gpu` workload (the seeded GPU replicator).

## 4. Evidence
- `verify.ps1` green, covering:
  - each op, partial work across ticks, the private-code and page checks, copy errors under GPU heat;
  - ledger conservation with GPU units (claims, release, DIVIDE share, ENGULF, death, wear, heat release);
  - sensing, and the GPU-off failures;
  - the shuffled GPU map;
  - **GPU-off byte-identical to Phase 1b** (the pinned hashes unchanged);
  - the determinism gate with GPUs on, events included;
  - the seeded GPU replicator copying faster than its COPY twin, and the `gpu_replicator` event replaying at 1 and 20 threads.
- Manual: the `gpu` vs `baseline` short paired check, and a bench of the GPU workload.

## 5. Docs
- design.md: a new 6.7 (Phase 2 semantics), 9.1 and the status line, 5.1 D (built).
- README.
