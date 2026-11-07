# Plan: a varied world (substrates, memory and disk tiers, sparks by activity, a bigger world)

> **Revision 2, for Damian's approval (2026-11-07).** World rule changes for the next run, behind keys, built together with `docs/disasters-plan.md` (revision 6) and `docs/config-plan.md`. Damian's decisions are marked (Damian). The open details carry recommendations.
>
> Revision 2 follows Damian's review:
> - lineages are linked by descent (disasters X12);
> - substrates are expected to make rich and poor land more than separate ways of life, with trade-offs as a later knob (W1);
> - memory tiers are built last (section 2);
> - fewer generations a day is a known cost of the bigger world (W6);
> - one run with everything, where every key is a toggle or knob on a new configuration page, instead of staged runs (W9).
>
> - **Why:** the long run (`runs\long1`, 420,000 ticks) shows life as a churn of new origins rather than lines of descent:
>   - about 10,000 lineages were founded every 1,000 ticks before the ring replicator, and about 30,000 since;
>   - 80% never get past 2 members;
>   - the gated ones live a median of about 300 ticks and reach depth 2–3;
>   - one lineage in the whole run reached 20,000 members.
>
>   The 11-codon ring replicator reached 612 of 1,000 nodes because every node rewards the same thing. Damian wants life far more lineage-based, and varied enough that no one form sweeps the world. Disasters supply the disturbance that keeps it from settling.
> - **What it builds on:** the landscape generator (6.13), the measured costs (6.15), the disk tier (6.10), dormancy (6.17), and the disasters plan (X5 disk failure, X12 the lineage tracker's fixes).
> - **Source of truth:** `docs/design.md` 4.4a, 5, 5.1, 6.10, 6.13, 6.15.

## 0. Decided with Damian

- **Substrates follow the resource types:** P cores, E cores, GPU, NPU, fast memory, slow memory, fast disk and slow disk. That's eight types; the NPU was added by Damian.
- **Each type has a multiplier of 0, 0.5, 1, 1.5 or 2.** A node's substrate is its combination of the eight, so there are 5⁸ = 390,625 possible substrates.
- **0 means present but nearly useless,** not absent, so every node can still host life.
- **The levels follow a bell:** 10%, 20%, 40%, 20% and 10% of nodes, so 1× is the commonest.
- **Substrates come from the landscape generator** and blend into each other across the map.
- **The memory and disk tiers are built.** Design 5 had them, and 5.1 deferred them.
- **Sparks follow node activity, not time.** As a node fills up, its sparks fade, and its future is up to the lineages already there. Migration, extinction and disasters still open room for new origins.
- **The world gets bigger:** bigger nodes or more of them, accepting a slower run (W6 recommends which).

## 1. Design calls

**W1. What a multiplier does.**
- A multiplier sets how well a resource performs on a node: its speed or throughput. How much of it the node has stays with the existing resource model (`res.*`).
  - So a node can have few but fast cores, or a lot of slow memory.
- Per type:

  | type | the multiplier scales |
  |---|---|
  | P cores | a P millicore's time each tick (the instructions it runs) |
  | E cores | the same for E millicores |
  | GPU | a unit's lane throughput and its warps' clock |
  | NPU | a unit's multiply-adds per tick (`npu.macs`) |
  | fast memory | the speed of reads, writes, searches and moves in the fast tier: its costs divided by the multiplier |
  | slow memory | the same in the slow tier |
  | fast disk | the speed of DISK on the fast disk: its access and per-codon costs divided by the multiplier |
  | slow disk | the same on the archival tier |

- The 0 level is `substrate.floor` (0.1×).
- Heat, error rates and wear are unchanged. Heat follows how busy a unit is, not how much it gets done, so a fast core is simply a better core.
- *What to expect (agreed in review):*
  - On CPU, the best genome is the same everywhere, so speed multipliers mostly make rich and poor land, and fast regions will send emigrants into slow ones.
  - Distinct ways of life come where the best strategy changes: GPU against CPU land, E against P cores, and slow memory, which punishes heavy copying and searching.
  - Multipliers on resources life barely uses (disk, NPU, archive) matter only once something uses them.
- *Later knob:* pairing a strength with a cost (for example, a fast substrate that runs hot or copies with more errors) would sharpen local adaptation. It isn't in this revision.
- *Recommendation:* as above.

**W2. Substrates on the map.**
- *Fields:* each type has its own noise field, seeded apart from the others and from the resource fields. Each is mixed by `substrate.smooth` (default 0.7) at `substrate.scale` (feature size, default 3, like `land.scale`), as the resource fields are.
- *Levels by rank:*
  - Across the nodes, the lowest 10% of a field's values get 0, the next 20% get 0.5, the middle 40% get 1, the next 20% get 1.5 and the top 10% get 2.
  - The shares are exact, set by `substrate.levels` (10,20,40,20,10).
- *Transitions:* because the fields are smooth, neighbours share most of their levels. Crossing from one region to the next usually changes one type by one step, the way coastlines wander.
- *Independence:* the eight fields don't depend on each other or on the land in this revision. Correlations (for example, GPU-rich land tending to have slow memory) are a later knob.
- *Recovery:* a recovered site takes its substrate from the fields at its position, as it does its resources. Where the world drifts (`world.drift`), substrates drift with it.
- *Recommendation:* as above.

**W3. Memory tiers.**
- *Where:* the first `mem.fast_share` of each node's soup is fast memory (default 1/8: 4,096 codons of 32,768); the rest is slow.
  - It stays one address space, so contact, ENGULF, INJECT, searches and waste work across the boundary as now.
- *What each tier costs:* each processor's own measured prices from the `home_v1` table (6.15), the fast tier at its nearest cache and the slow tier one level out:

  | processor | fast tier | slow tier | today (one tier) |
  |---|---|---|---|
  | CPU (P core; E cores by their column) | a load from the nearest cache, 755 ps | a load from 1 MB, 3.6 ns (4.8×) | 755 ps |
  | GPU lane | its nearest cache, 18.8 ns | the L2, 145.7 ns | 145.7 ns |

  - Loads, stores, template scans and bulk moves are priced by the tier of each codon they touch. Then each tier's multiplier divides its cost.
  - So a CPU copy of 32 codons costs about 144 adds in fast memory and about 620 in slow, beside about 4,000 for its two kernel calls (ALLOC, DIVIDE). Replicating is 10–15% cheaper in fast memory, and search-heavy code gains most.
  - Fetching instructions isn't charged today and stays that way. Where a program lives matters through what it reads, writes and searches.
- *Getting into fast memory:*
  - ALLOC memory works as today and lands wherever it finds room, mostly in slow memory and sometimes in fast by chance.
  - A new ALLOC selector claims fast memory only, and fails if there's no room of that size.
  - Sparks, arrivals and landing dormant programs land anywhere, as now. A program that learns to ask for fast memory gains.
- *The trade-off:* fast memory is scarce and, as design 5 had it, volatile. Its wipe chance is `mem.fast_volatility` (4) times the slow tier's.
- *No new SENSE* (Damian): a program finds the difference through speed alone.
- *Recommendation:* as above.

**W4. Disk tiers.**
- *The fast disk:* today's disk, unchanged (`world.disk`, `disk.rot`, its costs).
- *The archival disk:* a new tier on every node, `world.archive` codons (default a quarter of the disk), spread by its own resource field (`res.archive`). It is:
  - near-permanent: rot at `archive.rot`, a hundredth of the disk's;
  - slow: each DISK operation on it costs `archive.slow` (10) times the disk's, so a random read takes about 100 ticks;
  - scarce: a quarter of the space.

  That is design 5.1 B's form of expense without a currency: scarce capacity and slow access.
- *Holding it:*
  - A new ALLOC selector claims archival disk.
  - RELEASE, DIVIDE's share, ENGULF, death (as fossils), dispersal and dormancy treat it as disk, and each holding keeps its tier.
  - A body's disk is all its extents in claim order, addressed from 0 whatever their tier. A DISK operation costs the slowest tier it touches for access, plus each codon at its own tier's price.
- *Dormancy:* the clock spends the fast disk reserve before the archival one.
- *Disk failure (disasters X5, amended):* a failure takes one tier, chosen by size, so it's usually the fast disk. An archive failure is rarer but possible, so near-permanent never means permanent.
- *The multipliers* divide each tier's access and per-codon costs.
- *Recommendation:* as above.

**W5. Sparks follow node activity (Damian).**
- *The rule:* a node's spark rate is `spark.rate` × its spark weight (as now) × (1 − occupancy)^`spark.activity`, where:
  - occupancy is the share of its memory held by living bodies, counting neither waste nor resting dormant programs;
  - the default power is 2.
- *What that gives:*

  | node | occupancy | spark rate |
  |---|---|---|
  | typical today | 99% | 1/10,000 of the rate |
  | half full | 50% | a quarter |
  | empty | 0% | the full rate |

  So a full node's future is up to its lineages. A node emptied by a crash, a surge, a recovery or an extinction sparks again, and fresh origins compete with colonists arriving from its neighbours.
- *The smooth curve:* it follows the design's rule of gradual effects rather than cliffs. `spark.activity` 0 is today's rule.
- *Unchanged:* initial sparks, which start the world.
- *What to watch:* sparks also make prey and recycle waste. Vents and decay now do part of that, and step 7 checks that predators don't starve.
- *Recommendation:* as above, with power 2.

**W6. A bigger world (Damian: bigger or more nodes; slower is accepted).**
- *Why it costs speed:* memory is what limits populations (nodes are 99% full), so bodies, and the cost of simulating them, grow with memory.

  | option | memory | estimated speed | notes |
  |---|---|---|---|
  | 1,000 nodes × 32,768 codons | 2× | about 8 ticks/s, about 700,000 ticks a day | about 6.5 GB of RAM and 2 GB snapshots (63 GB machine) |
  | 500 nodes × 32,768 codons | same | about today's 16 ticks/s | bigger local populations, half the geography |
  | 2,000 nodes × 16,384 codons | 2× | about 8 ticks/s | more geography, same small local populations |

- *Recommendation:* 1,000 nodes × 32,768 codons.
  - Bigger local populations make chance extinctions rarer, which is what lineages need.
  - Keeping 1,000 nodes keeps the geography that substrates and disasters work on.
  - Every resource doubles with the memory (millicores 4,000, disk 8,192, archive 2,048, GPU and NPU units), so each resource per codon is unchanged.
  - It's a preset change (`configs/landscape.cfg`), not code. The speed estimate is checked in the first hour of the run.
  - *The cost (agreed in review):* half the speed means half the generations a day, and evolution needs generations as much as population. The first day's numbers show whether that's the right balance. The world size is a setting on the configuration page, so it can be changed for any later run.

**W7. Founders by origin.**
- *What it records:* each body carries a byte saying how it began:
  - spark;
  - daughter: born by DIVIDE but not recognised as a copy;
  - arrival: dispersed in without a lineage;
  - wake: woke from dormancy without a lineage.
- *Where it shows:* when a body founds a lineage, its origin goes into `lineages.csv` (new column `founder_origin`) and into four new stats columns (`founded_spark`, `founded_daughter`, `founded_arrival`, `founded_wake`).
- *Why:* to see whether spark-born lineages fall as W5 intends, without scanning a 355 MB file.
- *Scope:* like the lineage tracker it only observes. It isn't hashed or snapshotted, so bodies alive at a resume read as unknown.
- *Recommendation:* as above.

**W8. Seeing it.**
- *Per-node file:* the eight substrate levels and `occupancy`.
- *Viewer:*
  - a substrate mode on the map, colouring nodes by one type's level (a key cycles the types);
  - the fast region marked in the memory strip.
- *Stats CSV:* fast memory held, archive held, and archive stores and loads.
- *`nova land`:* the number of distinct substrates and each type's level shares.

**W9. Keys, the configuration page, determinism, snapshots.**
- *Keys:* `substrate.*`, `mem.fast_share`, `mem.fast_volatility`, `world.archive`, `archive.*`, `res.archive.*` and `spark.activity`.
- *Every feature is a toggle and every number a knob (Damian).* All of them appear on the configuration page (`docs/config-plan.md`), set before a run begins and fixed for the whole run (Damian). So any run can switch any part off or retune it, instead of staging runs:

  | key | on the page |
  |---|---|
  | `substrate.enabled` | checkbox |
  | `substrate.smooth`, `.scale`, `.floor` | knobs |
  | `substrate.levels` | five knobs, one per level's share |
  | `mem.fast_share` | knob (0 = no memory tiers) |
  | `mem.fast_volatility` | knob |
  | `world.archive` | knob (0 = no archive) |
  | `archive.rot`, `archive.slow` | knobs |
  | `spark.activity` | knob (0 = today's rule) |
  | founders by origin (W7) | always on (it only observes) |

- *Defaults:* on in the presets. Off in the test suites' legacy defaults (`substrate.enabled = false`, `mem.fast_share = 0`, `world.archive = 0`, `spark.activity = 0`), so every existing hash holds.
- *Determinism:* everything is per node and in fixed order. The warp kernel gets each node's tier boundary and multipliers. The same hash at 1, 4 and 20 threads, under shuffled dispatch, through a snapshot, and on either device.
- *Snapshots:* one format bump shared with the disasters (version 4). It carries per-node substrate levels and tier boundaries, plus the archive's contents, owners and stamps. Earlier versions load with tiers and substrates off.

## 2. Steps
One build with the disasters plan and the configuration page, in this order:
1. **The lineage tracker's fixes** (disasters X12) and **founders by origin** (W7).
2. **The key registry** (`docs/config-plan.md` step 1), so every new key below goes straight into it.
3. **Sparks by activity** (W5).
4. **Substrate fields and levels** in the generator (W2), with `nova land`'s report and the per-node columns.
5. **The multipliers** on P and E cores, the GPU and the NPU (W1).
6. **Disk tiers** (W4), with dormancy reserves.
7. **The disasters** (their own steps 1–6), with X5 amended by W4.
8. **Memory tiers** (W3), last as agreed in review: the CPU first, then the warp kernel.
9. **The configuration page** (`docs/config-plan.md`), the viewer and stats (W8), and the presets (W6).
10. **Tests**, then a short measured check of speed and founders by origin. Then the next long run with everything on.

## 3. Tests
- Everything off is today's world exactly: the same hashes in every suite.
- *Substrates:*
  - each type's level shares are exact;
  - neighbours differ by far less than random nodes do;
  - a recovered node takes its site's substrate;
  - the same seed gives the same map.
- *Multipliers:* a P core at 2× runs twice the instructions a tick, and at the floor a tenth. The same holds for E cores, GPU throughput and NPU multiply-adds.
- *Memory tiers:*
  - a copy costs each tier's price;
  - the fast selector lands only in fast memory, and fails with no room;
  - fast memory is wiped more often;
  - a warp's reads cost by tier.
- *Disk tiers:*
  - archival holdings rot slower and cost more;
  - mixed holdings address seamlessly;
  - DIVIDE, ENGULF, dispersal and dormancy carry tiers;
  - a disk failure takes one tier.
- *Sparks:* the rate follows the occupancy curve; an empty node sparks at the full rate and a full one almost never.
- *Founders:* a spark-born founder is recorded as spark, and so on for each origin.
- *Determinism:* a busy landscape with everything on gives the same hash at 1, 4 and 20 threads, under shuffled dispatch, through a snapshot, and on either device.

## 4. Risks
- **Scope:** this is the biggest build since Phase 9. The tiers touch the interpreter, the warp kernel, the cost tables and the disk ledger. Each step lands with its own tests before the next starts.
- **Slow memory slows everything a little:** most memory becomes slow, so CPU programs lose some pace, while GPU programs gain in fast memory. The prices come from measurements. If the world turns sluggish, `mem.fast_share` is the knob.
- **2× regions are simply better places:** they'll be crowded and contested. That's intended: rich land and poor land, as in any ecology.
- **Full nodes may never spark again:** that's intended (Damian). New origins come where disasters, extinctions or recoveries empty a node.
- **Speed:** a 2× world runs at about half speed. The tiers add one boundary check per memory access, and substrates cost nothing measurable. The first hour of the run measures all of it.
- **Substrates present vs possible:** of 390,625 possible substrates, at most 1,000 can exist at once (likely a few hundred distinct). That's enough variety for niches. A bigger world would show more.

## Result (2026-11-07)

Implemented substrates and their speed multipliers, activity-based sparks, founder origins, disk and memory tiers, viewer modes, and the larger landscape preset. Other preset sizes are unchanged pending Damian's decision. See design sections 6.19–6.21 and results/final-integration.md for verification and measurements.
