# Phase 9c plan: loose ends of Phase 9, threads, and two kinds of core

> **9c-b BUILT (2026-11-06), in design.md 6.15 (4d), in two steps.** 9c-b1, threads: a core's tick a thread at most, BX 21 and 22, SENSE 24–27, `cpu.slices`. 9c-b2, E cores: the E pool and per-site shares (`res.ecpu`), `home_v1`'s E column (the means of two bench runs; an E-core's bulk move is 92 ps against 16, in both), each body's time handed out by R3 (`Soup::share_time`), E heat at 0.4, ALLOC and RELEASE BX 5, both kinds through DIVIDE, ENGULF and cargo (with the divider's kind), the inspector and the "E-cores" map colour. Every earlier hash is unchanged; the paired check is in 6.15's 9c-b findings (programs start few threads yet; threads cost about 19% of a tick's wall time, mostly the slices).
>
> **Revision 2 of 9c-b, APPROVED by Damian (2026-11-06) as recommended.** Revision 1 was approved, with `heat.e_gain` = 0.4 unmeasured. Talking through C5 then reshaped 9c-b (Damian, 2026-11-06), all of it in-world behaviour:
> - **Words.** Nova's code calls a thread a "CPU" (registers, instruction pointer, stack, heads). The in-world cores are the millicores. So P and E apply to the millicores.
> - **"A program that owns 2 CPUs should be able to use them as distinct CPU units":** a body's cores are separate processors, each running a thread at most a core at a time. So a program uses two cores by running two threads, and threading can evolve.
> - **No penalty for the wrong kind:** a thread runs on the other kind of core when its own kind has none free. Selection works through speed against heat, as on real P and E cores.
> - **How the simulation spreads its work over the real machine's cores doesn't matter** (the world is the same on any core).
>
> **Revision 1 (C1–C9), approved:**
>
> **9c-a BUILT (2026-11-06):** C1–C3, in design.md 6.15 (4c). A GPU lane's place searched measures 32.7 ns (`gpu_scan`, in `home_v1`). Async batches come home at the next barrier and their results are held until due. A waiting CPU's share goes to its body's other CPUs.
>
> - **His direction (2026-11-06):** address Phase 9's smaller open items. Split the CPU into the machine's two kinds of core: "P cores can be faster while E cores produce less heat."
> - **Not here:** the genebank's growth, the starvation settings and landscape tuning. They wait for the 12-hour run, which comes after stress testing (Damian).
> - **Source of truth:** `docs/design.md` 6.15 (measured costs), 6.11 (the async devices), 6.5 and 4.5 (CPU units and heat).

## 0. Scope
- **Three fixes to Phase 9:**
  - a warp's template searches cost time on its GPU clock (C1);
  - async device results come back after each device's measured round trip, not the hand-set `accel.latency` (C2);
  - a CPU waiting on the disk or a device is idle, not busy (C3).
- **Two kinds of CPU:** P-core units and E-core units in the world, at their measured speeds, with E-cores running cooler (C4–C9).

**Off unless asked:** `classic` is unchanged by C1–C3, and a node with no E-core units is exactly as now. Every pinned hash stays.

## 1. Design calls

**C1. A warp's searches cost time on its clock.** On a CPU, JMP, CALL, FIND and the like pay for every place they search (275 ps each), the template codons they read, and the jump. On a warp today a step costs only its instruction's base GPU time (1.4 ns for a JMP), however far its lanes searched (up to 400 places each way). So GPU code gets long searches free.
- In the kernel each lane searches on its own, and lanes in lockstep wait for the slowest. So the kernel counts each lane's places searched and takes the most. It adds that many times the measured cost of a place searched on a GPU lane, plus the template codons read.
- *New measurement:* `gpu_scan`, a lane's search loop on the GPU's clock, as `nova bench costs` times the CPU's. A GPU lane waits for each load before it can test it, so this is likely tens of ns a place, against the CPU's 0.3.
- *Recommendation:* as above.

**C2. Async results come back after each device's own round trip.** In `accel.mode = async` (Phase 5), block ops don't block the CPU. The world gathers every op issued in a tick into one batch per device, runs it on the real device, and hands results back `accel.latency` ticks later: one number (default 2, at most 64) for every op on both devices. One latency keeps the results in issue order, which keeps the bookkeeping simple and exact. On `home_v1` it's a loophole: a GPU round trip measures 15 ticks and an NPU one about 710, yet async results come back in 2.
- With a timed table each device gets its own latency from the table: `ceil(gpu_start / tick)` and `ceil(npu_start / tick)`. Each device's ops stay in order within their own queue, so the world stays exact.
- Results are collected from the real device when ready and held until due, so a long latency doesn't hold device slots. The 64-tick cap is lifted for timed tables.
- `classic` keeps `accel.latency` as it is.
- *Recommendation:* as above.

**C3. Waiting is idle, not work.** The wait itself stays: a DISK read is synchronous, so the program's next instruction may need the data, and its CPU can't go on until it lands. What's wrong is the accounting: today a waiting CPU's tick counts as work, so its units heat as if computing flat out. On real hardware a thread blocked on I/O leaves its core, which cools or runs another thread.
- A CPU waiting on the disk or on a device (a block op longer than a tick) is idle: its units cool.
- Its share of the body's time that tick goes to the body's other running CPUs, as an OS runs another thread while one waits.
- ENGULF and INJECT stay busy: that's the CPU doing the moving. A kernel call's overrun also stays busy: the CPU really is in the kernel.
- *Recommendation:* as above, with the share passed on.

*C4–C9 below are revision 1's, kept for the record. Revision 2's calls for 9c-b, R1–R8, replace them: see section 1b.*

**C4. Two kinds of CPU unit.** Every millicore is a P-core unit or an E-core unit.
- `cpu.e_share` (default 0, so everything as now) of a node's units are E. It can vary by node like the other resources (`res.ecpu.spread`, `.smooth`, `.against`), so landscapes can have P-rich and E-rich regions.
- 1,000 millicores of either kind is one core of that kind.
- *Recommendation:* as above.

**C5. E-cores are slower, by measurement.** The table gains an E-core column, already measured by `nova bench costs`. Against a P-core an E-core is:
- about 15% slower at adds, loads and stores;
- 60–70% slower at divides, searches and kernel calls;
- 32% slower at random draws.
- Bulk moves read 5.7× slower, but that run was flagged as busy, so it gets measured again.

A body's CPUs run on its P-time first, then its E-time, as an OS schedules a thread on whichever core it holds. Each instruction is charged at the cost for the kind of core it runs on.
- The alternative is binding each CPU to one kind of core. That's simpler to reason about, but a body could hold time on one kind its CPUs can't use.
- *Recommendation:* P-time first, then E-time.

**C6. E-cores run cooler (Damian).** E-core work heats E units at `heat.e_gain` × the P-core's gain. That's a setting, default 0.4, to tune in the stress tests. Nova doesn't measure power: doing so would need Windows' performance-counter API (`pdh.h`), a new header and so your call.
- So selection gets the real trade-off: P is quick but runs hot and throttles; E is slower but can run flat out for longer.
- *Recommendation:* a setting, default 0.4. Measuring power is optional and separate.

**C7. Programs can choose.**
- ALLOC and RELEASE gain a resource: BX 1 stays P-core units, and a new BX picks E-core units (the selector map grows by one).
- SENSE gains *my E units* and *free E units*. The warp image's reading space grows by two words to hold them.
- *Recommendation:* as above.

**C8. Where E units go.** Each follows millicores as they move today:
- sparks draw from both pools in proportion to what's free;
- DIVIDE passes on a share of each kind;
- ENGULF takes both;
- a dispersing daughter's cargo carries both;
- CPU wear and heat release work per unit, as now.
- *Recommendation:* as above.

**C9. Determinism and seeing it.**
- E units are world state, hashed and snapshotted only when `cpu.e_share` > 0, so worlds without them hash as now. The gates are the usual: the same hash at 1, 4 and 20 threads, under shuffled dispatch, and through snapshots.
- Seeing it: `stats.csv` gains E units held and E-time worked. The inspector shows a body's P and E units. The map gains an "E-cores" colour.
- *Recommendation:* as above.

## 1b. Revision 2's calls for 9c-b: cores, threads, P and E

*Words, from here on in the docs and the viewer:* a **core** is 1,000 millicores of one kind; a **thread** is what the code calls a CPU. Code names stay as they are.

**R1. Cores of two kinds.** Every millicore is P or E, and 1,000 of one kind make a core of that kind. A body holding fewer has a fraction of one: a 10-millicore spark has 1% of a core.
- `cpu.e_share` (default 0) of a node's millicores are E. It can vary by node like the other resources (`res.ecpu.spread`, `.smooth`, `.against`).
- *Recommendation:* as above.

**R2. A thread runs on one core at a time (Damian).** A body's cores are separate processors.
- A thread gets at most one core's time a tick.
- With 2 cores and 2 threads, both run at full speed side by side.
- With 2 cores and 1 thread, one core's worth goes unused and cools. That's what makes threads worth having.
- With fewer cores than threads, the threads share them equally: 300 millicores and 2 threads give 0.15 of a core each.
- Today a single thread can use all its body's millicores (2,000 runs it at two cores' speed); this ends that.
- *Recommendation:* as above.

**R3. P and E, with no penalty for the wrong kind (Damian).** Each thread prefers P or E.
- Each tick a body's time is handed out in turn:
  1. each thread first gets up to a core of its preferred kind, shared equally if that kind is over-subscribed;
  2. whatever time of either kind is left goes to threads still short of a core.
- So a thread never sits idle while its body has free core time of the other kind.
- Each instruction is charged at the measured cost for the kind of core it runs on: an E-core is about 15% slower at adds and loads, and 60–70% slower at divides, searches and kernel calls.
- Heat lands on the units that ran it, E at 0.4 × the P gain (approved).
- *Recommendation:* as above.

**R4. Programs start threads and choose their kind.** The codon table is full, so these are new selectors on the GPU verb, beside BX 20's warp launch.
- *BX 21, start a thread* at the code after its template, found as JMP finds one. It gets the starter's registers and heads (a fork with its arguments) and the starter's preferred kind. Costs a kernel call. At most `cpu.max_threads` (8) a body.
- *BX 22, prefer a kind:* this thread prefers P (AX even) or E (AX odd).
- ALLOC and RELEASE gain a selector for E millicores (BX 1 stays P).
- SENSE gains: my threads, my E millicores, free E millicores, and my preferred kind.
- A thread ends when it halts, as a CPU does now. A body dies with no running thread (or warp).
- *Recommendation:* as above.

**R5. Threads take turns within a tick, a slice at a time.** In world time a body's threads on separate cores run side by side. The simulation runs them in turn: today each runs its whole tick before the next starts, so one can only see another's writes from a later tick, or from an earlier thread in the order.
- With `cpu.slices` (default 8) the tick is cut into slices. Each thread runs a slice's worth of its time per turn, in the body's thread order (rotated each tick), so threads can hand data to one another within a tick.
- Deterministic as now, and bodies still run one after another (there's no slicer between bodies, as decided in 6.1).
- *Recommendation:* 8 slices.

**R6. Births and where millicores go.**
- A newborn's thread prefers the kind of the thread that divided her. A spark's thread prefers P.
- E millicores follow millicores as they move today: sparks draw from both pools in proportion to what's free; DIVIDE passes a share of each kind; ENGULF takes both, with the prey's threads; a dispersing daughter's cargo carries both; wear and heat release work per unit.
- *Recommendation:* as above.

**R7. Off unless asked.**
- `cpu.threads = off` (the default) leaves R2, R4 and R5 off: one thread may use all its body's millicores, and BX 21 and 22 fail.
- `cpu.e_share = 0` leaves R1, R3 and R6's E parts off.
- With both off every hash is exactly as now; the presets for stress tests turn them on.
- *Recommendation:* as above.

**R8. Determinism and seeing it.**
- Threads' kinds and E millicores are world state, hashed and snapshotted only when switched on. The gates are the usual: the same hash at 1, 4 and 20 threads, under shuffled dispatch, through snapshots, and on the GPU (warps are untouched).
- *Seeing it:* `stats.csv` gains threads started, threads alive, E millicores held and E-time worked. The inspector shows a body's cores by kind and each thread's kind and share. The map gains an "E-cores" colour.
- *Recommendation:* as above.

## 2. Steps
- **9c-a, the fixes:** C1–C3. **BUILT.**
- **9c-b1, threads:** R2, R4 (BX 21 and 22, the SENSE readings), R5, R7's thread switch, on P cores alone. **BUILT.**
- **9c-b2, E cores:** R1, R3, R6, the rest of R4 (E millicores in ALLOC, RELEASE and SENSE), with the E-core column in `home_v1`. **BUILT.**

## 3. Evidence
- `verify.ps1` green, covering:
  - C1: a warp's searching step costs its slowest lane's places;
  - C2: async results come back after each device's own latency, in order;
  - C3: a waiting CPU cools, and its share goes to its body's other CPU;
  - R2–R5: a thread capped at a core, idle cores cooling, a hand-written program that starts a second thread and runs twice as fast on 2 cores, threads trading data within a tick, kinds handed out with no penalty, E time at E costs and heat;
  - R6: shares through DIVIDE, ENGULF and cargo;
  - every earlier hash unchanged.
- A short paired check: `landscape` with threads on and half its millicores E, against everything off. Do programs start threads, and which kinds do they hold?

## 4. Docs
- design.md 6.15 (C1–C3, and a new part on the two kinds of CPU), section 10, 9.1.
- README.
