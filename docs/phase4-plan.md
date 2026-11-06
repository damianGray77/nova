# Phase 4 plan: the individual features

> **Revision 2, BUILT (2026-11-05).** What the engine does, and the short checks, are in `docs/design.md` 6.10. Details settled while building: signals have a hard reach (`signal.reach`, 8 hops) besides the floor; complexity for salvage is genome length (C22); a recovered node's detector is re-attached after its soup is rebuilt. The text below is the plan as revised.
>
> Revision 1 was approved by Damian as recommended (2026-11-05). During the build he changed two things (2026-11-05): a collapse leaves remnants behind instead of erasing code, and a new salvage rule stops a much smaller program from taking a bigger program's code whole (step 4, C22–C23; C15 and C16 revised). His choices: graded pieces, applied to reading remnants and to engulfed prey (not to claiming memory), and ruins that come back weathered. Steps 1–2 are built.
>
> - **Source of truth:** `docs/design.md`. Phase 4's scope is in 9.1; the features are decided in 4.4a (A, C, D, F), 4.5 with 6.4 #17 (the ambient leak), and 5.1 (A, B, G).
> - **Long runs:** checked with runs of minutes; the 12-hour runs wait until everything is built (9.1).

## 0. Scope (design.md 9.1)
"Add individually, each its own experiment":
1. the ambient heat leak;
2. edge corruption and link failures;
3. the disk tier, and volatility wipes with repair from disk;
4. remnants and salvage: dead code remembers whose it was, and a much smaller program gets only pieces of it (added by Damian during the build);
5. the signal channel;
6. the node lifecycle: wear, starvation collapse, and creation and recovery as one mechanism.

*Milestones:* succession on a recovered node; first sustained signal use.

**Not here:** the archival disk tier and the fast cache (deferred in 5.1 B, C), the GPU divergence penalty (5.1 D), the UI (Phase 5), 1,000+ nodes and parallel worlds (Phase 6).

## 1. What the design already fixes
- **Ambient leak (6.4 #17):** heat is per owned unit, plus a weak node-level ambient leak, set low. It revives 4.5's temporal niche partitioning without overriding ownership.
- **Edges (4.4a F):** transit corruption flips bits per codon crossing. Link failures are occasional disasters, and a daughter in flight on a failed link is lost.
- **Disk (5.1 B):** one tier, owned via ALLOC (disk type), with slow bit rot. DISK selectors: 0 = compare (mismatch count into CX), 1 = store, 2 = load. Costs are CPU time proportional to the amount moved. At death, disk holdings return to the pool and their contents become waste ("fossils").
- **Volatility (5.1 A):** wipes hit random address ranges of bank-like size. Wiped contents become random codons, not zeros. A partial hit can be repaired from the body's disk copy (compare, then load); a whole-body hit is death. There is no boot-from-disk rule. The rate is a node property.
- **Signals (5.1 G):** emissions on a channel add up (superposition). They are attenuated by a fixed factor per hop, delayed by edge latency, and dropped below a per-node noise floor. Signals last one tick; loudness (EMIT's CX) costs heat. Channels are public, never owned. A fixed number per world, picked by BX.
- **Lifecycle (4.4a A, C, D):**
  - Write endurance wears a node down gradually: error rates and volatility rise before the final collapse, and node health is a SENSE reading.
  - Pool-starvation collapse: the free pool below a threshold for N consecutive ticks.
  - At collapse every program on the node dies; only daughters already dispersed survive. (4.4a C said its code is lost; *revised by Damian during the build:* it stays behind as remnants, C15.)
  - A node appears wherever a drifting noise field says "land", under a capacity budget (spawn ≈ collapse). A recovered node takes its profile from the noise as it is now, gets fresh endurance and recomputed edges, and starts as a fresh primordial soup.
- **Determinism (7.1 D):** nodes touch only their own state during a tick; everything cross-node happens at the barrier in a fixed order.
- **Mutation-reachable use (6.2):** every feature needs a way to sense and use it, or it is scenery. The opcodes already exist (DISK 60, EMIT 58, LISTEN 59, the disk selector of ALLOC/RELEASE); today they fail.

## 2. Design calls

**C0. How it's built.** One plan, six steps in the order of section 0: smallest and most local first, the lifecycle (which makes the graph dynamic) last.
- Every feature has its own switch, off by default. With all of them off, every world is byte-identical to Phase 3: all pinned and gate hashes stay.
- Each step is built, tested and verified before the next.
- The short checks at the end run each feature as its own paired experiment (on vs off) on `configs/world.cfg`.
- *Recommendation:* as above.

### Step 1: the ambient heat leak

**C1. The leak.** `heat.ambient` (default 0 = off; suggested 0.05).
- Each tick a node's ambient input = `heat.ambient` × the mean CPU heat input over all its millicores, held or free (idle units count as 0).
- That input is added to every CPU unit, memory page and GPU/NPU unit on the node, held or free, in the heat update. So one body running flat out warms everyone a little.
- At the suggested 0.05, a fully busy node adds about 0.3 heat a tick, settling near 3 at the default cooling: weak, as decided.
- No new SENSE reading; the existing heat readings include it. CSV column `ambient` (mean input).
- *Recommendation:* as above.

### Step 2: edge corruption and link failures

**C2. Transit corruption.** `edge.corruption` (default 0; suggested 2 × 10⁻⁴): the chance per codon per tick in transit of one random bit flip.
- Exposure is codons × ticks in flight, so long and rough links (slow, high latency) are riskier: distance has a price besides time.
- Sampled at delivery as a Poisson process over the exposure, from a stream keyed by the cargo's send number, so it doesn't depend on delivery order.
- A 32-codon daughter crossing in 5 ticks at the suggested rate is hit about 3% of the time.
- CSV column `corrupted` (codons flipped).
- *Recommendation:* as above.

**C3. Link failures.** `edge.failure` (default 0; suggested 10⁻⁴): the chance per link per tick of going down, both directions together. Downtime is exponential with mean `edge.downtime` (1,000) ticks.
- Decided at the barrier, links in id order, from a world-level stream for that tick.
- While a link is down, DIVIDE to it fails (the error flag shows it). Neighbour numbering and SENSE 14 don't change.
- Cargo in flight on a link when it fails is lost (CSV `transit_lost`).
- Link state is world state: hashed and snapshotted. CSV column `links_down`.
- *Recommendation:* as above. (A "live neighbours" SENSE reading is possible, but IFERR already lets a program notice a dead link.)

### Step 3: the disk tier and volatility

**C4. The disk.** `world.disk` codons of disk per node (default 0 = no disk; everything disk fails as now; suggested 4,096, a quarter of memory). A node property: `vary.disk`.
- *Contents:* the disk starts as random codons, all unowned, like memory.
- *Addressing:* relative to my own disk. A body's disk holdings are its disk blocks concatenated in the order it claimed them; offset 0 is the start of the first. So "store myself at offset 0" works wherever the body and its disk sit, which keeps genomes position-independent.
- *ALLOC disk* (resource selector 3): claims CX codons of free disk, the first free run from a random start, wrapping once, as sparks do. AX ← the offset of the new block in my disk.
- *RELEASE disk:* gives up the newest CX codons. The contents stay, as waste.
- *Fossils:* freed disk keeps its contents. Claiming that space and loading from it digs them up, just as ALLOC over waste does in memory.
- *Shares:* DIVIDE gives the daughter the newest floor(D × CX/64) codons, contents and all, splitting a block if needed. Dispersal releases that share at home and has her claim the same amount on arrival (empty, capped by free disk), as with every other holding. ENGULF takes all of the prey's disk. At death the disk is freed and the contents stay as fossils.
- *Bit rot:* `disk.rot` per codon per tick over the whole disk (default 10⁻⁶ when there is a disk: a few hundred times slower than the memory baseline, which is about 1.5–3 × 10⁻⁴ on the abio and world nodes).
- No disk heat in v1.
- *Recommendation:* as above.

**C5. The DISK instruction.** BX through a new 3-entry DISK selector map (0 compare, 1 store, 2 load; the shuffled control draws it, keeping compare off 0). AX = offset in my disk, CX = count, and the memory side is the read head (compare, store) or the write head (load).
- *compare:* CX ← mismatches between memory at the read head and my disk at AX, over CX codons.
- *store:* disk ← memory. *load:* memory ← disk.
- Heads advance over what they touched, as COPY does; AX is unchanged.
- Private code applies to the memory side, as for COPY (read: own body or waste; write: own body).
- All-or-nothing: it fails, changing nothing, if CX ≤ 0, the range runs past my disk, or the memory side isn't readable or writable.
- *Cost:* `disk.cost_base` (4) + `disk.cost_per_codon` (0.25) × CX instructions, like contact (6.4 #7).
- Loads are writes: page heat, the detector's written marks, and node wear (C12).
- No copy errors on disk transfers; disk rot is their hazard. So repair is exact when the disk copy is intact, and fails when it has rotted: nothing grants permanence.
- *Recommendation:* as above.

**C6. Disk sensing.** New SENSE readings, appended: 15 = my disk codons, 16 = free disk codons. They fail without a disk. (Readings that exist are counted up to the highest enabled one, so reading 14 is present but fails without dispersal.)
- *Recommendation:* as above.

**C7. Volatility.** `mem.volatility` (default 0; suggested 10⁻⁵): the chance per memory page per tick that a wipe starts there. A node property: `vary.volatility`.
- A wipe covers 1–`mem.wipe_pages` (4) whole pages, page-aligned and clipped at the soup's end.
- Wiped codons become random codons. Ownership doesn't change: the body still holds the memory, now garbage, and can repair it from disk.
- *Whole-body hit (5.1 A):* a body all of whose owned memory was wiped this tick has its CPUs halted, so it dies in the sweep. That's the one definition of death (6.4 #1).
- The detector clears written marks on wiped codons.
- At the suggested rate, a 256-page node is hit about every 400 ticks.
- CSV columns `wipes`, `wiped`, `wipe_deaths`.
- *Recommendation:* as above.

**C8. Disk milestone.** Event `disk_user`: the first DISK load by a member of a gated lineage that writes into the block it runs in, i.e. restoring its own code from disk. CSV columns `disk_stores`, `disk_loads`, `disk_compares`.
- *Recommendation:* as above.

### Step 4: remnants and salvage (added by Damian during the build)

**C22. Stamps: dead code remembers whose it was.** `salvage.ratio` K (default 0 = off; suggested 4). With it on, every memory codon and every disk codon carries a stamp: the complexity of the program whose code it was, 0 for none (random chemistry, or a program's own writing).
- *Complexity* is genome length: the length of the program's first block, its own code. (Damian had no preference. Total size would not work for engulfing: an eater's size includes its prey the moment it engulfs.)
- *Stamped* when code is left behind: at death, at RELEASE, when heat shuts a page, and when a disk share is released. Each codon with stamp 0 takes the program's genome length; one already stamped keeps its stamp, because it is still that other program's code. At ENGULF the prey's codons are stamped with the prey's genome length.
- *Cleared* when a codon is written (COPY, WRITE, INJECT, GPU and NPU outputs, DISK loads and stores: it is now the writer's own), wiped, or claimed into memory by ALLOC, a spark or an arrival. Claiming memory is not salvage (Damian's choice), so whatever a claim lands on becomes wholly the claimer's.
- Claimed disk keeps its stamps, so loading an old disk's contents (fossils) is salvage.
- Bit rot doesn't change stamps.
- Stamps are world state: hashed and snapshotted when the rule is on.

**C23. The rule: graded pieces.** When a program takes a codon with stamp C > K × its own genome length r, the codon comes through intact with chance K × r / C, and otherwise as a random codon. At or below K × r there is no limit.
- *Taking* means a data read through a head: COPY, READ, INJECT's payload, GPU and NPU reads, DISK compare, store and load. Execution and template searches are not salvage: an engulfed prey's CPUs keep running its code.
- So a small program gets fragments of a big corpse or a big prey, intact runs getting rarer as the gap grows, and the whole genome essentially never survives. No cliff, no per-CPU state; a draw from the node's stream only when a stamped codon is read.
- The program isn't told (no error flag): the copy simply comes out garbled.
- CSV column `garbled` (codons the rule replaced).
- *Recommendation:* as above.

### Step 5: the signal channel

**C9. EMIT and LISTEN.** `signal.channels` (default 0 = off: both fail as now; suggested 16). BX = channel; a BX past the count fails.
- *EMIT:* adds CX (signed) to the node's emission sum for that channel this tick. Cost: `signal.cost_base` (2) + `signal.cost_per_amp` (1/32) × |CX| instructions. Instructions are CPU work, so loudness costs time and heat (5.1 G) with no new heat path.
- *LISTEN:* CX ← the channel's field on my node this tick, a signed sum.
- *Signals last one tick:* LISTEN during tick t hears emissions from before t: my own node's from t − 1, and neighbours' later, by latency. Within a tick, hearing therefore doesn't depend on the order bodies run in.
- The `nova_v1_inv2` control table has no EMIT/LISTEN (codons 58–59 are its doubled ENGULF/INJECT), so it has no signals. That's fine for a control arm, and worth knowing.
- *Recommendation:* as above.

**C10. Propagation.** At the barrier, each node's emission sums go into a short history.
- The field heard at node m, channel c, during tick t is the noise plus the sum, over sources e within reach, of attenuation^hops(e, m) × E_e(t − delay(e, m)).
- *Paths:* hops = fewest hops; delay = 1 + the latency along the quickest of those fewest-hop paths (my own node: 0 hops, delay 1).
- *Attenuation:* `signal.attenuation` (0.25) per hop.
- *Floor and noise:* a contribution below `signal.floor` (1) is dropped. Every channel on every node also carries background noise, uniform in −floor…floor, from the node's stream. `vary.noise` spreads the floor over a noise field ("it could follow biome noise").
- *Reach:* sources within the hop count where attenuation drops the loudest representable signal below the floor. Paths are recomputed when the graph changes (step 5); a down link (C3) carries no signal.
- Flooding along fewest-hop paths has no echoes (a node's signal never comes back to it amplified), so the field can't run away.
- The field is computed serially at the barrier. The history buffers are world state: hashed and snapshotted.
- *Recommendation:* as above.

**C11. Signal milestone and the researcher's tap.**
- Event `signaller`: the first gated lineage whose members EMIT on `signal.sustained` (100) distinct ticks on one node. That reads 9.1's "first sustained signal use"; random code emits all the time, so the gate is what filters it.
- CSV columns `emits` and `listens`.
- `output.signals` (optional): per window and channel, emissions, total |amplitude| and the mean |field| over nodes. That's the tap; nothing is ever routed to speakers.
- *Recommendation:* as above.

### Step 6: the node lifecycle

**C12. Wear.** `node.endurance` codon writes per node (default 0 = never wears out; a node property: `vary.endurance`). Memory writes (COPY, WRITE, INJECT, GPU and NPU outputs, DISK loads) and disk stores count.
- Wear w = writes / endurance. Copy, flaw and rot rates and volatility are multiplied by 1 + `node.decline` (4) × w²: gradual decline (4.4a C).
- At w ≥ 1 the node collapses.
- *Recommendation:* as above. Starting value: the world bench runs about 10⁴ instructions per node per tick, of which writes are a fraction, so a node writes perhaps 10³–10⁴ codons a tick, and 10⁹ gives a lifetime of 10⁵–10⁶ ticks. The short check measures and tunes it.

**C13. Starvation.** `node.starve_ticks` (default 0 = off) and `node.starve_below` (fraction of the node's millicores).
- If the free CPU pool stays below that fraction for that many consecutive ticks, the node collapses. The counter resets whenever the pool is back above.
- *Revised by Damian after the short check:* the count is a meter that ebbs like heat instead of resetting. Each tick it loses `node.starve_ebb` (0.00025) of itself and gains 1 if the node is starving; the node collapses when it reaches `node.starve_ticks`. Constantly starving it settles at 1 / ebb, so it fills only if the node starves more than starve_ticks × ebb of the time (half, at the defaults). The consecutive count rarely built, because CPU wear frees a few units nearly every tick.
- *A caution from the data:* busy abio soups run their pool at 0.25–0.8% of millicores (6.6, the D10 sweep), so a threshold at 1% would collapse every busy node. Suggested start: 0.2% for 2,000 ticks, tuned in the short check.
- *Recommendation:* as above.

**C14. Node health.** SENSE reading 17 = node health, 0–100: 100 × the lower of (1 − w) and (1 − starvation counter / starve_ticks). It fails when the lifecycle is off. "Leave a dying node" can then evolve (4.4a C).
- *Recommendation:* as above.

**C15. Collapse.** Detected during the node's tick and applied at the barrier, in node order:
- every body dies and its lineages end. These are ordinary deaths: the code stays behind as remnants in memory, and disk contents as fossils, stamped when the salvage rule is on (*revised by Damian*; revision 1 erased them);
- cargo in flight to or from the node is lost;
- the node goes fallow: no tick, no sparks, no edges;
- the graph is rebuilt among live nodes (C17).
- CSV columns `collapses`, `live_nodes`; events `collapse node=…`.
- *Recommendation:* as above.

**C16. Creation and recovery: one mechanism.** The generator's positions become *sites*. A site is either a live node or fallow ("sea").
- *Drifting land:* `world.land` (default 1 = every site is land: off) is the fraction of sites that are land at any moment. It is the top fraction of a land noise field sampled at the sites, with time as a third dimension at `world.drift` cycles per tick (suggested 10⁻⁵: the map reshapes over about 10⁵ ticks). This needs a 3D version of the gradient noise; still hand-rolled.
- *Who appears:* every `node.recover_every` (100) ticks, at the barrier, in site order, a fallow site becomes a node if all of these hold:
  - it has been fallow at least `node.fallow_min` (2,000) ticks;
  - the land field says land there now;
  - the live count is below the budget, `world.budget` (default: the live count at tick 0).
  So spawning tracks collapse, as decided.
- *At t = 0:* sites that aren't land start fallow. Creation and recovery are the same rule.
- *A new node* takes every property from the fields as they are now: each property field gets the same time dimension, rank-normalised over all sites at that tick. Live nodes keep their profile: hardware doesn't drift.
- *Ruins (Damian's choice):* the remnants are still there, weathered: each codon of memory and disk has flipped with the chance its rot rate gives over the whole fallow time (sampled at the return), so long-fallow ruins come back scrambled. Stamps stay. If the node's memory or disk size changed, the overlap is kept in place and the rest is random codons from a stream keyed by the site and its recovery count. A site that has never been a node (sea at t = 0) starts as random soup.
- *Otherwise a fresh start:* fresh endurance, a full pool, its initial sparks, and an empty detector.
- Event `recovered node=…`. CSV `recoveries`.
- *Recommendation:* as above. Fixed sites keep the coordinates, noise and determinism simple; a continuum of new positions could come later if wanted.

**C17. Rebuilding the graph.** When the live set changes:
- proximity links are recomputed among live sites (each to its k nearest live sites);
- a proximity link is cut where its ruggedness exceeds the threshold fixed at generation (the value at the `world.ruggedness` quantile then), so a cut is local and doesn't shift when far-off nodes come and go;
- long links are the site pairs chosen at generation, active while both ends are live;
- edges are renumbered and neighbour numbering is recomputed;
- cargo on a link that survives keeps flying (matched by its source and destination); cargo on a vanished link is lost;
- signal paths are recomputed.
- With every site live, this gives exactly Phase 3's graph. The rebuild code runs only when the live set changes, so worlds without the lifecycle never touch it.
- *Recommendation:* as above.

**C18. Lifecycle milestone.** 9.1's "succession on a recovered node" is read from the data: the per-node file gains `live`, `health`, `life` (the recovery count) and `since` (the tick it last appeared). The `colonist` event already marks the first gated arrivals on a node. The lineage and colonist events on a recovered node show who came, from where, and when local abiogenesis caught up, if it did.
- *Recommendation:* as above.

### Across the steps

**C19. Sensing and selector maps.** SENSE gains readings 15–17 (C6, C14). The readings counted are those up to the highest enabled one, and a reading for a feature that's off fails, as GPU and NPU readings do. New selector map: DISK (3 entries). The shuffled control draws new maps only when their feature is on, so the controls' draws are unchanged otherwise.
- *Recommendation:* as above.

**C20. Determinism and compatibility.**
- All features off: byte-identical to Phase 3.
- Each feature's state is hashed and snapshotted only when it is on: link states, the disk and its ledger, stamps, signal histories, live flags, wear, starvation counters and recovery counts. Per-node properties are re-derived from the config plus each node's last recovery tick.
- The determinism gate gets one world with every feature on, at 1, 4 and 20 threads and under reversed and shuffled dispatch.
- *Recommendation:* as above.

**C21. Running the experiments.**
- Harness arms for world trials: `leak`, `transit` (corruption and failures), `disk` (disk and volatility), `salvage`, `signals`, `lifecycle`, and `phase4` (all of them). Each sets the suggested values above.
- `configs/world.cfg` keeps the features off, and gains a commented block listing them.
- Bench: the `world` workload with all five on, to price them.
- *Recommendation:* as above.

## 3. What gets built
- **Heat:** the ambient leak.
- **World:** transit corruption, link failures, signal propagation and history, sites, collapse, recovery with drifting 3D noise, and graph rebuilds; hash and snapshot of each.
- **Soup:** the disk and its ledger, volatility, stamps and the salvage rule, wear and starvation tracking, ruins weathered on return.
- **Interpreter:** DISK, ALLOC/RELEASE disk, EMIT, LISTEN, SENSE 15–17; disk shares in DIVIDE, dispersal and ENGULF.
- **Detection:** `disk_user`, `signaller`, `collapse` and `recovered` events; written marks cleared by wipes.
- **Outputs:** the CSV columns above, the per-node file's new columns, `output.signals`, `result.txt` keys for the new milestones.
- **Harness, bench and configs:** the arms, the bench workload, the world.cfg block.

## 4. Evidence
- `verify.ps1` green, covering per feature:
  - *leak:* ambient input and its effect on idle units;
  - *edges:* corruption rate and exposure, failures and downtime, lost cargo, DIVIDE on a down link;
  - *disk:* the ledger, relative addressing, the three ops, costs, all-or-nothing, shares, fossils, rot, sensing;
  - *volatility:* wipe sizes and rates, whole-body death, repair from disk;
  - *salvage:* stamping and clearing on every path, the intact chance K × r / C, no limit within K, engulfed prey, disk fossils, claims unaffected;
  - *signals:* superposition, attenuation, delay, floor and noise, one-tick life, costs, no echoes;
  - *lifecycle:* wear and decline, starvation, health, collapse leaving remnants, ruins weathered on return, recovery timing and budget, graph rebuild equal to Phase 3 when every site is live, cargo on surviving and vanished links;
  - with everything off, every pinned hash unchanged; the determinism gate with all five on; snapshot resume with each feature's state; event replay of the new events.
- Manual, per feature: a short paired world run (on vs off, a few minutes each), plus one with all five, and a bench.

## 5. Docs
- design.md: a new 6.10 (Phase 4 semantics and the short checks), 9.1, the status line, and 4.4a / 5.1 marked built.
- README.
