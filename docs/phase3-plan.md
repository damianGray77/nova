# Phase 3 plan: the world

> **Revision 1, APPROVED by Damian as recommended (2026-11-05) and BUILT.** What the engine does, and the first short runs, are in `docs/design.md` 6.9. Details settled while building: node-property noise is rank-normalised over the nodes, so each property spans −1…1 evenly; the bandwidth factor is 1 − 0.75 × the link's ruggedness rank; latency's default scale is 20 ticks per unit of distance; and the lineage tag in cargo is detector data, not hashed or snapshotted. The text below is the plan as approved.
>
> - **Source of truth:** `docs/design.md`. Phase 3's scope is in 9.1; the world itself is decided in 4, 4.1–4.3 and 4.4a (B, E, F, G).
> - **Long runs:** checked with runs of minutes; the 12-hour runs wait until everything is built (9.1).

## 0. Scope (design.md 9.1)
- A ~100-node graph from procedural generation, with resource and climate noise fields.
- Edges; the dispersal exchange and transit; many origins; node properties.
- *Milestones:* first colonists, first dispersal strategies.

**Phase 4, not here:** transit corruption and link failures, the node lifecycle (wear, pool-starvation collapse, creation and recovery by drifting noise), volatility and disk, the ambient heat leak, and signals. The UI is Phase 5.

## 1. What the design already fixes
- **Generation (4.1, 4.4a G):** nodes scattered in a hidden 2D space that organisms never see. One coherent noise field per property gives biomes with gradients. Proximity links (k-nearest); a ruggedness field removes or weakens edges, giving plains, chokepoints and isolated pockets; a few sparse long-range links.
- **Node properties (4.4a B):** capacity per resource, cooling rate (climate), spark rate ("warm pools"), background mutation ("radiation"). Max population drops out because it emerges from memory. Volatility and write endurance belong to Phase 4.
- **Edges in transit (4.4a F):** a dispersing daughter is cargo with no CPU. Bandwidth limits how fast she crosses (big daughters take longer), latency delays arrival, and capacity caps simultaneous transfers.
- **The exchange rule (4.4a E):** DIVIDE with BX = neighbour k sends the daughter away. Her share is released into the origin's pool, since the parent pays at home. On arrival she draws the same amounts from the destination's pools, capped by what's there. Her body needs waste at the destination to overwrite; with none, she dies.
- **Determinism (7.1 D):** a node touches only its own state and its own outboxes during a tick. Cross-node effects apply at the barrier: destination node id, then source node id, then send order.
- **Origins (6.3, 6.4 #23):** many origins (a soup on every node) by default; a single origin is a config option.

## 2. Design calls

**C1. Generating the graph.** `world.graph = procedural` (new); `ring` stays for the demo world and every existing config.
- Nodes are scattered in the unit square by jittered-grid sampling, so they're even but not regular.
- Links: each node to its `world.links` (default 4) nearest; links are symmetric (two directed edges).
- The ruggedness field removes links where it runs high (`world.ruggedness`, the fraction cut, default 0.2), leaving pockets and chokepoints. Isolated nodes are allowed; they become refuges or lifeless pockets.
- `world.long_links` (default 2% of nodes) extra links between distant random pairs: the rivers and trade routes.
- Noise: hand-rolled 2D gradient noise with octaves (fBm), seeded per field from the world seed. No library.
- *Recommendation:* as above.

**C2. Node properties from noise.** Each property keeps its global key as the mean, plus a `vary.<property>` knob f: node value = mean × (1 + f × noise), with noise in −1…1 from that property's own field.
- Properties: `soup_size` (memory), `millicores`, `gpus`, `npus`, `cool` (climate), `spark_rate`, `radiation` (scales the copy, flaw and rot rates together).
- `vary.* = 0` (the default) gives uniform nodes.
- `configs/world.cfg` uses 0.5 on all of them, so biomes differ by up to ±50%.
- Each node then gets its own `SoupParams`. Live knobs (spark rate and so on) apply to every node, times its factor.
- *Recommendation:* as above.

**C3. Edges.**
- *Latency:* 1 + floor(hidden distance × `edge.latency_scale`) ticks, so near neighbours are quick and long-range links slow.
- *Bandwidth:* codons per tick, `edge.bandwidth` (default 32) scaled down by the ruggedness field (×0.25–1), so rough country is slow to cross.
- *Capacity:* simultaneous cargoes per directed edge, `edge.capacity` (default 2).
- Corruption and failures come in Phase 4.
- *Recommendation:* as above.

**C4. Dispersal through DIVIDE.**
- *Numbering:* neighbours are numbered 1…degree, nearest first by hidden distance. That's the nearest-to-self rule; organisms see only the numbers.
  - BX 0–3 goes through the DIVIDE selector map (here plus the three nearest, shuffled in the control).
  - BX ≥ 4 is neighbour BX directly, so far neighbours are reachable by arithmetic.
  - A number past the node's degree fails, as now.
- *At the origin:* the daughter block's codons become cargo, and the block becomes waste, keeping its contents. Her share — floor(holding × CX/64) of millicores, GPU and NPU units — goes into the origin's pools. If the edge's capacity is full, the DIVIDE fails and nothing changes.
- *On arrival* (at the barrier, in the fixed order):
  - she is placed at a random waste run of her length on the destination, chosen by the destination's RNG; with no room she is lost;
  - she draws her share from the destination's pools, capped by what's there;
  - she gets one CPU at her start with registers zero, and runs from the next tick;
  - with no millicore she dies in that tick's sweep.
- *Recommendation:* as above.

**C5. Transit.** Each directed edge holds a queue of cargoes, at most `edge.capacity` at once. A cargo of L codons arrives at send tick + ceil(L / bandwidth) + latency. Queues are world state: hashed, snapshotted, delivered in the fixed order. Cargoes on one edge don't slow each other; capacity is the limit.

**C6. Origins and seeding.**
- `world.origins = all` (default: every node a random soup with sparks), or `one` (only node 0 gets sparks; the others start as lifeless random waste until colonised).
- `seed.node = all` (as now) or k, for seeded tests: one founder spreading from one node.

**C7. Sensing.** In multi-node worlds, SENSE reading 14 = my node's degree (how many neighbours I could disperse to). Single-node worlds keep their reading count, so they stay unchanged.
- Sensing a neighbour's state is left to signals (Phase 4), keeping the world's information local.
- *Recommendation:* as above.

**C8. Detection across nodes.**
- Lineage ids become world-unique (founding node in the top bits). A dispersing daughter's tag (lineage, depth, genotype) travels in her cargo and is adopted on arrival, so a lineage spans nodes. Each node keeps the members it holds.
- The gate stays per node, as now.
- New event `colonist`: the first time a member of a lineage that has met the gate arrives alive on a node other than the one it was founded on. That's the 9.1 milestone "first colonists".
- CSV:
  - world columns `sent`, `arrived`, `lost` (no room) and `in_flight`;
  - `colonised` (nodes holding a living member of a lineage founded elsewhere);
  - a per-node file (`output.nodes`) with one row per node per window: bodies, free memory and units, lineages, arrivals and departures, the node's properties.
- "First dispersal strategies" is read from the data: genomes that disperse and how far they get.

**C9. Running worlds.** A world run is one process using all threads (`run.threads = 0`).
- `configs/world.cfg`: 100 nodes, procedural, `vary.* = 0.5`, many origins, 16,384 codons and 2,000 millicores per node (mean).
- The harness gets `-Threads` per trial, and its parallelism becomes Percent × cores ÷ Threads.
- Bench: a `world` workload, 100 procedural nodes of random soup on all threads.

**C10. Determinism and compatibility.**
- A single-node world, and any `world.graph = ring` world without dispersal, is byte-identical to Phase 2b: the pinned hashes stay.
- Per-node properties are derived from the seed and config, so snapshots need only the config plus the cargo queues.
- The determinism gate gets a procedural multi-node world with dispersal and colonists.

## 3. What gets built
- **World:** procedural graph and noise fields; per-node `SoupParams`; edge latency, bandwidth and capacity; cargo queues and barrier delivery; hash and snapshot of the queues; `world.origins`, `seed.node`.
- **Interpreter:** DIVIDE to a neighbour (the exchange rule, cargo out, edge-full failure); SENSE 14.
- **Detection:** world-unique lineage ids, tags in cargo, adoption on arrival, the `colonist` event, the new CSV columns and per-node file.
- **Harness, bench and configs:** `-Threads`, the `world` workload, `configs/world.cfg`.

## 4. Evidence
- `verify.ps1` green, covering:
  - graph generation (deterministic from the seed, k-nearest, ruggedness pockets, long links, node properties within their ranges);
  - dispersal (the exchange rule at both ends, no room, edge full, bandwidth and latency timing, numbering);
  - SENSE 14;
  - lineages across nodes and the `colonist` event;
  - single-node and ring worlds unchanged;
  - the determinism gate on a procedural world with dispersal at 1/4/20 threads and shuffled dispatch;
  - snapshot resume with cargo in flight;
  - event replay.
- Manual: a few-minute world run (many origins, and one origin with a seeded founder) for first colonists; a bench of the `world` workload.

## 5. Docs
- design.md: a new 6.9 (Phase 3 semantics), 4.4a (built), 9.1 and the status line.
- README.
