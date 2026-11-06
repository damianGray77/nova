# Phase 5 plan: the real GPU and NPU

> **Revision 1, BUILT (2026-11-06).** What the engine does, and the findings, are in `docs/design.md` 6.11. Settled while building:
> - **The GPU kernel** is one shader for all 20 kinds rather than a few families (C5). Reductions use integer atomics, and argmax packs (value, reversed index) for an atomic max.
> - **The NPU (C6)** doesn't take the int8 multiply the plan assumed. The probe found its only exact form, float16 graphs with sums below 2048, so NPU ops became base-8 digit products put back together on the CPU.
> - **The NPU driver** compiles batches only in the second dimension and removes the device when one executes. So every 128 × 128 item is its own dispatch, from command lists recorded once and replayed (recording costs the driver about 1 ms of CPU per dispatch). A batch takes up to 256 items, and ops past that run on the CPU. A self-test at start-up decides whether the NPU is used.
> - **Results:** every device result equals the CPU reference, and hashes match across the CPU, the GPU and NPU, and the GPU alone. In the first checks the devices don't yet speed the world up (6.11 findings 2–3).
>
> Revision 1 was approved by Damian as recommended (2026-11-06).
>
> - **Asked for by Damian (2026-11-06):** "I really do want proper gpu and npu integration though. I want the resource landscape to use everything I have got." He chose this before the UI (now Phase 6, `docs/phase6-plan.md`), approved Windows' own Direct3D 12, DXCore and DirectML for it (no CUDA), and asked for different weights per resource, "since GPUs are designed around a high volume of instructions".
> - **Source of truth:** `docs/design.md`; the in-world GPU and NPU as built are 6.7 and 6.8. 7.1 G had real-GPU batching as a later optimisation; this brings it forward and adds the NPU.
> - **The machine:** an NVIDIA RTX 5080 (16 GB) and the Core Ultra 9 285K's NPU ("Intel AI Boost", a Windows compute-accelerator device), plus the Intel integrated GPU. Windows 11 build 26200 ships Direct3D 12, DXCore, `d3dcompiler_47` and DirectML 1.15.6; the Windows SDK 10.0.26100 has their headers.
> - **Long runs:** the 12-hour run waits for Damian's go-ahead.

## 0. Scope
- The in-world GPU and NPU run on the real devices: GPU ops as Direct3D 12 compute shaders, NPU ops through DirectML on the NPU.
- So the hardware is worth using, in-world accelerator ops become **asynchronous and batched**: issued during a tick, run for every node at once at the barrier, results back a tick or two later.
- The accelerators get **per-resource weights** for high volume: expensive per op, nearly free per element, far more capacity per unit than a CPU millicore.
- An exact CPU reference computes the same results, for checking and for machines without the devices.

**Not here:** the UI (Phase 6); running the interpreter itself on the GPU (organisms are branchy serial programs, a poor fit); CUDA (an optional backend later, if profiling ever shows Direct3D leaving speed on the table); 1,000+ nodes (the scale phase).

## 1. What the design already fixes
- **The ops (6.8):** 16 GPU ops (reductions compare, count, find, sum; copy, fill, not, rcopy; add, sub, xor, and, or, min, max, mul) and 4 NPU ops (dot, matvec, matmul, argmax) on codons, the NPU's in signed fixed point, all integer arithmetic. One accelerator verb (codon 61): BX 0–15 GPU, 16–19 NPU.
- **Holdings (6.7, 6.8):** GPU and NPU units are owned, heated, worn, shared at DIVIDE, taken by ENGULF and freed at death, exactly like millicores.
- **Determinism (7.1 D):** the same seed gives the same world at any thread count. Integer arithmetic makes that possible on any device.
- **Dependencies (7.1 B):** platform libraries only; Damian approved adding Direct3D 12, DXCore and DirectML (2026-11-06).

## 2. Design calls

**C1. Devices and discovery.** DXCore lists the adapters: GPUs (Direct3D 12 compute) and compute-only accelerators (the NPU).
- `accel.gpu = auto | cpu | <part of an adapter's name>` (auto: the GPU with the most dedicated memory, here the RTX 5080). `accel.npu = auto | cpu | gpu` (auto: the NPU if it passes the probe in C6, else the GPU).
- The choice never changes a result (C8), only where the work runs and how fast. The run prints the devices it uses, and `result.txt` records them.
- GPU kernels are HLSL compute shaders compiled at start-up by `d3dcompiler_47` (part of Windows; Shader Model 5.1 is plenty for integer work). No build step and no shader files to ship.
- If a device fails to start, or fails mid-run, that work falls back to the CPU reference, with a printed warning. The results are the same.
- *Recommendation:* as above.

**C2. Asynchronous ops.** `accel.mode = instant | async` (default `instant`).
- *Instant* is exactly Phase 4 (6.7, 6.8): every existing config and pinned hash is unchanged.
- *Async:*
  - **Issue**, during the tick, when the verb runs: the checks are as now (the selector, a unit held, CX > 0, all-or-nothing ops' bounds), the op is sized to this tick's capacity, the CPU cost is charged, and the heads advance as if it were done. The op joins the node's queue, with its addresses and AX. Nothing is read or written yet.
  - **Gather**, at the barrier ending that tick: every node's queued ops, in node order then issue order, read their inputs from memory as it is at that barrier. The batch goes to the devices.
  - **Complete**, at the barrier ending tick t + `accel.latency` − 1 (latency 1: the same barrier; 2, the default: the devices work through the next tick alongside the CPUs). Outputs are written in node order then issue order, into codons the issuer still owns (the rest are skipped). Copy errors are drawn then, from a stream keyed by the op. GPU-written marks are set.
  - **Results that were registers** (compare, count, find, sum, dot and argmax set CX) go to the issuing CPU's *result slot*, readable with SENSE (C10). An op whose CPU has died by then just has its outputs written where they still belong.
- Pending ops are world state: hashed and snapshotted.
- *Why:* a single round trip to a real device costs tens of microseconds, far longer than a 32-element op on the CPU, and early random code issues huge numbers of tiny ops. One batch per tick costs the simulator about the same however clumsy or clever the organisms are. Selection still works on the in-world costs, and latency becomes a skill: wait, interleave, or prepare a tick ahead.
- *Recommendation:* as above, latency 2.

**C3. Weights for high volume (Damian).** In async mode the accelerators default to a high-volume profile, each key still settable:

| | instant (6.7, 6.8) | async default |
|---|---:|---:|
| `gpu.setup` (instructions per op) | 4 | 32 |
| `gpu.cpu_per_element` | 1/16 | 1/1024 |
| `gpu.elements` (per unit per tick) | 32 | 4096 |
| `npu.cpu_per_mac` | 1/64 | 1/16384 |
| `npu.macs` (per unit per tick) | 256 | 65536 |
| NPU size limits (matvec k / matmul k) | 64 / 32 | 256 / 64 |

- A big op is cheap per element and a small one is mostly setup, so selection favours few, full ops: the way real GPUs and NPUs are meant to be used.
- Heat still follows utilisation (work done ÷ capacity), so running flat out still throttles.
- These are starting values. `nova bench` measures the devices and suggests capacities that keep a tick's batch within a tick's CPU time (C12).
- *Recommendation:* as above.

**C4. Keeping everything busy.** With latency 2, tick t's GPU and NPU batches run while the CPUs work on tick t + 1, and the barrier waits only if a batch isn't finished. Each device gets one upload, one dispatch per op family and one readback per tick. CPU, GPU and NPU then all work at once, which is the point.
- *Recommendation:* as above.

**C5. GPU kernels.** A few compute shaders cover the 16 ops:
- elementwise (copy, fill, not, rcopy and the 8 combiners), one thread per element;
- reductions (compare, count, sum) with group-shared partial sums;
- find (the first match).
- Inputs (source and destination codons per op), an op table (kind, count, offsets, AX) and outputs live in a few packed buffers. All integer, so the results are exact.
- *Recommendation:* as above.

**C6. The NPU through DirectML.** dot, matvec and matmul become integer matrix multiplies (int8 × int8 into int32, `DML_OPERATOR_MATRIX_MULTIPLY_INTEGER`), padded to a few fixed shapes (zero padding doesn't change a sum) and batched. The shift and saturation back to codons happen on the CPU at completion; argmax is done there too (k ≤ 64 is trivial).
- *The risk:* NPU drivers support only part of DirectML. At start-up a probe runs a known-answer integer multiply on the NPU. If the NPU refuses the operator or answers wrong, NPU ops run on the GPU (a GEMM kernel) or the CPU instead, with the same results, and the run says so.
- *Recommendation:* as above. The probe is the first thing built, so we learn early what the NPU accepts.
- *Probe results (2026-11-06, `nova devices`):*
  - RTX 5080 and the Intel integrated GPU: compute shaders right; DirectML int8 → int32 and float32 matrix multiplies exact at in-world sizes (largest sum 262,144); float16 not exact at full range.
  - Intel AI Boost NPU: refuses single operators and the integer and float32 forms. It runs **float16 matrix multiplies compiled as graphs**, exactly while every sum stays below 2048, once each graph's initialization has run as its own submission (without that, every result was NaN). A 1×1 convolution with fixed weights works too.
  - *So the NPU path is float16 digit products:* each value v (−32…31) becomes u = v + 32 = 8h + l with digits h, l in 0…7. A K-long sum is cut into chunks of 32, and each chunk needs four digit products (h·h, h·l, l·h, l·l), whose sums stay below 2048. The CPU recombines them exactly: Σ(ua − 32)(ub − 32) = Σ ua·ub − 32 Σ ua − 32 Σ ub + 1024 K, with Σ ua·ub = 64 Σ ha·hb + 8 Σ(ha·lb + la·hb) + Σ la·lb. Graphs are compiled per shape class (matvec, matmul) and batch size, and fed new inputs every tick.

**C7. The CPU reference.** The same async semantics computed on the CPU, in parallel across nodes. It is:
- the fallback when a device is missing or failing;
- the oracle: every device result must equal it bit for bit. `accel.verify = N` (0 = off) checks every Nth batch against it during a run and stops with a report on any difference.
- *Recommendation:* as above.

**C8. Determinism.**
- Kernels are integer only; inputs are read at a barrier and outputs written at a barrier, in node then issue order.
- So the world hash is the same on the GPU, the NPU or the CPU, at any thread count, and with any latency setting held fixed.
- The determinism gate gets an async-accelerator world, run on the CPU reference and, where a device exists, on it.
- *Recommendation:* as above.

**C9. Compatibility.** `accel.mode = instant` keeps everything as in Phase 4. Async is opt-in: `configs/world.cfg` gets a commented block, and the harness gets arms (C12). The instant-mode suites stay as they are.
- *Recommendation:* as above.

**C10. Sensing and the shuffled control.** In async mode SENSE gains reading 18 (my last accelerator result) and 19 (my ops still pending). No new opcodes are needed, and SENSE is a mutation away from everything. Op selectors and maps are unchanged.
- *Recommendation:* as above.

**C11. Detection.** GPU-written marks are set at completion, so an async GPU copy still makes a GPU link and `gpu_replicator` still means copying with the GPU. `npu_user` fires at issue.
- *Recommendation:* as above.

**C12. Bench, configs and arms.**
- `nova bench devices`: each device's batch throughput and round-trip latency, the CPU reference's for comparison, and suggested `gpu.elements` and `npu.macs` for the measured tick rate.
- The `world` workload gets an async-accelerator variant.
- Harness arms `gpu_hw`, `npu_hw` and `accel_hw` (async mode, the high-volume profile), against the baseline.
- *Recommendation:* as above.

**C13. Code layout and the allowlist.** Device code sits behind one interface in `core/src/accel_*.cpp` (DXCore discovery, the Direct3D 12 GPU backend, the DirectML NPU backend, the CPU reference). The engine calls only the interface. The no-STL guard's allowlist gains `d3d12.h`, `dxcore.h`, `DirectML.h` and `d3dcompiler.h`. The build links their import libraries from the Windows SDK. Everything else is unchanged.
- *Recommendation:* as above.

## 3. What gets built
- **Core:** async issue, gather and complete; the pending-op queue (hash, snapshot); result slots and SENSE 18–19; the high-volume profile; the accelerator interface with the CPU reference, the Direct3D 12 GPU backend and the DirectML NPU backend; the NPU probe; `accel.verify`.
- **App and tools:** device lines in the run's output and `result.txt`; `nova bench devices`; the harness arms; the `world.cfg` block.

## 4. Evidence
- `verify.ps1` green, covering:
  - async semantics on the CPU reference: issue and completion ticks at latency 1 and 2, heads, capacity, costs, partial elementwise ops, all-or-nothing ops, ownership changing before completion, an issuer dying, result slots and SENSE 18–19;
  - the GPU backend against the CPU reference on random batches of every op, and over a whole world hash for hash; the NPU backend (or its fallback) the same way. On a machine without the device these say so and are skipped;
  - instant mode unchanged (every pinned hash);
  - the determinism gate on an async world, and snapshot resume with ops pending.
- Manual: `nova bench devices`; a short paired check (`accel_hw` against the baseline) for whether random code takes to the hardware accelerators; a run with `accel.verify` on.

## 5. Docs
- design.md: a new 6.11 (the real devices), 7.1 G and 9.1 (renumbered: the UI is now Phase 6, scale Phase 7), the status line.
- README.
