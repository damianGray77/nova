# Phase 7 plan: one big landscape world

> **Revision 2, APPROVED by Damian (2026-11-06), BUILT (2026-11-06).** What was built is in design.md 6.13. Details settled while building:
> - **The disk tier (C8):** each node's per-codon disk arrays go to memory-mapped files instead of moving ownership to extents. Every rule and existing hash stays the same, and a world hashes the same with its disk in RAM or in files. Files cost about 5 bytes a codon (9 with salvage stamps). Disk contents are written in full at start-up; starting lazily is left open.
> - **Budgets** replace the setting they resolve (for example `budget.memory` replaces `world.soup_size`), so they work on any preset. The request is kept as `budget.<x>_was`.
> - **Threads and priority:** `budget.threads` defaults to every logical processor but one, and `budget.priority` to below normal; the harness passes its `-Priority` that way.
> - **Bridges:** hermits get none, and signals don't cross them.
> - **`paths.connected`** joins a landmass's pieces across its own bays when its land paths can't.
> - **Presets:** continents need about 30% land at scale 3, because noise land sunk toward the borders joins into one continent above that.
> - **`nova land`** was added to see what settings generate without running.
>
> Revision 1 (parallel worlds joined by bridges) was replaced after Damian's review: one world, procedurally generated like a game level, whose landmasses give different ecologies; fixed paths generated once; bridges between landmasses that open on a schedule; distributions for sparks and resources (scarce and abundant nodes); budgets mapped to the machine's real resources; everything a setting. His answers (2026-11-06):
> 1. Settings are fixed at the start of a run (live changes would be interesting, but start-only is more practical).
> 2. Budgets: whatever is realistic to work with, but with scarce and abundant nodes, and using as much of the machine as possible without crashing or freezing the OS.
> 3. The in-world disk on real disk, at a path he chooses (not filling C:). Resources map to their real counterparts, except CPU.
> 4. Triangulation, one style.
> 5. Performance work is always profile-driven.
>
> - **Source of truth:** `docs/design.md`. 9.1 Phase 7 ("1,000+ nodes, parallel worlds with island migration") becomes this plan; 7.1 J's island model is met inside one world.
> - **Long runs:** the 12-hour run waits for Damian's go-ahead.

## 0. Scope
- A new world generator, `world.graph = landscape`: land and water, separate landmasses, nodes with 0 to 10+ paths, bridges between landmasses on a schedule.
- Distributions for sparks and every resource, so some nodes are rich, some poor and some barren.
- Budgets that size the world from the machine: real RAM, a real disk file, the real devices; CPU stays virtual.
- The viewer for big landscapes, profiling, and output that holds up over long runs.

**Not here:** settings that change mid-run (beyond Phase 6's live knobs, which stay), separate worlds, several machines.

**Next, Phase 8 (Damian, 2026-11-06): programs that run on the GPU.** Landscapes will have GPU-rich nodes with almost no CPU, where today nothing can live (a body with no running CPU dies). Damian wants the GPU to differ from the CPU so programs can use the strengths of both: the CPU for serial work, the GPU for parallel. So a GPU core will run a body's code in lockstep across many lanes with an instruction set as rich as the CPU's: cheap when lanes agree, slow when they branch apart. It gets its own plan after Phase 7, and runs on the CPU reference first.

**Kept as they are:** `world.graph = ring` and `procedural`, with every existing config, test and pinned hash. The new generator is a new mode.

## 1. Two steps
Phase 7 is big, so it's built in two steps, each tested and documented before the next:
- **7a, the landscape** (C1–C6, C10–C12): generation, paths, bridges, the lifecycle on fixed paths, distributions, the viewer, profiling and output.
- **7b, real resources** (C7–C9): RAM and disk budgets, the disk tier in a real file, device calibration, and the start-up safety checks.

## 2. Design calls

**C1. The land.** The world is a rectangle (`land.aspect`, width over height). A noise field, warped so coastlines wander, is cut at `land.fraction` to give land and water. `land.scale` sets feature size: a few continents or many islands. A flood fill over the land labels each separate landmass.
- *Recommendation:* as above.

**C2. Nodes.** `world.nodes` nodes are scattered over the land by Poisson-disc sampling: evenly spread but not on a grid, the usual way games place points. Each node belongs to the landmass it sits on. The sampling's spatial lookups run once, during generation; nothing searches for neighbours while the world runs.
- *Recommendation:* as above.

**C3. Paths, generated once.** A Delaunay triangulation of the nodes (hand-rolled, built once) gives every node its natural neighbours, with no crossing paths. Then:
- paths that cross water are dropped, so landmasses are cut off from each other;
- each node draws how many paths it wants from a distribution (`paths.mean`, `paths.spread`), so most have a few and some only one;
- a fraction are hermits with none (`paths.isolated`);
- a fraction are hubs (`paths.hubs`) that add roads to their neighbours' neighbours, up to `paths.max` (default 12);
- a path survives if both ends still want it, shortest first;
- `paths.connected = true` keeps each landmass in one piece (default false, so dead ends and stranded nodes happen);
- a few long "river" paths within a landmass (`paths.long`), as now.

Each path's latency comes from its length and its bandwidth from the terrain (a ruggedness field), as in Phase 3. Paths are stored once as flat arrays: per node, a slice of targets, latencies and bandwidths. Neighbour numbering (DIVIDE to neighbour k) is nearest first and never changes.
- *Recommendation:* as above.

**C4. Bridges between landmasses.** Triangulation edges that cross water are the candidate crossings; the shortest between each pair of neighbouring landmasses becomes a bridge, up to `bridge.count` per pair. Other settings:
- `bridge.isolated`: a fraction of landmasses get none, natural controls (with the caveat that they differ in place and resources);
- `bridge.open_ticks` and `bridge.open_every`: open for 1,000 of every 20,000 ticks by default;
- `bridge.phase`: all together, staggered or random;
- `bridge.latency_scale`, `bridge.capacity`.

A shut bridge refuses departures; cargo already on it still arrives (unlike a failed link, which drops it). Bridges are paths with a schedule, numbered after a node's land paths.
- *Recommendation:* as above.

**C5. The lifecycle on fixed paths.** In this mode a collapsed node takes its paths down until it recovers, and nothing is rebuilt. That is cheaper, and the geography stays put when a node dies. Collapse (wear, starvation) and recovery work as in Phase 4. The land doesn't drift in this mode; sites keep their place and paths for every life.
- *Recommendation:* as above.

**C6. Distributions: scarce, abundant and barren.** One model serves sparks and every resource (memory, CPU, GPU, NPU, disk). Each node's share of the total is drawn from a skewed (lognormal) spread around the mean, with these settings (`spark.*` and `res.<memory|cpu|gpu|npu|disk>.*`):
- `spread`: 0 even, higher for richer and poorer nodes;
- `smooth`: 0 independent per node, 1 following a noise field, so rich and poor regions form like biomes;
- `barren`: the fraction of nodes with none (memory keeps a minimum soup);
- `hotspots` and `hotspot_boost`: clusters of plenty.

Shares are scaled so the node totals add up exactly to the budget. Barren spark nodes get life only from colonists.
- *Recommendation:* as above.

**C7. Budgets mapped to the machine (7b).** Settings are given as a percentage or a fixed amount, turned into real numbers at start-up, and recorded in the run's config and replay log. So a replay is exact, and the same config on another machine starts from the same recorded numbers.
- *Memory:* `budget.memory` (default 50% of the RAM available at start, or e.g. `24GB`). The world's codons are real RAM: total codons = budget ÷ the measured bytes a codon costs with all its bookkeeping (about 18 today, with headroom for populations), spread over the nodes by C6.
- *Disk:* `disk.path` plus `budget.disk` (off unless a path is given, so nothing lands on C: by accident). See C8.
- *CPU (virtual, as you said):* total millicores spread by C6. The real side is `budget.threads`: default every logical processor but one, at below-normal priority so the desktop stays responsive.
- *GPU and NPU:* their work already runs on the real devices (Phase 5, async mode). A device has no "share" that can be set directly, so a short calibration at start-up measures its throughput. The world's GPU and NPU totals are sized so a fully busy world keeps the device about `budget.gpu` / `budget.npu` busy, then spread by C6. The numbers are recorded like the rest.
- *Recommendation:* as above.

**C8. The disk tier in a real file (7b).** The in-world disk becomes a memory-mapped file at `disk.path`, one byte per codon; Windows pages it in and out.
- A page never written reads as its seeded random contents, so no 100 GB file has to be written at start.
- Ownership becomes per extent instead of per codon. Today it costs 5 bytes of RAM per disk codon, which can't scale to a real disk. Salvage stamps move to extents too.
- The disk's part of the world hash is kept up to date on every write, rather than re-reading the file at each checkpoint.
- Snapshots copy the file beside themselves, so resuming stays exact.
- Start-up refuses a budget the volume can't hold, with a margin.
- *Recommendation:* as above.

**C9. Not crashing or freezing (7b).** At start-up the run checks that the RAM, disk and video memory budgets fit what is free, with margins, and says what it would need otherwise. It runs one thread short of all and at below-normal priority. The logs that grew without limit are capped or handed off: the genebank (done in Phase 6), the run's event log and the per-node stats (C11).
- *Recommendation:* as above.

**C10. The viewer for big landscapes.** The map already zooms with the wheel around the cursor and pans by dragging (Phase 6). New:
- land and water drawn behind the nodes;
- landmasses labelled;
- bridges drawn open or shut with their schedule;
- simpler drawing when zoomed out (nodes as dots, short paths hidden);
- a small overview map;
- double-click a landmass to fit it.
- *Recommendation:* as above.

**C11. Profile-driven speed, and output at scale.**
- `nova run --profile` splits each tick into the parallel update, each barrier step and the run's work after the tick. Only what measures slow gets changed.
- The new node and path data is structure-of-arrays from the start. The interpreter keeps each CPU's state together, because it reads it together.
- `stats.nodes_every`: per-node rows every Nth window.
- The run's event log drops what it has handed on.
- Presets: `configs/landscape.cfg` (continents), `archipelago.cfg` (many islands, bridges) and `maze.cfg` (sparse paths, many dead ends), at 1,000 nodes; bench workload `landscape`.
- A 10,000-node check of memory, start-up and tick time, recorded in design.md.
- *Recommendation:* as above.

**C12. Determinism.** Generation is a pure function of the seed and the settings. A landscape world gives the same hash and events at 1, 4 and 20 threads and under shuffled dispatch, snapshots and replays exactly, and every existing mode's hash is unchanged.
- *Recommendation:* as above.

## 3. Evidence
- `verify.ps1` green, covering:
  - **7a:** land and landmass labels; the triangulation (every triangle's circumcircle empty, against a brute-force check); paths never crossing water; the path-count spread, hermits and hubs; bridge schedules (shut refuses, cargo already on a bridge arrives); the fixed-path lifecycle; distributions adding up to their budget exactly; the gates in C12.
  - **7b:** the disk file (lazy pages, extents, the incremental hash, snapshot copy and resume); budget resolution and its record in the config; the start-up refusals.
- Measured: generation time and tick time at 1,000 and 10,000 nodes; a profile; renders of each preset; a short run of the archipelago preset reporting bridge crossings and colonists across landmasses.

## 4. Docs
- design.md: a new 6.13 (the landscape and real resources), 4.1 and 9.1 (built), 7.1 I (measurements) and J (islands inside one world), the status line.
- README: running the presets, choosing a disk path and budgets.
