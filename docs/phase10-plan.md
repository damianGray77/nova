# Phase 10 plan: vents, decay, bites and death

> **BUILT (2026-11-06), in design.md 6.16, in the four planned steps.** Notes from the build: vents are placed one per `vent.spacing` codons (the plan's "vent.count"); decay is on by default only with a table whose codon 0 is INERT (`nova_v2`); the disk keeps a rarer `disk.rot` for held disk alongside `disk.decay` for fossils; the test suites run on the old defaults through `Config::set_fallback`, so no earlier hash had to be re-pinned. With everything on, warps make the landscape about six times dearer to simulate.
>
> **Revision 1, APPROVED by Damian (2026-11-06) as recommended.** Calls marked *(Damian)* were decided in conversation; the rest follow the recommendations.
>
> - **Why:** the hour-long test (`runs/test1h`; design.md 6.15, 9c-b findings) gave the first replicators on `home_v1`: 180 lineages, one of which (204.1) reached about 300 alive and 7 generations on four nodes before it used up its nodes' millicores. Everywhere else the soup was a stream of animated random waste ("codon zombies") with ENGULF as a sure kill: a blind ENGULF takes the nearest surfaceless body whole.
> - **His direction:** material should come from the world the way a hydrothermal vent supplies it, not packaged as organisms; dead matter should decay toward nothing; ENGULF should be a bite, as good on raw material as on programs, so foraging and predation are on equal footing and predation has to evolve; halted and frozen programs are dead.
> - **Defaults (Damian):** new features are on by default. Hashes don't need to stay compatible until features stabilise and only tuning is left.
> - **Source of truth:** `docs/design.md` 6.3 (sparks), 6.4 and 6.6 (contact, INJECT's criteria), 6.10 (waste, remnants, salvage, rot), 6.15 (measured costs, actions).

## 0. Scope
- **Vents:** points in each node's memory that spray random material into nearby waste, fast and continuously.
- **Sparks:** unchanged in kind (they animate a run of waste with a CPU and millicores), but slower.
- **Decay:** dead matter fades to an inert codon; cosmic rays stay as rare random flips everywhere.
- **Bites:** ENGULF takes a bite of the nearest material, waste or body; a bite can't be bigger than the eater; a partial bite takes a piece and the same share of the prey's power.
- **Death:** halted threads end; a frozen body dies.
- **Defaults:** all of the above on by default (Damian); turning on the older features too is call C8.

## 1. Design calls

**C1. Vents (Damian: material, not radiation; into waste only, at high rates).**
- Each node has `vent.count` vents at random positions in its memory, drawn from the seed for each life of the node.
- Every tick each vent writes `vent.rate` random codons into waste within `vent.radius` codons of it, nearer more often (a triangular falloff). Codons owned by a body are skipped: vents never touch living code.
- A vent writes whole codons (six random bits at once): memory's unit is the codon, and every six-bit value is a codon, so a single random bit written into a decayed codon would only ever make the six codons with one bit set. Fresh material carries no salvage stamp.
- *Recommendation:* as above, with `vent.count` one per 4,096 codons (4 on a 16,384-codon node), `vent.rate` 64 and `vent.radius` 256 to start; the paired check (section 3) settles them. Vents don't heat memory (a possible later addition).

**C2. Sparks: slower (Damian), otherwise as now.** A spark already makes no code: it gives a random run of waste (8–64 codons) a CPU and `spark.grant` millicores, so it animates whatever lies there. That stays. Placement stays uniformly random (Damian), so some sparks land near vents on fresh random material, some far away on decayed matter, and some vents have no spark nearby and just make material.
- *Recommendation:* a quarter of the presets' rate to start (the landscape's `spark.rate` 1 → 0.25), settled by the paired check.

**C3. Decay to an inert codon (Damian: dead matter fades toward zero).** Codon 0 is NOP0 today, a template label, so a soup decayed to zeros would be a sea of labels: every JMP would find a match in dead matter and every corpse would have a surface.
- A new codon table, `nova_v2` (the default): codon 0 becomes INERT. It is not a NOP, so it ends templates and surfaces, and executing it fails, so a thread that runs into dead matter halts after `cpu.halt_after` (8) of them in a row. NOP0 keeps codon 1. Every other codon is as in `nova_v1`.
- *Decay:* each tick every waste codon loses one random set bit with chance `mem.decay`, so dead matter drifts to INERT and loses its function as it goes. A whole corpse is gone in a few half-lives. Living code doesn't decay.
- Disk fossils decay the same way (`disk.decay`, replacing `disk.rot`'s flips).
- *Recommendation:* as above, with `mem.decay` at today's default rot rate to start (about 3 × 10⁻⁴ a codon a tick in the landscape, so a codon's bits last a few thousand ticks).

**C4. Cosmic rays (Damian: rare flips are interesting).** Today's bit rot already is a cosmic ray: a random bit flip in any codon, living or dead, at `hazard.rot`. With decay taking over dead matter, rot becomes the rare event it should be.
- *Recommendation:* keep `hazard.rot` as is, with its default cut to a hundredth of today's.

**C5. ENGULF is a bite (Damian).**
- *Size:* a bite is CX codons, capped at the eater's own size: a body can't eat anything bigger than itself, but it can take a piece of it. CX of 0 or less bites nothing.
- *Target:* the nearest material within `contact.range` of the IP. An aimed bite (a template after ENGULF) goes to the best-matching surface, as today. A blind bite takes the nearest of: a waste codon, a surfaceless body. The grip contest is unchanged: a surface shields a body from blind bites, and aimed bites roll match / length. Raw material has no surface and no grip roll.
- *Waste (foraging):* the bite claims up to its size of waste codons from the nearest waste codon outward, as a new block of the eater with its contents.
- *A body no bigger than the bite* is swallowed whole, as ENGULF does today: everything it holds goes to the eater, and its threads keep running inside the eater (engulf rule 1, kept, Damian), until they halt (C6).
- *A bigger body loses a piece (Damian):* the bite takes its codons from the end of the prey nearest the attacker, the point of contact (Damian), as a new block of the eater with their contents. It takes the same share of the prey's millicores (random units, P and E as they come) and of its GPU and NPU units, as DIVIDE shares holdings; not its disk. A thread running in the bitten code now runs outside its body, fails and halts. If the prey has no running thread or no millicore left it dies (C6); otherwise it limps away.
- *Time:* a bite of waste and a bite of a body cost the same: a kernel call, then its codons at `engulf.codons_per_tick`, at least one tick, as an action with a start, a wait and an end (6.15). Damian suggested slowing ENGULF by half to give prey time to react: `engulf.codons_per_tick` 16 → 8.
- INJECT is unchanged.
- *Recommendation:* as above, with the pace halved.

**C6. Halted and frozen means dead (Damian; not upkeep).**
- A thread that halts ends: it leaves its body at the end of the tick. Today halted threads stay in living bodies, part of why bodies averaged 2.9 threads in the test. A body with no thread left dies, as now.
- *Frozen:* a body none of whose threads has run an instruction for `life.frozen_ticks` ticks dies. A thread waiting on an action (the disk, a device, a bite or an INJECT in progress) isn't frozen. This is an OS's watchdog for a hung process. It also ends bodies stuck in upkeep debt, which today sit frozen holding their memory and millicores until wear removes their last millicore, but upkeep itself is unchanged (the square-cube law stays as tuning).
- *Recommendation:* `life.frozen_ticks` 200.

**C7. Seeing it.**
- `stats.csv`: codons vents wrote, bits decayed, bites of waste and of bodies, bodies swallowed whole, codons bitten, and deaths by cause (halted, frozen, worn out, wiped).
- The memory strip marks vents and shows INERT codons dark; the map gains a "material" colour (the share of a node's waste that isn't INERT).
- *Recommendation:* as above.

**C8. Defaults (Damian: new features on by default; hashes needn't hold yet).** C1–C6 are on by default. These older features are also off by default today and weren't in the hour-long test:
- the in-world GPU and NPU (`world.gpus`, `world.npus` 0), their wear, and the async devices (`accel.mode = instant`);
- warps, life on the GPU (`gpu.warps`);
- Phase 4: the disk tier, volatility, salvage, signals, the node lifecycle (endurance, starvation), the ambient heat leak, transit corruption and link failures;
- measured costs (`cost.table` defaults to `classic`), threads and E cores (`cpu.threads`, `cpu.e_share`), CPU wear (`cpu.wear` 0).

Experiment controls stay off: shuffled selectors, the open membrane, one origin, `accel.verify` and `warp.verify`, budgets.
- *Recommendation:* turn the world features on by default too, at the values the presets use (`configs/world.cfg`, `gpu_world.cfg`, the Phase 4 arms), with `home_v1`, threads, half E cores and `cpu.wear` 0.001. Tests keep testing what they test by setting their keys explicitly; pinned hashes that change are re-pinned once, with the reason recorded.

## 2. Steps
- **10a, decay and death:** C3, C4, C6 (`nova_v2`, decay, rarer rot, halted threads end, the frozen watchdog).
- **10b, vents and sparks:** C1, C2.
- **10c, bites:** C5.
- **10d, defaults and seeing it:** C7, C8, the presets, and the paired check.

## 3. Evidence
- `verify.ps1` green, covering:
  - C1: vents write only waste, within their radius, at their rate, deterministically;
  - C3: INERT ends templates and surfaces and fails when run; decay drives a dead block to INERT; living code doesn't decay;
  - C5: a bite of waste, a whole swallow, a partial bite taking the end nearest the eater and its share of millicores, a bite capped at the eater's size, a blind bite shielded by a surface, a bite's thread failing in the bitten code;
  - C6: a halted thread leaves; a frozen body dies; a waiting one doesn't;
  - the determinism gates: the same hash at 1, 4 and 20 threads, under shuffled dispatch, through snapshots, and on the GPU.
- *The paired check:* an hour on `configs/landscape.cfg` with everything on, against the 9c test. Do replicators last longer? Where do they live relative to vents? What share of bites hit waste and what share hit bodies, and does it shift as lineages evolve? How many sparks become codon zombies?

## 4. Docs
- design.md: a new section 6.16 (vents, decay, bites, death), 6.3 and 6.10 updated (sparks, waste, rot), section 10, 9.1.
- README.
