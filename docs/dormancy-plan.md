# Plan: dormancy and drift

> **Revision 2, APPROVED by Damian (2026-11-07), with D7 changed:** node collapse comes only from write wear, and the starvation rule is removed (revision 1 kept collapse unchanged). A world rule change, behind keys, measured before it becomes the default. This replaces `docs/node-life-plan.md` (L1–L5), which Damian set aside.
>
> - **Why:** a node fills up and its programs suffocate. If its neighbours are full too, there's nowhere to go and the node wilts, then collapses (every collapse measured so far is starvation of a full node), taking its lineages with it. 85% of daughters dispersed to a neighbour die on arrival, because it has no free CPU (`docs/node-life-plan.md` section 0 has the numbers).
> - **The idea (Damian):** like a bacterial spore, a suffocating program goes dormant. It leaves its cores behind, freeing its node instead of taking from the next one, and drifts at random from node to node. It wakes where there's room, taking new cores of the same kind there. It has a limited time to find a home, and while resting it's easy food.
> - **Source of truth:** `docs/design.md` 4.4a E (cores never travel between nodes), 6.9 (dispersal, transit, arrival), 6.10 (the lifecycle), Phase 10's death rules and bites (6.16: `life.frozen_ticks`, ENGULF).

## 0. Decided with Damian

1. **The only trigger, for now:** a body the watchdog would kill as frozen goes dormant instead. That's a body with millicores and a live thread that hasn't run an instruction for `life.frozen_ticks` (200 ticks), typically stuck in upkeep debt.
2. **The clock:** a dormant program slowly loses its memory beyond its own code, then its disk holding. With neither left, it dies. The clock never eats its code.
3. **Predation:** while resting in a node's memory a dormant program can be bitten or swallowed like anything else, and it can't react or defend itself.
4. **The movement:** hop by hop along paths, at random, deterministic, outside the edges' cargo limit.
5. **Collapse:** from write wear only; the starvation rule goes (D7).

## 1. Design calls

**D1. Going dormant.** At the death sweep, instead of dying, a frozen body:
- *leaves behind:* its millicores and GPU/NPU units go back to its node's pools, keeping their heat, as a dispersing daughter's share does. Its memory becomes waste with its contents kept, as any death's remnants do. Its disk contents stay as fossils;
- *keeps:*
  - its **code** (its main block: the one its first thread started in, copied as cargo);
  - its **memory reserve** (the codons in its other blocks: allocated space, data, engulfed parts);
  - its **disk reserve** (the disk units it held);
  - what it held of each kind of core (P and E millicores, GPU and NPU units);
  - its form (CPU- or GPU-born), its thread's preferred kind, and its lineage tag;
- *departs at once* to a random neighbour. It doesn't rest at home first: home has no room, and the cores it just freed would wake it right there. A program on a hermit node (no paths) rests at home instead.
- A frozen body with no memory reserve and no disk reserve dies as now. Under decision 2 it has no time to search.
- *Not triggers, for now:* bodies that lose their last millicore (`died_worn`, 29% of deaths), and arrivals that find no free millicore. Both still die, and either could become a trigger later.
- *Recommendation:* as above.

**D2. The clock.**
- Each tick, drifting or resting, a dormant program loses `dormant.mem_loss` codons of its memory reserve. Once that's empty it loses `dormant.disk_loss` units of its disk reserve each tick. With both empty it dies (`dormant_expired`).
- Its code is damaged only by the existing transit corruption on each hop, by bites, and by memory hazards while resting.
- *Defaults:* set in step 1 from the reserves frozen bodies actually hold, so that a typical one survives some tens of hops. A hop costs 1–20 ticks of latency plus a short rest.
- *Recommendation:* linear loss as above, defaults from step 1's data.

**D3. Drift.**
- Leaving a node, a dormant program picks a neighbour uniformly at random from that node's random stream, in the barrier's fixed order. It travels as cargo with the path's latency and bandwidth, like a dispersing daughter, but outside the edge's capacity limit.
- Transit corruption applies to its code on each hop.
- Fallow nodes have no paths, so drifters never enter one; a path that goes down drops what it carries, as now.
- *Recommendation:* as above.

**D4. Landing and resting.**
- On arrival, if the node has a waste run long enough for its code (found as an arrival finds hers), it lands there as a resting dormant body. That body doesn't run and holds no cores. It stays up to `dormant.dwell` ticks (default 15), then moves on to a random neighbour.
- With no room in memory it passes straight through: it departs at once, a hop with no rest.
- *Recommendation:* as above.

**D5. Waking.**
- At each barrier, a resting dormant program wakes if the node has at least one free unit of its kind: a millicore of a kind it held for a CPU-born program (P or E), a GPU unit for a GPU-born one.
- It claims up to what it held at home of each kind, capped by what's free here. It also claims disk up to its remaining disk reserve, empty. Its remaining memory reserve is dropped: it was the program's fuel.
- It becomes an ordinary body with one CPU at its start and registers zero, as an arrival does, in its own form.
- Several waking on one node go in fixed order (landing tick, then source node, then send order).
- Waking on a node its lineage wasn't founded on counts as a colonist, as arrivals do (the `colonist` event, `colonised`).
- *Recommendation:* as above.

**D6. Predation and hazards while resting.**
- A resting dormant body is in memory like any body, so ENGULF's bites and swallows apply. It has no surface shield, and it can't respond.
- A bite takes the piece at the point of contact, which may damage its code, and the same share of its reserves (as a live body loses the same share of its millicores). A swallow kills it, and the eater gains it as usual.
- Memory hazards apply as to any living body (bit rot, volatility, a memory bank shutting from heat). Decay doesn't: its code counts as living.
- *Recommendation:* as above.

**D7. Collapse from write wear only (Damian).**
- The starvation rule is removed: the meter, `node.starve_below`, `node.starve_ticks` and `node.starve_ebb`. A full node no longer collapses for being full.
- Collapse comes only from the existing wear rule: a node's writes reaching `node.endurance`, with the gradual decline before it (rising hazards, the health reading).
- Node health becomes wear alone.
- Snapshots: the meter leaves the format (version 3); a version 2 snapshot still loads, with its meter read and dropped.
- *Consequence:* at today's `node.endurance` (10⁹ writes) no node wore out in 40,000 ticks, so collapse and recovery will all but stop. Step 5 measures the wear rate; lowering `node.endurance` is the knob if renewal is wanted.
- A collapse kills the dormant programs resting on that node, like everyone else.

**D8. Determinism and snapshots.**
- Every choice comes from a node's stream, in the barrier's fixed order.
- Drifting and resting dormant programs go into snapshots: code, reserves, dwell left, kinds held, form, lineage tag and hops.
- *Recommendation:* as above.

**D9. Keys.**
- `life.dormancy` (on or off), `dormant.mem_loss`, `dormant.disk_loss`, `dormant.dwell`.
- Off in the test suites' legacy defaults, so every existing hash holds. On by default only if step 5's runs show the benefit.
- *Recommendation:* as above.

**D10. Seeing it.**
- New stats CSV columns: `dormant_started`, `dormant_woke`, `dormant_expired`, `dormant_eaten`, `drifting`, `resting` and `hops`. They come alongside the collapse counters (by cause) and the arrivals that found no free millicore, from the analysis.
- The per-node file gets resting dormant programs; the viewer's memory strip marks them; `nova resume` prints the totals.
- *Recommendation:* as above.

## 2. Steps
1. **The counters and the data:** the collapse and arrival counters in the CSV. Measure the memory and disk reserves frozen bodies hold, to set D2's defaults. Remove the starvation rule (D7).
2. **Dormancy:** going dormant, the clock, drift, landing, resting and waking (D1–D5), with snapshots (D8) and keys (D9).
3. **Predation** on resting dormant programs (D6).
4. **Tests,** then the counters and the viewer (D10).
5. **Measure:**
   - from the 35,000 snapshot (10,000 ticks) and from the start (40,000 ticks), with dormancy off and on;
   - collapses by cause; frozen bodies going dormant; how many wake, after how many hops, and where;
   - colonist events and `colonised`; bodies, lineages and gated replicators; deaths by cause; the cost in time.
   - Then the defaults, design.md (4.4a, 6.9, 6.16) and the README.

### Result (built 2026-11-07)
- **Built as approved.** Design.md 6.17 has the full account. Two details were settled in the build:
  - Its upkeep debt is cleared when it goes dormant, so it starts afresh, as an arrival does.
  - A dormant program passing through gets a fresh send number, so its transit corruption is its own on each hop.
- **Defaults:** `life.dormancy` on; `dormant.mem_loss` 1 and `dormant.disk_loss` 1 codon a tick; `dormant.dwell` 15 ticks. Frozen bodies hold 67 codons of memory and 6 of disk in reserve on average, so a typical dormant program has about 70 ticks to find a home.
- **Measured** (`snap.35000`, starvation collapse gone in both runs):
  - Over 2,000 ticks: births +11%, deaths −7%; 92% of dormant programs woke.
  - Over 5,000 ticks: births 191,591 → 230,843 (+20%), deaths 1,110,219 → 992,868 (−11%). Of 190,163 programs that went dormant, 177,981 woke (94%), 11,547 expired and 40 were eaten, over 265,705 hops (about 1.4 each). 192,026 frozen bodies were bare, with no reserves, and died.
  - Speed is unchanged (152.4 s against 152.6 s).
- **Tests:** the ecology suite's new checks pass (195 in all); every other suite runs with dormancy off and keeps its hashes.
- **Not done:** a viewer mark for resting programs in the memory strip (D10). The per-node file has no `resting` column yet; the stats CSV has the gauges.

## 3. Tests
- `life.dormancy` off is today's world exactly: the same hashes in every suite.
- Suite `ecology`:
  - a frozen body with reserves goes dormant (its cores freed at home, its code intact), and one with none dies;
  - the clock eats memory, then disk, then kills;
  - it passes through a node with no room, rests in one with room, wakes when a core of its kind frees there, and moves on after its dwell;
  - a GPU-born dormant program doesn't wake on millicores alone;
  - a resting one can be bitten and swallowed, and never bites back.
- Determinism: a busy landscape with dormancy on gives the same hash at 1, 4 and 20 threads, under shuffled dispatch, and through a snapshot taken with programs drifting and resting.

## 4. Risks
- **Bare programs can't go dormant** (decision 2: no reserves, no time). If most frozen bodies are bare, dormancy will rarely happen. Step 1's data shows how many; a small grace period would be the knob, if needed.
- **Resting dormant programs take memory** on the nodes they pass. That's intended (a full node can't host visitors), but memory, as well as cores, decides where they can stop.
- **A random walk can be slow** through a crowded region on a 1,000-node landscape averaging 3.5 paths a node. The clock bounds the cost. A bias toward emptier neighbours would be a later tweak, if needed.
- **Frozen is a narrow trigger:** it catches programs stuck in upkeep debt, not ones that lose their last millicore. If step 5 shows little effect, widening the trigger is the next question for Damian.
