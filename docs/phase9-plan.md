# Phase 9 plan: costs measured on the real machine

> **Revision 2, APPROVED by Damian (2026-11-06). BUILT (2026-11-06) in two steps, 9a and 9b; tuning after real stress tests (Damian).** What was built is in design.md 6.15. Settled while building 9b:
> - **A warp's latency is its own clock:** each step takes its instruction's measured time on a GPU lane, and a warp has a tick of time a tick (built into the shared kernel). Throughput: a GPU unit is one shader lane, and a step fills the warp's whole width (lanes that branched away still hold their slots).
> - **A GPU lane is measured natively** (HLSL microbenchmarks on the GPU's clock); the devices' start-up and share are measured through Nova's own backends, as the in-world ops use them. The NPU's multiply-add is DirectML's float16 graphs, so it is slow (0.45 G a second).
> - **Async mode keeps `accel.latency`** for results: the async barrier gives every op one latency, so measured per-device latencies would need a redesign.
> - **Searches on a warp aren't counted** (each lane searches on its own).
>
> Settled while building 9a:
> - **Heat with long instructions:** one instruction can now run far past its tick (a kernel call is about 2,000 adds), so heat sees at most a full tick of work in the tick that ran over, and the ticks that pay the debt off count as busy. Without that, a small body making a kernel call late in a tick read as 260% busy and overheated.
> - **One disk:** DISK costs what the home rig's drive (E:) costs, wherever Nova keeps the disk tier, so `disk.path` still changes no result (C5 had said "main memory" when the tier is in RAM).
> - **An action's instruction is the one it started**, whatever has since been written over it, and an action with a gone target ends at once.
> - **Warps' host instructions stayed instant until 9b**, with their Phase 8 costs counted in adds.
>
> **Revision 2's changes from revision 1, both Damian's:**
> 1. **One table, measured once, outside the simulation** (replaces measuring at start-up: C8, C9). Nova only ever runs on his machine, and measuring every run would bring in the Windows scheduler and the rest, making runs harder to replay. So `nova bench costs` measures the machine once, and its numbers go into the source as a fixed, named table (`home_v1`).
> 2. **Starting the GPU costs more than each op run in parallel** (C6). Spinning up the GPU for one op takes far longer than each op among many run side by side. So a body's GPU work in a tick is one batch run in parallel: the batch pays the start-up once, and each op adds only its share, both measured.
>
> **Revision 1:**
>
> - **His direction (2026-11-06):** running Nova on real time would be the most realistic (programs developing themselves on real architecture), but it is a big rewrite and gives up determinism. So for now: build a table of what each instruction costs on the CPU, the GPU, the NPU, memory and I/O, measured on the real machine, and run the ticked world by it.
> - **And actions that take time (Damian, the same day):** once some actions last longer than a tick, each has a start, a time acting and an end, as distinct events. That gives programs time to respond, so defences against being eaten can evolve instead of every attack simply happening. Built as C7.
> - **ENGULF by size (Damian):** ENGULF isn't a real CPU instruction, so it has no measured time. It takes time in proportion to the prey's size: big programs take longer to eat than cell-sized ones (C7).
> - **Why:** today every CPU instruction costs 1, whatever it does, and the costs of the GPU, NPU and disk are set by hand. On real hardware a divide costs more than an add, a GPU is slow at serial work but wide, and a round trip to the NPU or a disk takes ages. Selection should feel those differences.
> - **Source of truth:** `docs/design.md`: 6.2 (the instruction set), 6.5 (costs and debt), 6.7–6.8 and 6.11 (the GPU and NPU), 6.13 (real resources, `budget.*`), 6.14 (warps).
> - **Long runs:** the 12-hour run waits for Damian's go-ahead.

## 0. Scope
- **A cost table:** what every instruction costs on every device, in world time. Today's costs become one such table (`classic`); a table measured on this machine is the other.
- **Measuring (revision 2):** short benchmarks outside the simulation (`nova bench costs`) time the real CPU, memory, disk, GPU and NPU once. Their numbers become a fixed table in the source, so every run uses the same one.
- **World time:** a tick gets a length in world time, and every resource gives a body time a tick, not instructions.
- **Actions that take time:** a slow action, and any action on another body, is started, carries on for its ticks, and ends, with a check at the end. Bodies on the receiving end can sense it and respond meanwhile (C7).

**Off unless asked:** with `cost.table = classic` (the default until the paired check) every hash is exactly Phase 8's.

**Not here:**
- running on wall-clock time (the actor rewrite we discussed);
- re-measuring during a run (could come later, each new value written to the replay log);
- the P-cores and E-cores of the Core Ultra 9 as two kinds of CPU in the world (an idea for later; both get measured);
- time within a tick: bodies still run one after another in a node's tick (there is no time slicer, as decided in 6.1), so an action's time is counted in whole ticks;
- edges: they are the world's geography, and there is no real network to measure.

## 1. What exists
- **CPU costs:** a body's CPUs get `millicores × cpu.instr_per_core / 1000` instructions a tick, less memory upkeep, at their heat-adjusted speed. Every instruction costs 1, except:
  - ENGULF and INJECT: `contact.cost_base` + `contact.cost_per_codon` × codons moved;
  - EMIT and LISTEN: `signal.cost_base` + `signal.cost_per_amp` × amplitude;
  - DISK: `disk.cost_base` + `disk.cost_per_codon` × codons;
  - the GPU verb: `gpu.setup` + `gpu.cpu_per_element` × elements (block ops), or `npu.cpu_per_mac` × multiply-adds (NPU ops).

  An overshoot is debt into the next tick.
- **Device capacity:** `gpu.elements` and `npu.macs` a unit a tick. In `accel.mode = async`, results come back `accel.latency` ticks later.
- **Warps:** each GPU unit gives `warp.capacity` a tick. A step costs `warp.step_cost` + `warp.lane_cost` × lanes, and a host instruction is charged at the serial rate.
- **Calibration:** `budget.gpu` and `budget.npu` already time the real devices at start-up and write `gpu.elements`, `npu.macs` and `warp.capacity` into the config. This phase grows that idea into a whole table.

## 2. Design calls

**C1. What gets measured: the hardware, as if Nova's instructions were each device's own machine code.** Each device's basic operations are timed with small native benchmarks: an add, a multiply, a divide, a load, a jump, a kernel call, a GPU lane's add, a GPU round trip, and so on. Then each Nova instruction is costed as the operations it would be (C4).
- *The alternative:* time Nova's own interpreter, instruction by instruction, on each device. That measures our emulation, not the machine. On the GPU it would mostly measure our kernel's barriers (about 5 µs a warp step today), and every speed-up of Nova's code would change the world.
- *Recommendation:* the hardware. It is what "programs on real architecture" means. Speeding up Nova still changes only how fast the world runs, never what happens in it.

**C2. World time.** Costs are whole picoseconds of world time, so all the arithmetic stays exact on any machine.
- A tick has a length (C3).
- 1,000 millicores of CPU give one tick's length of CPU time a tick (scaled by heat as now). An instruction costs its recipe's time, and an overshoot is debt, as now.
- The GPU, NPU and disk give time in the same currency (C5, C6).
- Memory upkeep stays a rule, not a measured time. It is charged as that many plain instructions' worth of time.
- *Recommendation:* as above.

**C3. How long a tick is.** By default, `cpu.instr_per_core` plain instructions (an add) on one of the machine's P-cores. So today's settings keep their scale: a CPU still runs about as many simple instructions a tick, while a divide, a long search or a disk read now costs what it really costs. `time.tick_ns` sets it outright.
- A tick's length changes only the step size of world time. Every per-tick rate (sparks, hazards, heat, upkeep) keeps its meaning against CPU work, and the devices' costs against each other come from the measurements whatever the tick.
- *Recommendation:* derived from `cpu.instr_per_core`, as above.

**C4. Each instruction is a recipe of measured operations.** Appendix A lists every instruction's recipe. Two examples:
- COPY is a load, a store and a random draw (its copy-error chance);
- JMP is reading its template, scanning the codons it searches, and a jump.

The recipes are config (`cost.recipe.<instruction>`), so they can be tuned like the codon table.
- *The biggest change:* world-changing instructions (ALLOC, RELEASE, DIVIDE, ENGULF, INJECT, DISK) cost a real kernel call, as a program asking an operating system does. That is likely hundreds of plain instructions' worth, not 1 (to be measured).
- *Recommendation:* the recipes in Appendix A.

**C5. What one unit of each resource is, on the real machine.**
- *CPU:* 1,000 millicores = one P-core of this CPU (as now in spirit; now measured).
- *GPU:* one GPU unit = one shader lane of the real GPU (the RTX 5080 has 10,752). So a body holding 32 units can run one 32-lane warp at full speed, and one holding 8 runs it at a quarter.
- *NPU:* one NPU unit = a thousandth of the real NPU's measured throughput.
- *Disk:* the drive at `disk.path` when the disk tier is in files, otherwise main memory, as the machine really holds it.
- *Memory:* codons, as now.

Existing settings like `world.gpus = 400` keep their numbers but get this meaning, so presets may want new values after the paired check.
- *Recommendation:* as above.

**C6. Parallel devices have latency and throughput, both measured.**
- *Warps:* two limits, as on a real GPU.
  - Latency: a warp takes at most as many steps a tick as fit one after another, at its measured step time (the GPU's slower clock, longer delays and the lockstep selection). That is the GPU's slowness at serial work.
  - Throughput: a body's lanes share its units' slice of the GPU's measured lane-operations a second.
  - So one warp can't fill many units; filling them takes many warps, or wide ones.
- *Host instructions:* a warp waiting on one also waits a measured GPU round trip, the real cost of a GPU program asking the CPU for something.
- *Block ops and NPU ops:* a measured round trip plus a measured time per element or multiply-add.
- *Start-up against parallel work (Damian, revision 2):* spinning a device up for one op costs far more than each op among many run side by side. So a body's ops on a device in one tick form one batch, run in parallel:
  - the batch pays the measured start-up once (a lone op dispatched and waited for);
  - each op adds only its measured share when many run at once (many ops in one dispatch, divided by their number);
  - a warp's launch pays the start-up, and its lanes and steps then run inside the kernel.

  So programs that gather their GPU work into batches get more from it, as on a real GPU. (The batch is the body's, not the node's, so the saving goes to the program that earned it.)
- *Recommendation:* as above.

**C7. Actions that take time: a start, a time acting, and an end (Damian).** Most instructions take a tiny slice of a tick and happen at once, as now. Two kinds become *actions in progress*:
- any instruction that takes longer than a whole tick (a DISK read, a big DIVIDE, a block op in `instant` mode). A shorter one that runs past the end of the tick is still debt, as now;
- any instruction that acts on another body (ENGULF, INJECT). These aren't machine instructions, so their time is a world rule, not a measurement (Damian):
  - ENGULF takes time in proportion to the prey's size: `engulf.codons_per_tick` (16) of the prey a tick. A cell-sized prey of 30 codons takes 2 ticks; a body of 1,000 takes 63.
  - INJECT likewise takes the codons it writes at `inject.codons_per_tick` (16).
  - Either takes at least one whole tick, so every body gets a turn to respond, wherever it sits in the schedule. (Bodies run one after another within a tick, so a shorter window would favour bodies late in the order.)
  - The whole prey goes at the end, not bite by bite, so a defence that works saves all of it.

How an action goes:
1. *Start:* it is checked as now: aim, range and resources. Its end tick is set from its measured time. The target is marked with what is acting on it and when that ends.
2. *Acting:* the actor's CPU is busy: it runs nothing else until the end, and its time is spent. Everyone else carries on.
   - The target can sense it: SENSE gains two readings, *ticks until the next action on me ends* (0 for none) and *what it is*.
   - The target can respond with anything it has.
3. *End:* the action is checked again, and lands only if it still holds:
   - the actor is alive;
   - the target is still there and still matches the template it was aimed by;
   - it is still in range;
   - the resources are still held.

   Otherwise it fails, with the error flag, and its time is spent.

So defences need no new instruction. A target can do any of these:
- rewrite the surface the attacker aimed at;
- move or divide away;
- release the targeted part;
- engulf the attacker first.

Each of those is an action with its own time, so races follow: they resolve by end tick, then schedule order, all deterministic.

Further rules:
- An actor that is engulfed or dies mid-action loses the action.
- A body mid-DIVIDE carries its unborn daughter, and can be eaten with her.
- In `async` mode, device results also arrive after their measured round trip in whole ticks (replacing the hand-set `accel.latency`).
- Actions in progress are world state: hashed, snapshotted, in the viewer.
- With `cost.table = classic` every action is instant, as now.
- *Recommendation:* as above.

**C8. One fixed table, measured once (revision 2: Damian).**
- `nova bench costs` runs the benchmarks outside the simulation and prints each operation's time.
- Its numbers go into the source as a named table, `home_v1`, with the machine and date beside them, as the codon table `nova_v1` is.
- `cost.table = classic | home_v1`. `classic` is today's costs, every hash unchanged.
- Runs never measure, so every run, replay and snapshot uses exactly the same numbers. A later re-measure would be a new table (`home_v2`), never a silent change.
- *Recommendation (revision 1, replaced):* measuring at start-up or from a file.

**C9. Measuring well.**
- Each benchmark repeats and takes the median.
- Only machine operations are measured: world rules like ENGULF's pace (C7), upkeep and hazards stay settings.
- The CPU's benchmarks run pinned to a P-core. The E-cores are measured too, for the record (and for the idea in Scope).
- `nova bench costs` warns when the machine looks busy (repeats disagree by more than 10%).
- Measuring takes a few seconds, and runs are measured with the machine otherwise idle.
- *Recommendation:* as above.

**C10. Determinism.** Either table is a fixed set of whole numbers in the source, so the world is deterministic.
- `classic` gives every Phase 8 hash.
- With `home_v1`, the gates are: the same hash at 1, 4 and 20 threads, under shuffled dispatch, and on the GPU and the reference. Snapshots and replays are exact, including a snapshot taken with actions in progress.
- *Recommendation:* as above.

**C11. Seeing it.**
- `nova bench costs` prints the machine's operations, and `nova costs` prints the table in use: each instruction's cost, also in plain instructions (so "DIV = 12 adds" reads at a glance).
- `result.txt` records the table's name.
- `--profile` adds world time a wall second: the world's virtual machine is far bigger than the real one, so it runs slower than real time.
- `stats.csv` adds world time spent by kind (compute, searching, memory, kernel calls, disk, devices). It also counts actions started, landed and failed at their end, by why they failed (the target changed or moved, the actor died, and so on).
- *A new milestone, `defended`:* the first gated lineage whose members made several actions aimed at them fail at their end, by changing what the attacker aimed at, leaving, or eating the attacker first. Like `gpu_native` it goes into `result.txt` and the harness.
- *The viewer:* actions in progress are drawn between actor and target, and the inspector shows a body's action and what is acting on it.
- *Recommendation:* as above.

## 3. Two steps
As Phases 7 and 8, built in two steps, each tested and documented before the next:
- **9a, the table, actions and the CPU side:**
  - C1–C5 and C7–C11 for the CPU, memory and disk;
  - every cost in the code read from the table, with `classic` giving every hash;
  - world time in picoseconds;
  - actions in progress, their SENSE readings, `defended`;
  - the CPU, memory and disk benchmarks in `nova bench costs`, the table `home_v1` and `cost.table`.
- **9b, the GPU and NPU:**
  - C6, with the GPU and NPU benchmarks (HLSL microbenchmarks with the device's own timestamps, and DirectML for the NPU), added to `home_v1`;
  - start-up against a stream, warps' two limits, host round trips, and device latencies in ticks;
  - the profile, presets and the paired check.

## 4. What gets built
- **Core:** the cost tables (`classic`, `home_v1`) and recipes; world time; actions in progress.
- **App:** `nova bench costs` (CPU and memory in C++, disk on the real drive, GPU in HLSL on Direct3D 12, NPU through DirectML), `nova costs`, `cost.table`, the profile line and the stats columns.

No new dependencies: Windows' own headers already in the allowlist (thread affinity and core types come from `windows.h`).

## 5. Evidence
- `verify.ps1` green, covering:
  - `classic` giving every pinned hash;
  - recipes adding up (unit tests on the table);
  - `home_v1`: the determinism gates, snapshots and replays;
  - device latencies in ticks;
  - actions: start, acting and end; the target's SENSE readings; a hand-written prey that rewrites its aimed surface and makes an ENGULF fail; an actor eaten mid-action; races resolved by end tick; `defended`;
  - warps' two limits.
- Measured: the home rig's operations in the docs (Core Ultra 9 285K P-cores and E-cores, RTX 5080, the NPU, the disk), with the spread across 5 runs of `nova bench costs`.
- **A paired check:** `gpu_world` and `landscape` with `classic` against the measured table. Do replicators still appear? Where does life go: CPU against GPU, DISK and the NPU used or abandoned? What new costs does selection work around? Do predators still pay off, and does `defended` appear?

**What may change most (to be measured, not promised):** the NPU and the disk become far slower against the CPU than today (the NPU's round trip measured 3.4 ms in Phase 5: if an add takes about a cycle, that is hundreds of ticks in today's presets). World-changing instructions cost a kernel call. Divides and long searches cost more than adds. Warps get the GPU's real shape: slow in series, wide in parallel. Predation stops being instant, so prey that sense an attack and change in time can survive it: the first defences.

## 6. Docs
- design.md: a new 6.15 (measured costs and actions that take time), 6.2 (instruction costs, the two SENSE readings), 6.14 (warp costs), 9.1 (built), the status line.
- README: `nova bench costs`, `nova costs`, `cost.table`.

## Appendix A. Recipes (first draft)

The operations measured: on the CPU, *alu* (an add), *mul*, *div*, *load*, *store*, *jump* (a jump the hardware can't predict), *scan* (a codon searched), *move* (a codon copied in bulk), *draw* (a random draw), *call* (a kernel call) and *share* (a cache line passing between cores). Then *disk* (a disk access, and a codon read or written), and the devices' *gpu round trip*, *gpu element*, *npu round trip* and *npu multiply-add*.

| instruction | recipe |
|---|---|
| NOP0, NOP1 | alu |
| INC, DEC, ZERO, ROT, ROTB, SWAP, NOT, NEG, SHL, SHR, AND, OR, XOR, ADD, SUB, SETH, GETH | alu |
| MUL / DIV | mul / div |
| IFZ, IFLT, IFNE, IFERR | alu (a predictable branch) |
| JMP, CALL, FIND, and DJNZ / JZ when they jump | load × template length + scan × codons searched + jump |
| DJNZ / JZ not jumping | alu |
| PUSH / POP / RET | store / load / load + jump |
| COPY | load + store + draw |
| READ / WRITE | load / store + draw |
| RAND | draw |
| SENSE | load |
| ALLOC / RELEASE | call + move × codons |
| DIVIDE | call + move × the daughter's codons |
| ENGULF, INJECT | call + scan × codons searched to aim, then an action of the prey's size (ENGULF) or the codons written (INJECT) at 16 codons a tick (C7), not measured |
| EMIT, LISTEN | share |
| DISK | call + disk access + disk codon × codons |
| GPU block ops (BX 0–15) | gpu round trip + gpu element × elements |
| NPU ops (BX 16–19) | npu round trip + npu multiply-add × multiply-adds |
| launch (BX 20) | gpu round trip |
| YIELD | the rest of the tick |

**On a warp,** a step costs its recipe in the GPU's own operations (a lane's add, groupshared or video-memory loads), plus the step's fetch and its lockstep selection. A host instruction costs a GPU round trip, plus its recipe on the host's CPU.
