# Phase 8 plan: programs on the GPU

> **Revision 2, APPROVED by Damian (2026-11-06), BUILT (2026-11-06).** What was built is in design.md 6.14. Details settled while building:
> - **Lanes live on the GPU** between rounds, as C10 planned, but came in a second step: the first build kept them on the host. Damian asked for the speed-ups after the first findings. Now lanes the host changed go up and lanes the host needs come back, and a device error stops the run (resume from a snapshot).
> - **`warp.device = auto`** shares each round between the GPU and the CPU at once (long chains of steps to the CPU), and runs whole ticks without the GPU when that is quicker (the choice never changes a result). In today's worlds that is mostly without: bodies average about 10 warp steps a tick, which the CPU does in about 6 ms at 1,000 nodes, below the GPU tick's floor.
> - **The GPU's compiler (FXC) shaped the shared source:** steering values are handed to all lanes through groupshared memory with a barrier after each; a lane's stack lives in memory; nothing with a side effect sits in `&&` or `?:`.
> - **ENGULF by a body born on a GPU** takes the prey's CPUs as well as its warps.
> - **Upkeep:** a body with no running CPU pays it all from its warps; with both, its warps pay what its CPUs fall short of.
> - **The preset** needed a new landscape setting, `res.<x>.against`, to put GPU-rich regions where CPU is poor.
>
> Two changes from revision 1, both his:
> 1. **Two forms of life.** "Birth should be allowed with a GPU OR a CPU": programs born on the GPU are a form of life alongside those born on the CPU, not a fallback for when the CPU runs out (C4).
> 2. **The real GPU now**, not later: warps run on the machine's GPU from the start of this phase (C10). Running on the GPU shaped four new calls: a warp works only on its own body's memory (C5), its lanes read before they write (C6), world-changing instructions wait for the host (C7), and random draws are keyed per lane (C9).
>
> - **His direction (revision 1):** landscapes have GPU-rich nodes with almost no CPU, where nothing can live today (a body with no running CPU dies). So the GPU needs an instruction set as rich as the CPU's, letting programs run on it alone. It must also *differ* from the CPU, so programs can use the strengths of both: the CPU for serial work, the GPU for parallel.
> - **Source of truth:** `docs/design.md`: 6.2 (the instruction set), 6.4 #1 (the death rule), 6.7 and 6.8 (the in-world GPU and NPU), 6.11 (the real devices), 6.13 (landscapes and real resources).
> - **Long runs:** the 12-hour run waits for Damian's go-ahead.

## 0. Scope
- **Warps:** a new way for a body to run code. A warp runs the body's code in lockstep across many lanes, with every instruction available. Warps run on the machine's real GPU, with a CPU reference that gives exactly the same results.
- **Two forms of life:** a body is born on a CPU or on a GPU, and either kind can live, divide and spread.
- **The rest:** a milestone for the first GPU-born replicator, the viewer, and a GPU-rich landscape preset.

**Off unless `gpu.warps = true`:** with it off, everything is exactly Phase 7, every hash included.

**Not here:**
- lockstep on the NPU;
- changes to the CPU's instructions or to the block ops;
- anything that would make results depend on timing. Overlapping GPU and CPU work is allowed only where it leaves every result unchanged (C10).

## 1. What exists
- **A body runs on CPUs.** A CPU is registers AX, BX, CX and DX, a stack, the IP, read and write heads, and an error flag. Its instructions per tick come from the millicores the body holds, less memory upkeep. Bodies get their one CPU at birth; more arrive only by ENGULF.
- **The in-world GPU today is block ops:** the GPU verb (codon 61) with BX 0–15 does one op across CX elements, paid from the body's GPU units (`gpu.elements` per unit per tick). BX 16–19 are the NPU's ops. In `accel.mode = async` they run on the real GPU and NPU (Phase 5) through one Direct3D 12 compute shader (`cs_5_1`, so 32-bit integers, no 64-bit ones and no wave intrinsics).
- **The death rule:** a body with no running CPU dies.
- **A node's tick:** sparks, the bodies' CPUs in schedule order, then heat, rot, wear, volatility and the death sweep. The nodes tick in parallel on the threads, then meet at the barrier.

## 2. Design calls

**C1. Warps: lockstep lanes, each with its own registers.** A warp belongs to a body and has `warp.lanes` lanes (default 32, the width of the GPU's own hardware warps; up to 64).
- Each lane is a full CPU state: registers, stack, IP, heads, error flag.
- *Lockstep:* each warp step runs the instruction at the lowest IP among the lanes still running, for every lane at that IP (a "min-IP first" rule).
  - Lanes that agree run together, cheaply.
  - Lanes that branch apart take turns, one group per step.
  - Lanes that come back to the same address (a loop's head) merge again by themselves.
- So the GPU rewards code where lanes agree, and punishes branchy serial code: the strength and weakness of a real GPU.
- *Recommendation:* as above.

**C2. Lane heads: offset by lane, moving by the lane count.** In a warp, lane L's read and write heads sit L codons past the warp's, and COPY, READ and WRITE move each lane's heads on by the number of lanes. So a warp where every lane agrees copies 32 consecutive codons in one step, and a COPY loop needs a 32nd of the iterations. SETH and GETH keep the offset (SETH puts lane L's head at AX + L).
- The alternative, plain per-lane heads, makes every lane compute its own offset: correct, but a much longer road for random code to find.
- *Recommendation:* offset heads.

**C3. Telling lanes apart.** All lanes start alike except their heads (C2). SENSE gains two readings: my lane number, and the lanes in my warp.
- Lanes then diverge only when they read different data (READ at their own heads) or their lane number, as on a real GPU.
- *Recommendation:* as above.

**C4. Two forms of life (Damian).** A body is born on a CPU (one CPU, as now) or on a GPU (one warp). Both are full forms of life.
- *Daughters* take the form of whatever divided them: a CPU's DIVIDE gives a CPU-born daughter, a warp's DIVIDE a GPU-born one. So a lineage keeps its form, and code adapted to lanes stays on lanes.
- *Either resource will do:* a daughter who holds none of her form's resource (no millicores for a CPU-born daughter, no GPU units for a GPU-born one) but some of the other is born in the other form. With neither, she dies, as now.
- *Sparks:* `spark.gpu_grant` (default 0, so sparks are as now) gives each spark that many GPU units from the free pool. A spark that got both millicores and GPU units starts on the GPU with chance `spark.gpu_born` (default 0.5); one that got only one kind starts on that. With warps on, a spark needs either pool to have units, so CPU-barren, GPU-rich nodes still get sparks.
- *Arrivals* keep the form they left with: it travels in the cargo, with the same fallback.
- *Launching:* the GPU verb's BX 20 (new) starts a warp at the code after the template that follows it, found as JMP finds one. Every lane starts with the launcher's registers and heads (lane L's heads L past), like a kernel launch with its arguments. So a CPU can set up a copy and hand it to a warp. Either form can launch; the launcher carries on. A body holds at most `warp.max` warps (default 4).
- *ENGULF* takes the victim's warps along with its CPUs, up to `warp.max`.
- *Upkeep:* a body pays memory upkeep from its CPU instructions, as now; whatever its CPUs can't cover comes from its warp capacity, at the cost of a one-lane step per instruction. So GPU-born bodies pay for their memory too.
- The form is recorded in a field that is spare today, so worlds without warps hash exactly as before.
- *Recommendation:* as above.

**C5. A warp's world is its own body (new: shaped by the real GPU).** Lanes fetch, read, write and search for templates only inside their own body's blocks. Reads of waste or of other bodies' code fail, so warps don't salvage remnants (CPUs still do).
- *Why:* each body's warps then depend on nothing but the body's own memory. So on the GPU every body runs independently and in parallel, and only warp bodies' memory moves to and from the device, not the whole world's soups. A real kernel works on its own buffers in the same way.
- The alternative, warps reading anywhere, means mirroring every node's whole soup on the GPU, kept in step with every CPU write, every tick.
- *Recommendation:* own body only.

**C6. Within a step, lanes read before they write (new).** All lanes doing a step read their inputs first, then write. When two lanes write the same codon, the highest-numbered lane's value stays.
- This is what simultaneous lanes on real hardware can do deterministically. The reference does exactly the same, so the GPU and the reference agree.
- *Recommendation:* as above.

**C7. World-changing instructions wait for the host (replaces "once per step").** The world-changing instructions are ALLOC, RELEASE, DIVIDE, ENGULF, INJECT, DISK, EMIT and LISTEN, plus the GPU verb's NPU ops and launch, and Tierra's mal and divide.
- When a warp reaches one, its turn for the tick ends there. Right after the GPU phase, the host (on the CPU) runs the instruction once, with the lowest lane's registers. Every lane at that instruction gets the result (registers and error flag), and they carry on next tick. So a world change costs a GPU program a round trip, as host calls do on a real GPU.
- The host runs these in the node's schedule order, then warp by warp, so the order is fixed.
- They cost what they cost on a CPU, paid from the warp's capacity at the serial rate.
- Block ops (BX 0–15) fail in a warp: the warp *is* the GPU.
- `warp.rounds` (default 1) allows extra GPU rounds in a tick for warps that waited, at the cost of another trip to the GPU each.
- *Recommendation:* as above, with one round.

**C8. What a warp costs, and the economics.** Each GPU unit gives `warp.capacity` work a tick (default 256), at its heat-adjusted speed. A warp step costs `warp.step_cost` (16) plus `warp.lane_cost` (1) for each lane it runs. A step fetches its instruction once for all its lanes. Block ops and warps draw on the same units: whatever a body's block ops used this tick, its warps get the rest. A body's running warps split its capacity equally, as CPUs split instructions.

On a node with 2,000 millicores and 400 GPU units (instructions a tick):

| run on | lanes agreeing | instructions a tick |
|---|---:|---:|
| the CPU (all millicores) | — | 50,000 |
| the GPU (all units), serial code | 1 | about 6,000 |
| the GPU, lanes agreeing | 32 | about 68,000 |

- A GPU running serial code is a poor CPU. One running lane-parallel code beats the CPU. Selection decides which code goes where.
- Warp steps heat the GPU units like block ops, and the leftover fraction carries to the next tick, as a CPU's does.
- *Real resources (as 7b):* `budget.gpu` also sizes `warp.capacity`, from a start-up calibration of how fast the real GPU runs warp steps, so a world of busy warps keeps the GPU about that busy.
- *Recommendation:* these defaults. They are settings, to tune with the rest.

**C9. Randomness and hazards in warps (new).** The CPU draws its hazards from the node's single random stream, in order, which lanes running at once can't share. So in a warp, each draw is keyed by (seed, node, tick, body, warp, step, lane) instead. Copy errors, flaws, salvage and RAND work this way, with the same rates as on a CPU.
- A hot page's chance of refusing a write is turned into a whole-number threshold on the CPU before the GPU phase.
- So everything the GPU does is whole-number arithmetic and bit-exact on any GPU.
- SENSE in a warp reads values taken just before the GPU phase. Nothing in the world changes during it anyway (C7).
- *Recommendation:* as above.

**C10. Warps on the real GPU (replaces "later").** A Direct3D 12 compute kernel runs every warp in the world each tick: one thread group per body, one thread per lane, the body's warps one after another.
- *One source, two compilers:* the warp interpreter is written once in the plain subset that is both C++ and HLSL (functions, 32-bit integers, no pointers). It compiles as C++ for the CPU reference and as HLSL for the GPU, so the two can't drift.
- *The tick with warps:*
  1. every node's first half on the threads (sparks, CPUs);
  2. the warp phase: all warps on the GPU, in dispatches kept short so the desktop stays responsive;
  3. every node's second half on the threads (host instructions, heat, hazards, deaths);
  4. the barrier, as now.
- *Data:*
  - Lane state lives on the GPU between ticks.
  - Each tick, warp bodies' memory goes up (codons, ownership, page thresholds), with the SENSE values, budgets and host results.
  - Coming back are what warps wrote, plus each warp's status: lanes halted, the host instruction it waits on, work used, and counters.
- *Device choice:* `warp.device = gpu | cpu` (default the GPU when there is a hardware one). The hash is the same either way, and tests check that.
- *Video memory:* buffers grow with the warp count, within the GPU's free video memory, checked like 7b's RAM and disk.
- *A device error:* lane state lives on the GPU, so a driver reset or a lost device stops the run with a message. It resumes exactly from its last snapshot on either device. Snapshots read lane state back from the GPU.
- *Profile-driven, after measuring:* these change speed only, never results:
  - keeping warp bodies' memory on the GPU, sending only what the CPU side changed;
  - overlapping the GPU phase with other nodes' CPU work;
  - hashing on the GPU.

  Each is done only if `--profile` shows it is where the time goes.
- *Recommendation:* as above.

**C11. The death rule.** A body dies when it has neither a running CPU nor a running warp. A warp runs while it has a lane that hasn't halted and the body holds GPU units. A lane halts after K consecutive errors, like a CPU, and a warp ends when all its lanes have halted.
- *Recommendation:* as above.

**C12. Determinism.**
- Warps and their lanes are world state: hashed and snapshotted.
- Gates:
  - with `gpu.warps = false`, every hash is Phase 7's;
  - with warps on, the same hash at 1, 4 and 20 threads and under shuffled dispatch;
  - the same hash on the GPU and on the reference;
  - snapshots and replays exact, including a run snapshotted on one device and resumed on the other.
- A differential test runs random warps on the GPU and on the reference side by side and compares every lane and every codon every tick.
- *Recommendation:* as above.

**C13. Detection: the first GPU-born replicator.** Lane writes count as GPU writes, so warp copying shows up in `gpu_replicator` as block ops do. A new milestone, `gpu_native`, is the first gated lineage whose member, a GPU-born body, made an inheriting daughter. `result.txt` and the harness record it.
- *Recommendation:* as above.

**C14. Seeing it.**
- *Inspector:* a body's form and warps, with each lane's IP and registers, and which lanes ran on the last step.
- *Map:* colour by GPU lanes running.
- *Charts:* new `stats.csv` columns: warps, warp steps, lane instructions, lanes agreeing on average, GPU-born bodies.
- *Profile:* `--profile` adds the warp phase, uploads and downloads.
- *Preset:* `configs/gpu_world.cfg`, a landscape whose GPU hotspots sit in CPU-barren regions, with `spark.gpu_grant` on.
- *Recommendation:* as above.

## 3. Two steps
As Phase 7, built in two steps, each tested and documented before the next:
- **8a, warps and the two forms, on the reference:** C1–C9, C11–C13. Every rule is settled and tested here, on the warp interpreter compiled as C++.
- **8b, the real GPU:** C10 and C14. The same interpreter compiled as HLSL, the tick split, data movement, device handling, the `budget.gpu` calibration, the differential test and profiling.

## 4. What gets built
- **Core:**
  - warps and lanes in the soup (world state), and the body's form;
  - the warp interpreter (one source, C++ and HLSL), with the min-IP lockstep, offset heads, read-then-write steps, host instructions and keyed random draws;
  - births in both forms, `spark.gpu_grant` and `spark.gpu_born`, the death rule, costs, upkeep and heat;
  - the SENSE readings, `gpu_native`, the stats columns, snapshots and the hash;
  - the GPU backend for warps, and the tick split.
- **App:** `result.txt` and harness columns; the inspector and map; `configs/gpu_world.cfg`; harness arms `warps` and `gpu_world`.

## 5. Evidence
- `verify.ps1` green, covering:
  - the lockstep rule (agreeing lanes in one step, divergence in turns, merging at a loop head);
  - offset heads (a 32-lane COPY copies 32 codons);
  - read-then-write and the highest lane winning;
  - own-body memory;
  - host instructions waiting a tick;
  - launches, `warp.max`, block ops failing in a warp;
  - both forms of birth with the fallback, sparks, arrivals and ENGULF;
  - the death rule, costs, upkeep and heat;
  - a hand-written GPU-born replicator that copies itself in about a 32nd of the CPU's COPY steps on a node with no CPU;
  - `gpu_native`;
  - the determinism gates and the differential test;
  - Phase 7's hashes unchanged with warps off.
- Measured:
  - the replicator's speed on a CPU against on a warp;
  - the warp phase on the GPU against the reference at 1,000 nodes;
  - transfer volume and a tick profile.
- A short paired check: `gpu_world` with warps on against off. Does life take the CPU-barren, GPU-rich regions, and does a GPU-born replicator appear?

## 6. Docs
- design.md: a new 6.14 (warps and the two forms), 6.2 and 6.4 #1 (the instruction set and the death rule on warps), 6.11 (warps on the real GPU), 9.1 (built), the status line.
- README: warps, `warp.device`, and the preset.
