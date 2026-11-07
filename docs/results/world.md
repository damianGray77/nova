# Results: a varied world, part 1 (sparks by activity, substrates, multipliers)

Steps 3–5 of `docs/world-plan.md` (revision 2, approved by Damian): calls W5 (sparks follow node activity), W2 (substrates on the map) and W1 (what a multiplier does), with W8's `nova land` report and per-node columns and W9's keys, defaults, snapshot and determinism rules. Built 2026-11-07 in the `world` worktree, beside the disk tiers (W4), the disasters and the configuration page, which other builds cover.

## What was built

### W5: sparks follow node activity

- A node's spark rate is `spark.rate` × its spark weight (as before) × (1 − occupancy)^`spark.activity`.
- *Occupancy* (`Soup::occupancy`) is the share of the node's memory held by living bodies: every owned codon (soup size − waste), less the codons of resting dormant programs (6.17). Waste is not counted, nor is a dormant program's code while it rests.
- *When it is taken:* at the start of the node's tick, after `begin_tick` and the vents (which only write waste), before any spark or body runs. So it is the soup as the last barrier left it, after that barrier's arrivals and dormant landings. The tick's spark count is drawn from it then, as the count always was, also with `spark.timing = last` (the sparks are still placed after the bodies). The draw uses the same single `chance` call as before, so the node's random stream is unchanged in shape.
- `spark.activity = 0` returns the rate untouched (no power taken), so the old rule is exact. Initial sparks are unchanged.
- `Soup::spark_rate_now()` gives the rate the tick would draw from now; the tests and the viewer can read it.

### W2: substrates on the map

- *Eight types,* in the plan's order: P cores, E cores, GPU, NPU, fast memory, slow memory, fast disk, slow disk (`SubstrateType`, `core/include/nova/landscape.h`).
- *Five levels:* multipliers of `substrate.floor` (0.1), 0.5, 1, 1.5 and 2 (`substrate_multiplier`).
- *The fields* (`substrate_levels`, `core/src/landscape.cpp`): for each type,
  1. its own gradient-noise field (`Noise2`, 4 octaves, frequency `substrate.scale`; `Noise3` where the world drifts), with field id `kFieldSubstrate` (256) + the type: apart from graph.h's `NoiseField` ids, so apart from every resource field and from each other, and adding nothing to that enum (the disk build adds `res.archive`'s field there);
  2. rank-normalised over the nodes and turned into a normal score (as `spread_weights` does);
  3. mixed with an independent normal draw per node, `substrate.smooth` × field + √(1 − smooth²) × own, exactly as the resource distributions mix (C6); the draws come from one keyed world stream per type ("subs" + type), one per node in node order;
  4. ranked over the nodes (ties by id): the lowest share take the floor, the next 0.5 and so on up.
- *Exact shares:* `substrate_counts` turns the five shares into whole counts adding up to the node count (largest remainders, ties to the lower level): 100/200/400/200/100 of 1,000 nodes, 2/5/10/5/2 of 24.
- *World models:* substrates need a map. They are read and on in procedural and landscape soup worlds with `substrate.enabled` (default on); a ring world doesn't read the substrate keys and every level is 1x there. The demo model has none.
- *Recovery and drift:* a node's levels are taken by `World::derive_node` from the fields as `fields_at` leaves them for that tick, like its other properties:
  - a landscape's fields are static (it never drifts), so a site keeps its substrate for every life, as it keeps its resource shares;
  - in a procedural world with `world.drift`, the substrate fields drift through their noise with the others (time = tick × drift), and a recovering site takes the levels its site has at its recovery tick; a node keeps its levels for the rest of that life.
- *State:* the levels derive from the config and each node's `born` tick, so they are not new state; `World` keeps each node's current levels (`sub_level_`), hashes them (substrates on only), and writes them into snapshots (v4), where loading checks them against the levels the config gives, as it checks the edge list.
- *`nova land`* adds the number of distinct substrates (each node's eight levels together) and each type's share of nodes per level. With `substrate.enabled = false` it says "off".
- *The per-node file:* `nodes.csv` gains `sub_p_cores, sub_e_cores, sub_gpu, sub_npu, sub_mem_fast, sub_mem_slow, sub_disk_fast, sub_disk_slow` (each type's multiplier; 1 without substrates) and `occupancy` (at the window's end), after every earlier column. `RunArchive` reads columns by name, so it reads old and new files alike; it doesn't read the new columns (the viewer is left to the page and later builds).

### W1: what each multiplier does

Each node's `SoupParams` carries the eight speeds (`p_speed`, `e_speed`, `gpu_speed`, `npu_speed`, `mem_fast_speed`, `mem_slow_speed`, `disk_speed`, `archive_speed`), floats, 1 by default; `World::derive_node` sets them from the node's levels. Heat, error rates and wear are unchanged.

- **P and E cores** (`Soup::prepare_heat`, `run_body`, `share_time`, `heat_update`):
  - A body's time a tick comes from the sum of its units' heat speeds; each kind's sum is now multiplied by its kind's speed (P units by `p_speed`, E units by `e_speed`), so a millicore of that kind gives its speed × its tick's time and runs that many more (or fewer) instructions. The E share of the body's time (`ef`) follows.
  - *Threads* (6.15 4d): a thread runs on one core at a time, and a core gives its kind's speed × a tick. Where the two kinds' speeds differ, a thread with part of a core's time of one kind has the rest of a core for the other: a thread on half a P core at 1x and E cores at 2x runs half a tick on P, then the other half on E, where it does a whole tick's work. With equal speeds the old rule applies exactly.
  - *Heat:* a unit's nominal work becomes its kind's speed × a tick, so a fast core busy all tick heats exactly as a busy core does (6 a tick at `heat.cpu_gain` 6, at 1x, 2x and the floor alike).
  - Upkeep is unchanged in instructions, so a fast core pays it in less of its time. Actions' lengths in ticks (DISK, long device ops) and every other use of a tick's length are unchanged: a tick is still `cpu.instr_per_core` P adds.
- **GPU** (`Soup::prepare_gpu`, `accel_heat_update`, `warp_pack`, `Exec::device_time`):
  - *Lane throughput:* a unit's work a tick is `gpu.elements` × `gpu_speed`, for block ops and their heat. A body's warp work comes from what its units have left (`units = capacity / gpu.elements`), so it follows: `WW_QUOTA`, each warp's work, already reaches the kernel per warp.
  - *The warps' clock* (timed tables): a warp has `gpu_speed` × a tick of time a tick (`WW_TLEFT`, also per warp), and carries over what it ran past it, against the same scaled tick.
  - *No change to the kernel:* both values are computed on the host for every warp, so `warp_isa.inc`, `warp_kernel.hlsl` and its constants are untouched (the constants must be the same on every node for one dispatch, so the per-warp values were the place for it). DXC's and FXC's kernels need nothing new (suite `warp` still checks both); the everything-on world's GPU check below ran the build's kernel for 32 lanes with every round compared word for word against the reference.
  - *Timed block ops* in instant mode: the work's time over the body's units is divided by `gpu_speed` too.
  - The host-instruction wait (the GPU round trip plus the host's time) and a launch's start-up are not lane work and are unchanged.
- **NPU:** a unit's multiply-adds a tick are `npu.macs` × `npu_speed` (capacity, heat), and timed instant ops divide their multiply-add time by it.
- **Fast and slow memory, fast and slow disk:** the tiers don't exist yet. Their multipliers are set per node in `mem_fast_speed`, `mem_slow_speed`, `disk_speed` and `archive_speed`, with exactly those names, for the memory-tier build (W3) and the disk build (W4), which divides its tier costs by the last two. Nothing reads them here. (Since the integration the disk tiers read the last two, and all four are doubles, so the floor divides a cost by exactly 0.1: `docs/results/integration.md`.)

### Keys, defaults, snapshots

- *New settings* (`core/src/keys.inc`; `docs/keys.md` regenerated, 275 settings):
  - Substrates group: `substrate.enabled` (true; needs `world.graph=procedural|landscape`), `substrate.smooth` (0.7; **1 since the integration**), `substrate.scale` (3; **1.5 since the integration**: see "Defaults changed at the integration" below), `substrate.floor` (0.1), and the five shares `substrate.share_floor`, `.share_half`, `.share_one`, `.share_one_half`, `.share_two` (10, 20, 40, 20, 10).
  - Sparks group: `spark.activity` (2; 0 = by time alone).
- *Defaults:* on in the engine (Damian's rule). Off in the test runner's legacy fallback (`substrate.enabled = false`, `spark.activity = 0` in `tests/test_main.cpp`), so every existing pinned and gate hash is unchanged.
- *Snapshots:* `kSnapshotVersion` 3 → 4. With substrates on, each node's eight levels follow the lifecycle section (checked on load). A snapshot before v4 comes from a world without either rule, so `Run::init_from_snapshot` resumes it with `spark.activity = 0` (soup worlds) and `substrate.enabled = false` (procedural and landscape worlds) unless its config or a `--set` names them. The effective config then carries them, so later snapshots keep them off. (Since the integration these lines, with the disk tiers' `world.archive = 0`, live in one text, `core/include/nova/legacy.h`, which replay logs from before v3 use too: `docs/results/integration.md`.)
- `substrate.` joins the prefixes a refused live change calls "structural".

## Settled details

1. **The five shares are relative weights,** each 0–100, normalised by their sum: five knobs on the page needn't add up to 100. All five 0 is refused.
2. **The per-node file holds multipliers,** not level numbers: the floor reads 0.1 (or whatever `substrate.floor` is), the others 0.5, 1, 1.5, 2.
3. **The column names** are `sub_<type>` with the types' short names (`p_cores`, `e_cores`, `gpu`, `npu`, `mem_fast`, `mem_slow`, `disk_fast`, `disk_slow`).
4. **Fast disk = `disk_speed`, slow disk = `archive_speed`,** as the brief named them.
5. **A ring world doesn't read the substrate keys,** as it doesn't read `land.*` or `vary.*`: setting one there is an unused-key error. Sparks by activity apply in every soup world.
6. **Occupancy is taken once per tick per node** at the start of its tick (above), not as the tick goes. That keeps the rule a function of the soup's state at a fixed point, the same on any thread and in any dispatch order.
7. **The floor's speed as a float:** 0.1 is stored as a float (0.1000000015) for the cores and accelerators (the tiers' four speeds became doubles at the integration, since they divide costs). A whole number of millicores times it rounds back exactly in the tests' cases (1,000 × 0.1 = 100.0); the speeds multiply a body's summed unit speed once, not each unit's, so the floor's error doesn't accumulate over a body's units.

## Measurements (Debug build, the home rig, shared with the running `long1`)

**Multipliers** (suite `substrates`), each at 1x, 2x and the floor (0.1x):

| what | 1x | 2x | floor |
|---|---:|---:|---:|
| P core, 1,000 millicores, 10 ticks of 100 adds: instructions | 1,000 | 2,000 | 100 |
| a thread on 2,000 millicores (capped at a core) | 1,000 | 2,000 | 100 |
| E cores (home_v1, 218 ps an add, 10 ticks) | 872 | 1,744 | 88 |
| a GPU unit's element-ops a tick (classic 32 x 8 units) | 256 | 512 | 25.6 |
| warp steps in 10 ticks, bound by throughput (8 units) | 1,280 | 2,560 | 128 |
| warp steps in 10 ticks, bound by each warp's clock (a tick of 190 lane adds) | 1,900 | 3,800 | 190 |
| two NPU units' multiply-adds a tick (a matmul of 9, 729 of them, fits only at 2x) | 512 | 1,024 | 51.2 |
| a timed GPU compare of 10^6 elements on 1 unit ends at tick | 162 | 88 | |
| a timed NPU matmul of 32 on 1 unit ends at tick | 15,887 | 8,298 | |
| a P unit's heat after a tick flat out | 6 | 6 | 6 |

**Sparks** (W5): rate 0.8 gives 0.8 on an empty node, 0.2 half full, 8 × 10⁻⁵ at 99%, 0 full; a resting dormant program and waste leave it unchanged. In a running soup at rate 0.5 for 400 ticks: 206 sparks on an empty node of 100,000 codons (its sparks hold under 2% of it), 0 on one 99% full. (A first version of that check gave its sparks 10 millicores at a full instruction rate and counted 84 sparks instead of about 200: random code that runs claims memory, so even an empty node's rate falls within a few hundred ticks. I didn't measure that node's occupancy; the test now keeps its sparks idle.)

**The map** (`nova land configs\landscape.cfg`, 1,000 nodes, seed 1): every type exactly 10.0 / 20.0 / 40.0 / 20.0 / 10.0%; **990 distinct substrates** of 1,000 nodes at the first defaults (smooth 0.7, scale 3), **542** at the defaults since the integration (smooth 1, scale 1.5); generation adds nothing measurable (778 ms with substrates, 776 without, mostly the soups).

**How smooth the map is** (a 1,000-node landscape, seed 5; neighbours are its paths): mean level steps a type between two nodes, and how many of the 8 types differ. The first row is today's defaults (since the integration), the second the approved first defaults.

| `substrate.smooth` | `substrate.scale` | neighbours: steps a type | types differing | random pairs: steps | types | neighbours / random | distinct (landscape.cfg) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| **1.0** | **1.5** | 0.224 | **1.75** | 1.198 | 5.92 | **19%** | 542 |
| 0.7 | 3 | 0.878 | 5.24 | 1.201 | 5.93 | 73% | 990 |
| 0.9 | 3 | 0.615 | 4.27 | 1.201 | 5.92 | 51% | 967 |
| 1.0 | 3 | 0.347 | 2.69 | 1.200 | 5.93 | 29% | 806 |
| 0.7 | 1.5 | 0.868 | 5.21 | 1.199 | 5.92 | 72% | 990 |

**Everything on** (a 24-node landscape on the engine's own defaults: timed costs, threads and E cores, warps, GPU and NPU, disk, signals, dormancy, vents, substrates and sparks by activity, with a quick lifecycle): 160 ticks, 560 bodies, 256 warps, mean occupancy 0.46, 4 collapses and 3 recoveries; the same hash at 1, 4 and 20 threads, under shuffled dispatch and through a snapshot at tick 80 resumed on 20 threads; and on the RTX 5080 with `warp.device = gpu` and every round verified, 160 of 160 rounds the reference's word for word, the same hash. The whole suite runs in about 6.5 s.

**A first look at the rules' effect** (`configs/landscape.cfg` cut to 200 nodes of 4,096 codons, 600 ticks, 4 threads, Debug, accelerators and warps on the CPU reference; one seed, so indicative only):

| | both off | substrates only | both on |
|---|---:|---:|---:|
| sparks | 17,753 | 17,974 | 3,145 (−82%) |
| sparks a window of 100 ticks, first → last | 3,518 → 2,380 | 3,545 → 2,530 | 447 → 543 |
| births | 9,510 | 9,985 | 6,412 |
| deaths | 23,797 | 24,927 | 12,178 |
| CPU instructions | 53.6 million | 68.2 million (+27%) | 50.4 million |
| warp steps | 182 million | 187 million | 135 million |
| bodies at tick 600 | 11,900 | 11,830 | 7,792 |
| mean occupancy at tick 600 (nodes over 90%) | 0.74 (94) | 0.74 (94) | 0.57 (8) |
| wall time | 156 s | 163 s | 125 s |

- Sparks by activity cut sparks by four fifths at once, and the nodes stop filling to the brim: with the old rule 94 of 200 nodes were over 90% full by tick 600, with it 8. Fewer sparks also means fewer deaths of random bodies (half as many).
- Substrates alone left the world's size alone but ran 27% more CPU instructions: work moves to the fast cores. (The levels average 1.01x, so this is life using fast land more, not more speed overall.)
- The wall times are of different worlds on a shared machine, not of overhead: the rules add a multiply per body a tick, a few per device op, and one pass over a node's bodies for its occupancy each tick (only with dormancy on).
- `nova land`'s very first run after the build took 70 s; every run since takes 0.74–0.78 s on each preset, with substrates on or off, so that was a one-off start-up cost (the GPU device or a cache), not the generator.

## Deviations and findings, for Damian

1. **The defaults make a far patchier map than the plan expects.** The plan says neighbours share most of their levels and crossing regions usually changes one type by a step. With `substrate.smooth` 0.7 and `substrate.scale` 3, mixed as the resource fields are (as the brief and the plan say), neighbours differ in 5.2 of 8 types (random pairs: 5.9), 73% of a random pair's difference, and 990 of 1,000 nodes have a substrate of their own. At smooth 0.7 the independent part of the mix weighs about as much as the field (√(1 − 0.49) ≈ 0.71 against 0.7), and at the presets' node spacing that decides most levels; even the field alone at scale 3 changes a third of the types between neighbours. `substrate.smooth = 1` with `substrate.scale = 1.5` gives what the plan describes (neighbours differ in 1.75 types, 19% of random, 542 distinct substrates). I kept the approved defaults; the test asserts that the defaults are under 80% of random and the field alone under 35%. Recommendation: `substrate.smooth` 1 and `substrate.scale` 1.5 for the run (or anything between, from the table above). **Resolved at the integration:** Damian asked for substrates that transition into each other nicely, and the defaults are now 1 and 1.5 (below).
2. **Pre-v4 snapshots resume with `spark.activity = 0` too.** The plan names tiers and substrates; an old snapshot's world had the old spark rule as well, so it resumes with both off unless told otherwise.
3. **Snapshot v4 carries the substrate levels only to check them** (they derive from the config and each node's life, like its resources): a mismatch refuses the snapshot rather than overriding the config's map.
4. **The viewer is untouched,** as asked: no substrate map mode yet (W8), and `RunArchive` doesn't load the new columns. `docs/design.md` isn't updated: the plan's behaviour is documented here and in `docs/keys.md`, for folding into design.md at the merge.
5. **For the merge:**
   - `SoupParams`'s `disk_speed` and `archive_speed` duplicate the disk build's: drop one pair.
   - `kSnapshotVersion` is 4 here too, with this build's block after the lifecycle section under `substrates_` (and `version >= 4` on load); `Run::init_from_snapshot`'s pre-v4 block may want the disasters' and tiers' switches added beside these two.
   - The test `test_old_snapshot` downgrades a snapshot of a world with substrates off to v3; if another build writes a v4-only section for that world's config (legacy fallback), it needs that feature off there too.
   - `nodes.csv`'s new columns go after `born`; the lineage build adds stats columns, not node columns.

   All four were settled at the integration (`docs/results/integration.md`): one pair of speeds (now doubles), one v4, the pre-v4 lines in `core/include/nova/legacy.h`, and `test_old_snapshot` on a world with a disk and no archive.

## Defaults changed at the integration

Damian asked for substrates that "transition into each other nicely". The approved defaults gave a patchy map (deviation 1), so the integration changed them, in the registry (`core/src/keys.inc`, `docs/keys.md` regenerated) and in `SubstrateParams`' own defaults (`core/include/nova/landscape.h`), which the neighbours test checks agree:

| | `substrate.smooth` | `substrate.scale` | neighbours: types differing (of 8) | neighbours / random pairs | distinct substrates (`landscape.cfg`, 1,000 nodes) |
|---|---:|---:|---:|---:|---:|
| before (approved) | 0.7 | 3 | 5.24 | 73% | 990 |
| **after** | **1** | **1.5** | **1.75** | **19%** | **542** |

(Neighbours: the paths of a 1,000-node landscape, seed 5; random pairs: 20,000; suite `substrates`, re-measured on the integrated build, and `nova land configs\landscape.cfg`, with and without `--set substrate.smooth=0.7 --set substrate.scale=3`.)

- At smooth 1 the levels follow the fields alone (the independent draw per node is still drawn, weighing 0), and at scale 1.5 the features are twice as large, so crossing from one region to the next usually changes one or two types by a step, as the plan describes. Each type still has its own field and exact 10/20/40/20/10 shares.
- *Tests:* `test_neighbours` measures today's defaults first and asserts neighbours under 30% of random pairs and under 2.5 types differing; the first defaults stay in the table for the record (under 80%), with the field alone at scale 3 (under 35%) and each smoother setting smoother than the last.
- *What else moved:* the everything-on world (suite `substrates`, engine defaults, now with the archive on too) reads 160 ticks, 531 bodies, 230 warps, mean occupancy 0.44, 6 collapses and 4 recoveries (before: 560, 256, 0.46, 4, 3), still the same at 1, 4 and 20 threads, shuffled, through a snapshot and on the RTX 5080. No pinned or gate hash moved: the test runner's legacy fallback keeps substrates off.

## Determinism

- Every substrate draw comes from keyed world streams (the fields' permutations, keyed by field id; one normal draw per node per type from the "subs" stream), in node order, at init or at a recovery barrier; nothing in a tick draws for it. Occupancy is read from the node's own soup at a fixed point of its own tick.
- With both switches off every path is the old code exactly: a speed of 1 multiplies by 1.0f (exact) or takes the old branch, and the spark rate is returned untouched. `verify.ps1` keeps every pinned and gate hash (all 41 tests green: 40 suites and the include check).
- Suite `substrates` checks: exact shares and the counts' rounding; each type its own field; neighbours against random pairs; the same map for the same seed and not for another; a static landscape's recovered sites back on their substrate; a drifting world's recovered sites on the levels at their recovery tick, and a snapshot taken through recoveries resuming exactly; every multiplier at 2x and the floor, heat unchanged, the mixed-kind thread rule, timed device ops; the spark curve, resting dormant programs and waste; the per-node file's header and values; pre-v4 snapshots; and the everything-on world at 1, 4 and 20 threads, shuffled, through a snapshot and on the GPU.

## Files touched

- New: `tests/test_substrates.cpp`, `docs/results/world.md`.
- Changed:
  - `core/include/nova/landscape.h`, `core/src/landscape.cpp`: the substrate types, levels, counts and fields.
  - `core/include/nova/soup.h`, `core/src/soup.cpp`: `SoupParams`' new fields (one block), `spark.activity` read, `occupancy`, `spark_rate_now`, the spark draw, P/E speeds in `prepare_heat`, `run_body`, `share_time` and `heat_update`, the GPU and NPU per-unit work.
  - `core/src/warp.cpp`: the warps' clock.
  - `core/src/interp.cpp`: timed block ops' time.
  - `core/include/nova/world.h`, `core/src/world.cpp`: reading the keys, the substrate map per node, `derive_node`, hash, save, load and check, the structural prefix.
  - `core/src/run.cpp`: pre-v4 snapshots.
  - `core/include/nova/snapshot.h`: v4.
  - `core/include/nova/stats.h`, `core/src/stats.cpp`: the per-node columns.
  - `app/main.cpp`: `nova land`'s report.
  - `core/src/keys.inc`, `docs/keys.md`: the settings.
  - `tests/test_main.cpp` (legacy defaults, the suite), `tests/CMakeLists.txt`, `tests/test_snapshot.cpp` (the future-version message follows `kSnapshotVersion`), `tests/test_ecology.cpp` (the defaults' check).
