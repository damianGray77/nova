# Phase 1b plan: the abiogenesis test

> **DRAFT, revision 3, awaiting Damian's approval. Nothing in it is implemented.** The engine on this branch is still Phase 1a (`2ef4571`).
>
> - **Source of truth:** `docs/design.md`. Phase 1b's scope and gate are in 9.1.
> - **This file holds:** the plan, the nine design calls (D1–D9) with recommendations and where each stands after Damian's feedback, one new open call (D10), and the pilot findings that shaped them.
> - **Pilot code** (throwaway, reproduces every number here): `pilots/phase1b/`.
> - **Written by** session 1672, 2026-11-05. Memory DB notes 2879–2886 hold the same findings as they happened.

## 0. Scope and gate (design.md 9.1)
- **Setup:** one node, a sparks-only start, Nova's rules, GPU off.
- **Paired controls**, each run as separate processes:
  - nearest-to-self selector numbering vs a shuffled control;
  - the doubled codon table vs a plain one;
  - single vs doubled INJECT/ENGULF.
- **Spark tuning.**
- **Self-replicator detection from the data side:** genebank, genotype lineage, a compressibility estimate, and CSV output.
- **A compute budget, and a fallback** if no replicator appears within it.
- **Gate:** the first self-replicator arising from random code, detected, logged, and reproducible from its seed through the replay log.

## 1. Pilot findings
Every number below comes from single-node runs of `pilots/phase1b/diag.exe` on the bench node:
- 65,536 codons, 4,000 millicores, 25,000 instructions per core per tick;
- sparks: 400 at tick 0, 4 per tick, a 10-millicore grant, 8–64 codons long.

Most findings rest on one or two seeds, so treat them as directional. The `run.ps1` set name is given for each.

### 1.1 Under the Phase 1a rules a random soup clogs (`clog`)
- **Memory fills:** free memory is 0.2% at tick 100, 0.0% at 1k, and 1.4% at 20k.
- **CPU never frees up:** the free CPU pool sits at 0, and sparks fire about 0.03–0.04 times per tick instead of 4.
- **Cause: leaked ALLOC blocks, not big claims.**
  - At tick 1,000, the bodies' own code accounts for 4,656 codons, while other blocks account for 60,880 (93%).
  - Those blocks are mostly 16–63 codons, the size SENSE 0 returns.
  - Single bodies hold up to 6,104 blocks.
  - Random loops run ALLOC on every pass and never DIVIDE or RELEASE, so each pass strands a block.
- This is the "revisit if a node clogs" case in design.md 6.4 #20.

### 1.2 Bigger nodes don't help (`clog`)
With the same CPU, soups of 262,144 codons (4×) and 1,048,576 codons (16×) are both 0.0% free by tick 1,000. The leak grows with instructions executed, not with memory, so a bigger node buys only tens of ticks.

### 1.3 Hard ALLOC limits (revisions 1–2; superseded by D1) (`alloc`)
| rule | free memory at 20k | births per tick | seeded replicator* |
|---|---:|---:|---|
| Phase 1a (no limit) | ~0% | ~1 | 150–219 copies at t=100, extinct by 5k, memory full |
| one pending block per body | 67% | ~12 | — |
| + ALLOC fails above 3× own size | 73% | ~16 | 8 copies at t=10, 0 at t=100 |
| one pending block per CPU + 3× | 74% | ~9 | 117–137 at t=100, extinct by 5k; memory refills to ~97% |

\* The seeded replicator in this table and in 1.4–1.9 is `SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE` with a 10-millicore grant (see 1.5).

Damian rejected hard limits. Growth and reproduction should be allowed, but they should cost resources ("unfettered growth just = cancer"). RELEASE stays a plain memory operation, and anything like aborting a daughter should emerge.

### 1.4 Memory upkeep: linear vs square-cube (`upkeep`)
**Rule.** Each tick, before it runs, a body pays u · m · (m/64)^(e−1) instructions, where m = all codons it owns. Any unpaid amount carries over as debt and counts as work, so it heats the body.

| rule | free memory @1k / 10k / 20k / 50k | biggest body |
|---|---|---:|
| linear, u = 0.5 | 0.4% at 1k (clogged) | — |
| linear, u = 1 | 27.5 / 0.1 / 0.0 / — | — |
| linear, u = 2 | 61.6 / 47.0 / 30.6 / 22.1 (still sliding) | 5,789 |
| square-cube (e = 4/3), u = 0.5 | 59.5 / 17.8 / 8.0 / 0.3 | — |
| square-cube, u = 1 | 63.6 / 45.0 / 38.0 / 29.6 | 2,339 |
| **square-cube, u = 2** | **64.6 / 56.6 / 53.5 / 51.2 (nearly flat)** | **2,087** |
| e = 1.5, u = 1 | 68.9 / 48.8 / 40.9 / 32.8 | 2,016 |

- **Why linear fails:** it lets a body hold memory in proportion to its CPU, so composites that gather CPU by engulfing keep growing.
- **Why square-cube works:** it gives diminishing returns. 10× the CPU buys only about 5.6× the memory.

### 1.5 The share arithmetic, and the minimal replicator (`share`)
- **The rule:** a daughter gets floor(units × CX/64) of each of the parent's non-memory holdings.
- **The 6.2 sketch can't make a second generation:** `… DJNZ NOP0 INC DIVIDE` gives the daughter of a 10-millicore spark floor(10/64) = 0. She dies the tick she's born. Seeded: 0 living copies at every checkpoint.
- **A same-length variant can:** `SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE` gives a share of its own length/64. At a grant of 10 that's a 1-millicore daughter, which then makes a copy of her own. It's 8 codons (7 core plus DJNZ), about 2^-40 per random 8-codon window.

### 1.6 Seeded replicators die from CPU starvation, not predation (`surface`, square-cube u = 2)
- **How it spreads:** mostly by being *engulfed*. Its CPU keeps running inside the eater and hands each daughter a share of the *eater's* CPU.
- **It always goes extinct:**
  - bare: 6–17 copies at t=100, 0 by 5k;
  - with a 1- or 3-NOP surface (immune to blind ENGULF): 3–4 at t=100, 0 by 5–10k.

  The surface didn't help; it lost the being-eaten route.
- **Cause:**
  - The founder's 10 millicores split into 1-millicore daughters, whose own daughters get 0.
  - The free pool is ~0, so nothing can be claimed.
  - **No lineage can outgrow its founding grant.**
  - **Emergence is not the bottleneck; persistence is.**

### 1.7 Predation works as income (`predation`, square-cube u = 2)
- **The test replicator:** `NOP0 ENGULF SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE`, which runs a blind ENGULF every cycle and has a surface so it doesn't eat its own kin.
- **It gained CPU:** the lineage's total grew from 10 to **57–70 millicores** (richest body 23–24) by t=100–1,000.
- **It still went extinct by 10k.** Likely causes:
  - it can't digest prey, because RELEASE frees only the latest ALLOC block, so prey memory adds to its square-cube upkeep for good;
  - the prey's CPUs keep running inside it.
- **Without the surface:** 0 copies. It eats its own daughters, which are the nearest surfaceless bodies.

### 1.8 Sparks that take only leftovers don't free CPU (`predation`)
Firing sparks *after* the bodies run, so life gets first claim on freed CPU, leaves the average pool at ~0 (26 → 0.7 → 5.2 → 1.0 → 0.2 → 0.0 over the checkpoints). Random programs claim freed units with ALLOC BX=1 before sparks would.

### 1.9 CPU wear makes free CPU exist, and a consumer can boom (`cpuwear`, square-cube u = 2)
- **Wear:** each held millicore fails back into the free pool with probability f per tick, a hazard like bit rot. CPU becomes a flow that has to keep being claimed.
- **Random soup at f = 1e-3:**
  - sparks last: average pool 14–29 millicores (vs ~0); 25,719 sparks over 50k ticks (vs 1,332 without wear); memory 65–95% free;
  - sparks first: average pool ~8; 64,775 sparks; memory ~46–58% free.
- **The CPU-claiming replicator** (15 codons, claims "memory I own" millicores each cycle):
  `ZERO INC ROTB SENSE ALLOC ZERO ROTB SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE`
  - **Seed 9, f = 1e-3, sparks last: boom then bust.**
    - t=100: 126 exact / 161 similar copies, holding **405 millicores** (40× its grant).
    - t=1,000: 1,358 bodies in the soup.
    - Memory went down to 18.6% free, then the lineage was **extinct by 5k** and memory recovered.
  - **Seed 7: 0 copies by t=100 in every variant** (f = 0, 3e-4, 1e-3; sparks first or last). It died early for a reason not yet diagnosed.
  - **The bare SENSE-share replicator at f = 1e-3:** 5 copies at t=100, 0 by 1k (it never claims, so wear only drains it).
- **Reading:** "consumption of free resources" (Damian) works once there is free CPU to consume. So far it's boom-and-bust.

## 2. Design calls
**D1. Memory: square-cube upkeep, no hard limits.** *Revision 3, awaiting approval.*
- **Rule:** the 1.4 rule with `mem.upkeep = 2` and `mem.upkeep_exp = 4/3` as defaults (both knobs). `mem.upkeep = 0` reproduces Phase 1a, and its hashes.
  - Exponent 4/3 is the square-cube law: intake ∝ size^(2/3) while demand ∝ size, so cost relative to intake grows ∝ size^(1/3). This was Damian's suggestion.
- **Dropped from earlier revisions:** the one-pending-block rule and the 3× size cap. Growth, several daughters at once, and RELEASE all stay as in Phase 1a, but now cost resources.
- **What it costs, for a 10-millicore body:**
  - an 8-codon replicator mid-copy pays ~8% of its CPU;
  - a 36-codon spark pays 24%;
  - a 64-codon spark pays 51%;
  - the most it can hold is ~105 codons.

  So spark grant and max length are coupled.
- **Giant claims self-destruct.** A 10-millicore body that grabs 30k codons owes ~466k instructions the next tick, overheats and dies, and the memory comes back.
- **Genome doubling stays emergent.** There is no gene-duplication mutation; size changes only when a program's own code allocates and copies more than its length (the read head wraps, giving a tandem duplicate).
- **Caveats, for later:**
  - Charging on a body's *total* memory makes an engulf-composite pay more than its parts would separately, the opposite of how multicellularity escapes the square-cube law. A later knob could measure size per CPU instead.
  - Eaters can't digest (RELEASE frees only the latest block).

**D2. A self-replicator is a *lineage*, not an exact genotype.** *Revision 2, after Damian's feedback: "a daughter shouldn't be an exact copy… healthy daughters survive regardless of exactness".* The rules never check exactness; this is only the detector.
- **Inheritance link:** a DIVIDE whose daughter matches the dividing CPU's own block on at least `detect.similarity` of positions (default 0.75; chance level is ~1.6%), counted as same-offset matches / the longer length.
  - It separates "she inherited her parent's code" from "a random block was budded off"; random bodies bud about 12 times per tick.
- **Depth:** generations of inheritance links. Mutants are full members of the lineage.
- **Gate:** a lineage in which an inheriting daughter has herself produced an inheriting daughter (depth ≥ 2), with ≥ 2 members alive at once.
- **Exact genotypes** are tracked only in the genebank, to show diversity within a lineage.
- **Detector controls:**
  - the INC-share sketch at grant 10 must not fire;
  - the SENSE-share variant must fire;
  - ALLOC + DIVIDE without COPY must not fire;
  - random budding must not fire.

**D3 + D5. Budget by time; continuous mode stops on Esc.** *Revision 2, per Damian.*
- **Stopping:**
  - `run.seconds` (0 = no limit) stops at the next tick boundary;
  - Esc in a direct run, or in the harness (via stop files for its children), stops gracefully: stats, genebank, result and the replay `end` record are all written;
  - replay stays exact because the log records the end tick.
- **Machine use:** 75% of the machine for *this* simulation (not a standing preference), i.e. 15 concurrent single-node processes at below-normal priority.
- **First experiment:** baseline × 6 seeds plus each control × 3 seeds, `-Hours 12` (0 = continuous). That's ~5.6e11 instructions per trial and ~8.4e12 in total.
- **A trial that runs out of time** without a replicator is recorded as a miss, with its instructions counted. What follows a miss (a seeded start or a rule change) is Damian's call.

**D4. Spark settings are knobs.** *Per Damian.* spark.grant, min_len and max_len become live like spark.rate (logged as replay knob events), and the harness accepts spark overrides per experiment. Defaults: 400 / 4 / 10 / 8–64. There's no calibration sweep up front.

**D6. Shuffled-selector control.** *Explained to Damian; awaiting his confirmation.*
- **Why it matters:** BX picks a verb's variant, and registers start at 0. "Nearest to self first" makes SENSE 0 = my size, ALLOC 0 = memory and DIVIDE 0 = here, so a minimal replicator never has to set BX.
- **The control:** each trial draws its own scrambled numbering from its seed.
  - It covers SENSE (8 variants); ALLOC/RELEASE (4, sharing one permutation); SETH/GETH (2); and DIVIDE ("here" moves to 1 of 4 slots, and the rest fail on one node).
  - The self variant never stays at 0, so every trial actually tests something.
- **Settings:** `isa.selectors = nearest|shuffled` and `isa.selector_seed`. The drawn mapping is printed and logged.

**D7. Plain-64 table.** *Approved.* The 16 new instructions are NOPX, JMPF, JMPB, IFEQ, IFGT, MOD, MIN, MAX, DUP, DROP, INCA, DECA, INCB, DECB, ZEROA and ZEROB.
- CLRERR is dropped: every instruction already sets the error flag, so it would be a no-op.
- Layout: codon 2k = core k (unchanged), codon 2k+1 = its new partner. Codons 32–63 are unchanged.

**D8. Doubled INJECT/ENGULF.** *Approved ("Sure").* ENGULF also takes codon 58 and INJECT 59, the EMIT/LISTEN slots, which always fail until Phase 4. Damian is fine with failing codons as junk generally.

**D9. Arms and pairing.** *Explained to Damian.*
- **Arms:** baseline, shuffled, plain and inv2. Each changes one thing from the baseline.
- **Pairing:** every arm uses the same seeds (seed s starts from the same random soup bytes).
- **Process layout:** one node per process, each with its own replay log.
- **Per-arm rate:** λ = detections / total instructions, with misses included. It's the maximum-likelihood rate for exponential waiting times with censoring.

**D10 (new, open). CPU turnover.** *Raised by Damian: "consumption should also be of free resources, not just predation".*
- **The finding:** the mechanism exists (ALLOC BX=1 claims from the free pool), but CPU has no holding cost. It's a stock that long-lived random bodies hold forever, so the pool is ~0 (1.6, 1.8). Memory had the same problem until D1.
- **Options, each a knob:**
  - (a) **CPU wear** (`cpu.wear`, a per-millicore failure rate per tick). This is "nothing grants permanence" applied to CPU. It's the only mechanism tried that makes free CPU exist (1.9).
  - (b) **Spark timing** (`spark.timing = first|last`). "last" lets life claim before chemistry. It doesn't help on its own (1.8), but with wear it leaves the pool fuller (avg ~14–29 vs ~8).
- **Tentative recommendation:** build both as knobs, and default the abiogenesis runs to `cpu.wear = 1e-3` with `spark.timing = last`. Decide only after a multi-seed sweep: the evidence is one booming seed and one unexplained early death.
- **Not gating:** the Phase 1b gate (emergence) doesn't depend on D10; persistence does.

## 3. What gets built (once approved)
- **Rules:**
  - memory upkeep (D1);
  - CPU wear and spark timing (D10, if approved);
  - the `nova_v1_plain` table (16 new Ops) and `nova_v1_inv2`;
  - the selector map (D6).
- **Detection** (`lineage.h/.cpp`): pure observation, so the world hash is identical with detection on or off.
  - Per-block tags: lineage id, depth, birth tick, genotype.
  - A lineage table and the gate detector.
  - Events gathered at the barrier in node order.
- **Genebank:** Tierra-style names (`0008aab`), parent genotype, the genome as op names, written to `genebank.txt`.
- **Compressibility:** a hand-rolled greedy LZ77 bit-cost estimate per stats window.
- **CSV columns:** links, max_depth, lineages, top_lineage, genotypes_in_top, compress.
- **Replay log v2:** `event` records, re-checked by `nova replay`. v1 logs still load.
- **Run:**
  - `--set key=value`;
  - `run.seconds`;
  - Esc and stop-file stops;
  - `run.after_replicator`;
  - `result.txt`.
- **Harness:** `tools/abio.ps1`:
  - a job queue over arms × seeds;
  - `-Parallel 15`, below-normal priority;
  - `-Hours` (0 = until Esc), with Esc stopping everything;
  - spark and other overrides;
  - resumable;
  - `summary.csv` with λ per arm.

## 4. Evidence
- **`verify.ps1` green**, covering:
  - the detector controls (D2);
  - detection on/off hash identity;
  - compressibility (random ≈ 1, repetitive ≪ 1);
  - the selector maps;
  - plain-64 semantics;
  - upkeep (debt, heat, the death of an over-extended body, and the Phase 1a hash at upkeep 0);
  - the new default hash pinned;
  - event replay at 1 and 20 threads.
- **Manual:**
  - a harness smoke test, including an Esc stop and a replay of the stopped run;
  - the first experiment;
  - a gate replay of the first baseline replicator, with a case report (disassembly, mechanism, lineage);
  - a bench before and after.

## 5. Docs
- design.md:
  - a new 6.6 (Phase 1b);
  - 6.2 (plain-64 and the share finding);
  - 6.3 (spark knobs);
  - 6.5 (upkeep, plus wear if adopted);
  - 7.1 I (compute used);
  - 9.1 and the status line.
- README.
- Memory DB: completed-work memory.

## 6. Out of scope
- GPU, dispersal and the UI.
- Snapshot-resume of detector state.
- The ambient heat leak.
- Digestion (freeing engulfed memory) and size-per-CPU upkeep (D1 caveats).

## 7. Resuming (home rig)
- **Branch** `wip/phase-1b`, based on `main` at `2ef4571`.
- **Next step:** Damian approves or amends D1, D2–D6, D9 and D10. Then implementation starts, using the plan-then-build workflow (memory 2206).
- **To reproduce section 1:**
  - `.\pilots\phase1b\build.ps1`
  - then `.\pilots\phase1b\run.ps1 -Set <name>`
- **Context:** memory DB notes 2879–2886, plus the hand-over note that points at this branch.
