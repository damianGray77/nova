# Nova — Design

> **Status:** Phase 0 (the skeleton and determinism gate), Phase 1a (the interpreter on one node; semantics in 6.5), Phase 1b (abiogenesis rules, detection and harness; 6.6), Phase 2 (the in-world GPU; 6.7), Phase 2b (a 16-op GPU and the in-world NPU; 6.8), Phase 3 (the world: procedural graph, node properties, dispersal and transit; 6.9), Phase 4 (the ambient leak, edge corruption and failures, the disk and volatility, remnants and salvage, signals, and the node lifecycle; 6.10) Phase 5 (the in-world GPU and NPU on the real devices through Direct3D 12 and DirectML, asynchronous and batched; 6.11), Phase 6 (the analysis UI, `nova view`; 6.12), Phase 7 (one big landscape world generated like a game level, and budgets mapped to the machine's real RAM, disk and devices; 6.13) and Phase 8 (programs on the GPU: warps in lockstep lanes, life born on a GPU or a CPU, on the real GPU or the CPU reference with the same results; 6.14) are built. The 12-hour paired experiments wait until everything is built (9.1).
>
> The design itself was settled on 2026-11-05: organism design (Section 6), the world lifecycle (Section 4, via 4.4a), resources plus signals (Section 5, via 5.1), technical direction (Section 7, via 7.1) and the build plan (Section 9, via 9.1). **The design is complete** apart from metrics and starting parameters, which get tuned in Phases 1a–1b.
>
> **Source:** design discussion between Damian and Claude chat, handed to Claude Code over claude-bridge on 2026-11-05 (messages #13 and #14; Section 4.5 came from the #14 addendum).

Items are marked **[decided]**, **[proposed]** (agreed in spirit, details open), or **[open]**.

## 1. Goal and philosophy

- **Goal:** recreate life from first principles — but in a *minimal digital universe*, not our physics. The question: "what is the smallest set of rules from which self-replication, evolution, ecology, and eventually major transitions (symbiosis, multicellularity) emerge on their own?" **[decided]**
- **Digital-native life:** organisms are programs, so resources should be native to computers (memory, storage, compute, bandwidth, signal channels), not imitations of chemistry. Follows Tom Ray's Tierra philosophy: instantiate life in a new medium rather than simulate organic life. **[decided]**
- **Rules, not hand-tuning:** behaviors and structures should emerge from general rules. **[decided]**
- **Nothing grants permanence:** every program eventually dies. Every protection (repair, archival storage, efficient resource use) either costs resources or has a failure rate, so replication is the only way a lineage lasts. This is what makes replication the primary drive without coding it. **[decided]** (see 6.1)
- **The world never acts on a program's behalf:** instructions do only what the program's own registers ask, so zero means nothing. Adopted for DIVIDE (6.1); extended to every verb (ALLOC, RELEASE, INJECT, and so on) in 6.4. **[decided]**
- **Realism explicitly rejected:** simulating real physics or a multiscale physics stack is computationally infeasible. **[decided]**
- **Success framing:** exploration. Expected: rich ecology (specialists, migrants, scavengers, parasites, regional species). Long shot but legitimate: symbiosis / multicellular-like transitions. Truly open-ended evolution is an unsolved research problem.

## 2. Lessons from prior work

- **Tierra (Ray, 1991):** parasites, host immunity, hyper-parasites (hosts hijacking parasites' CPU), social cooperators, cheaters all emerged. Plateaued because only replication speed was rewarded (complexity = pure cost), and the environment was uniform/static with two resources (CPU time, memory).
- **Avida:** rewarded logic tasks with extra CPU, so complexity paid; complex functions evolved from simpler ones. Multi-resource runs gave coexisting ecotypes. Ofria's group evolved division of labor in digital multicellular organisms; Moreno's DISHTINY studies digital multicellularity.
- **Lenski E. coli LTEE:** hidden second resource (citrate) → new lineage, coexisting ecotypes.
- **Ratcliff snowflake yeast:** selecting for size (not growth speed) produced multicellularity quickly.
- **Takeaways:** (1) environmental variety and trade-offs matter more than instruction-set power; (2) organisms are the most important part of each other's environment; (3) organisms must be able to reshape the world (waste, byproducts).
- Other refs: Particle Life, Lenia / neural CA, Fontana's AlChemy, von Neumann's self-reproducing automaton, 2024 Google "Computational Life" paper, island models, acoustic niche hypothesis.

## 3. Safety / architecture constraint [decided]

- Everything is **virtual**: organisms are bytes interpreted by our own VM. They never execute on the real CPU or touch the real file system, network, RAM allocation, GPU, or audio output.
- "Memory", "disk", "GPU", "tensor unit", "audio" are all simulated resources. Keeps runs reproducible, scarcity tunable, host safe.
- Worlds generated from a **seed** for exact reproducibility.

## 4. World structure: network geography [decided]

- World is a **graph of nodes**; distance = network hops. Provides locality for signals, scavenging, parasitism, migration.
- **Scale:** target 1000+ nodes; early experiments ~100.
- Not fully connected: paths, chokepoints, valleys, isolated pockets.

### 4.1 Procedural generation [decided, with the 4.4a G additions]
1. Scatter nodes in a hidden 2D/3D coordinate space (generation + visualization only; organisms never see coordinates).
2. One Perlin/simplex noise field per resource type → coherent **biomes** with gradients.
3. Connect by proximity (k-nearest or similar).
4. "Ruggedness" noise field removes/weakens edges → plains, valleys/chokepoints, isolated pockets.
5. A few sparse long-range links (rivers/trade routes); too many homogenize the world.
- Use correlated randomness, not uniform random; biome borders expected to be interesting.
- Hubs = high-traffic crossroads; remote nodes = refuges.

### 4.2 Node properties [decided — see 4.4a B]
Resource mix + regeneration rates; max population; memory volatility rate; write endurance. *Superseded by 4.4a B: capacity per resource type, cooling rate, volatility, write endurance, spark rate, background mutation.*

### 4.3 Edge properties [decided — see 4.4a E, F]
Bandwidth, latency, capacity, transit corruption rate (migration risk + mutation source). *Transit details: 4.4a F; dispersal resource exchange: 4.4a E.*

### 4.4 Dynamic landscape: node lifecycle [decided via 4.4a]
- **Write-endurance collapse:** finite writes before collapse (SSD write endurance analog). Heavy use destroys habitat.
- **Resource-exhaustion collapse:** node also collapses if free/open resources are depleted, to disincentivize hogging (closer to real overgrazing).
  - Caution: a hard "collapse at zero" may kill every node as soon as populations reach carrying capacity. Proposed: collapse only if free resources stay below a threshold for N consecutive ticks (hysteresis).
  - *(Superseded by 4.4a A: there is no regrowth under ownership. Exhaustion collapse is pool-starvation collapse with this hysteresis.)* Pair with **stock-dependent regrowth**: regeneration proportional to remaining reserves (logistic regrowth). Moderate use is sustainable; stripping a node bare stops regrowth → collapse.
- **Recovery / fallow period:** collapsed nodes go dormant (uninhabitable, maybe unreachable), then return regenerated. Returning nodes are empty → expect ecological succession (pioneer colonizers, then specialists).
  - *Resolved by 4.4a D:* fallow lasts as long as the drifting noise keeps the spot "sea"; the profile comes from the current noise; edges are recomputed; write endurance is fresh; the node restarts as a primordial soup.
- *Resolved by 4.4a C:* fate of organisms on a collapsing node. Decline is gradual and sensible (rising error rates and volatility, plus a node-health SENSE reading). Everyone on the node dies at collapse and its code is lost. Only daughters already dispersed survive.
- **Creation [decided — 4.4a D: creation and recovery are one mechanism]:** preferred proposal = drifting noise (time as extra noise dimension; spawn nodes at frontier positions where the field indicates land) + capacity budget (spawn ≈ collapse rate). Alternatives: budding/splitting; spawning in low-traffic regions.
- Also: slow resource drift and occasional disasters (memory wipes, link failures) to avoid permanent equilibrium.

### 4.4a Reconciling Section 4 with the Section 6 decisions [decided — E adopted first, then A–D, F, G accepted by Damian, 2026-11-05]

Section 4 was written before the resource-ownership model. Three conflicts and four updates:

- **A. Conflict: nothing "regrows" under ownership [decided].** 4.4 assumes consumable resources that deplete and regrow, like grazing. Under 6.1, resources are durable hardware units that are *held*, not eaten, so "stock-dependent regrowth" has nothing to act on.
  - *Proposal:* drop regrowth.
  - Resource-exhaustion collapse becomes *pool-starvation collapse*: if a node's free pool stays below a threshold for N consecutive ticks (the hysteresis already proposed), the node collapses. It still targets hogging: a program can hold units while idling cool, and starvation collapse is the tragedy-of-the-commons price of that.
  - The three pressure timescales become: fast = heat; medium = competition for the free pool, the ambient heat leak and the starvation counter; slow = wear, collapse and recovery.
- **B. Update: node properties (4.2) [decided].** Replace "resource mix + regeneration rates; max population" with:
  - hardware capacity per resource type (millicores, GPU units, MB of memory, disk);
  - *cooling rate* (a node's "climate": cold nodes let programs run hotter);
  - memory volatility;
  - write endurance;
  - spark rate ("warm pools");
  - background mutation rate ("radiation").

  Max population drops out, because it emerges from memory. Each property can follow its own biome noise field (4.1).
- **C. Conflict-free update: node death is gradual [decided].** Worn hardware fails gradually in reality: worn flash has more bit errors. So as write endurance runs down, the node's error rates and volatility rise before the final collapse. That's the 4.5 no-cliffs principle applied to habitat. Node health is a SENSE reading, so "leave a dying node" can evolve.
  - *Fate:* every program on a collapsed node dies (no running CPU, 6.4 #1). Its code is lost, not scattered.
  - Survivors are only the daughters already dispersed: a mass extinction with refugees.
- **D. Update: recovery and creation become one mechanism [decided].** A node appears wherever the drifting noise field says "land", subject to the capacity budget (spawn ≈ collapse). A recovered node is simply a new node at old coordinates. It takes its resource profile from the noise *as it is now*, gets fresh write endurance, has its edges recomputed by proximity, and starts as a fresh primordial soup (random waste, full pool, sparks firing) per 6.3. Ecological succession then plays out as colonists from neighbours versus local abiogenesis. Fallow duration is however long the drifting noise keeps that spot "sea".
- **E. Conflict: dispersal versus "resources never move between nodes" [decided — exchange rule adopted by Damian].** DIVIDE with BX = neighbour sends a daughter away, but her CX/64 share is CPU on the *origin* node, and cores can't travel. On arrival she'd have no CPU and die at once (6.4 #1).
  - *Proposed exchange rule:* the parent's CX/64 share is released into the origin node's pool (the parent pays at home). On arrival, the daughter draws the same amount from the destination's free pool, capped by what's there.
  - Her body needs unowned memory at the destination (she overwrites waste there); if there is none, she dies.
  - This is the world executing what the code asked, not acting on its behalf. Empty or recovering nodes welcome colonists; full nodes reject them.
- **F. Update: edges in transit (4.3) [decided].** A dispersing daughter is cargo with no CPU.
  - *Bandwidth* limits how many instructions cross per tick, so big daughters take longer.
  - *Latency* delays arrival.
  - *Capacity* caps simultaneous transfers.
  - *Transit corruption* flips bits per instruction crossing.
  - Link failures are among the occasional disasters, and a daughter in flight on a failed link is lost.
- **G. Update: generation (4.1) [decided].** Unchanged, plus extra noise fields for cooling rate, spark rate, volatility and background mutation, so climates vary independently of resource richness.
- *Built in Phase 3 (6.9):* B (capacity per resource, cooling, spark rate and background mutation, each on its own field; volatility and write endurance wait for Phase 4), E (the exchange rule), F (bandwidth, latency and capacity; corruption and link failures wait for Phase 4) and G (the generator).
- *Built in Phase 4 (6.10):* the rest of B (volatility and write endurance, on their own fields), A and C (wear, gradual decline, a node-health reading, pool-starvation collapse), D (creation and recovery as one mechanism, over a drifting land field and a budget), and F's corruption and link failures. *Revised by Damian during the build:* at a collapse the code is not lost; it stays behind as remnants, and a returning node finds its ruins weathered by the fallow time.

### 4.5 Resource heat / throttling [decided — the original lean plus 6.1, 6.4 #2/#17/#18 and 4.4a A]
- In addition to (not instead of) node collapse. Modeled on thermal throttling: each resource (CPU, GPU, memory, etc.) on a node has a "heat" value that rises with use and decays over time; past a threshold, the resource throttles.
- **Leaning toward node-level (shared) heat:** one organism's heavy use throttles the resource for everyone on the node → competition over timing, possible temporal niche partitioning (organisms alternating CPU-heavy and memory-heavy phases; species coexisting by using resources at different times). Per-organism fatigue could be added as well. (Was open: which, or both.) *Resolved (6.4 #17): per-owned-unit heat plus a weak node-level ambient leak.*
- **Gradual throttling, not hard cutoffs:** progressive slowdown keeps the fitness landscape smooth, so small improvements in resource usage get small rewards. Hard cutoffs create cliffs evolution handles poorly.
- Gives the design three pressure timescales: fast (heat/cooldown, ticks), medium (stock-dependent regrowth), slow (exhaustion collapse, write endurance, fallow recovery). *Updated by 4.4a A:* fast = heat; medium = competition for the free pool, the ambient leak and the starvation counter; slow = wear, collapse and recovery.
- Build into the engine early, but enable it as its own experiment per the phased plan.
- *Update (6.1):* Damian's resource-ownership model ties heat to the resource units a program owns. Heat throttles a unit gradually as it rises, and only past the top threshold is the unit freed for others to claim **[decided]**. Programs can sense the reading before they lose the unit.
- **Heat raises error rates too [decided — 6.4 #18]:** past certain heat thresholds, hardware throttles and/or flips more bits (bit rot). So heat has advantages (faster mutation) as well as disadvantages (slower resources, damage).
  Claude's suggestions (the per-resource mapping and staggered curves were decided in 6.4; the rest are consequences):
  - *Per-resource mapping onto Tierra's three mutation sources:* hot memory flips more bits in the code it stores; a hot CPU makes more execution flaws (off-by-one results); copying while hot makes more copy errors.
  - *Smooth curves with staggered onsets, not hard bands:* warm means errors start rising at full speed (an overclocking zone); hot means throttling as well; at the top, the unit is released. That gives a real "fast but sloppy" choice before the slowdown.
  - *An evolvable mutation rate:* by running hot or cool, a program effectively sets its own lineage's mutation rate. Bacteria do this under stress (the SOS response). This lets "evolvability" itself evolve.
  - *The error threshold (Eigen):* the longer a genome, the less copying error it can tolerate before its information melts away. So heat-driven mutation means large, complex programs must run cooler. The requirement that complexity be efficient then emerges without a rule.
  - *Usage-dependent aging:* heavy use heats memory, which rots code, so living fast means ageing fast. This adds to the "nothing grants permanence" principle. (A weak node-level ambient component sits on top: 6.4 #17.)

## 5. Resources [decided via 5.1] (all virtual, all with trade-offs)

- **Memory tiers:** small fast cache, larger RAM. **Volatile:** can randomly "flicker off" and reset. Expected to drive checkpointing (state in memory, genome backed up to disk). *v1 (5.1 A, C): RAM only; volatility drives repair from disk, not resurrection.*
- **Disk tiers:** fast storage with slow bit rot vs slow, near-permanent, expensive archival storage. Long-term survival requires maintenance (DNA repair analog). *v1 (5.1 B): one disk tier; archival deferred.*
- **Compute:** CPU cycles; virtual GPU unit (one op across a data block cheaply, divergence penalty); virtual tensor unit/NPU (block matrix multiply). Open whether anything evolves to use them. *v1 (5.1 D): CPU in millicores; GPU block ops in a later phase; tensor/NPU deferred.*
- **Bandwidth:** limited bus between tiers and neighbors. *v1 (5.1 E): edges plus amount-proportional DISK/GPU costs; no bus model.*
- **Waste:** leftover data and dead organisms' code persist; scavenging code fragments = digital horizontal gene transfer; doubles as fossil record.
- **Signal / "audio" channel [decided — design in 5.1 G]:** shared virtual medium; organisms emit and analyze signals; channel/frequency space is a resource. Needs locality: attenuation per network hop (preferred), Ethernet-style collisions, limited channels per neighborhood with noise floor. Researcher may tap it to listen (never routed to real speakers by organisms).

### 5.1 Reconciling Section 5 with the Section 4 and 6 decisions [decided — G adopted first, then A–F accepted by Damian, 2026-11-05]

Scope rule: Section 9 names scope creep as the #1 risk, so v1 keeps the fewest tiers that still give every trade-off a home. Everything deferred stays in the design as a later experiment.

- **A. Volatility means repair, not resurrection [decided].**
  - *The catch:* the original text expects volatility to drive checkpointing (genome backed up to disk). But a body whose memory is wiped can't run its own restore code, because that code is gone too. The only fix would be a world-run "reboot from disk", which comes close to granting permanence: perfect restore means near-immortality.
  - *Proposal:* volatility events wipe random address ranges of random (bank-like) size. Wiped contents become random codons, not zeros (codon 0 is NOP0).
    - A *partial* hit leaves intact code that can repair the damage from the body's disk copy (DISK compare, then load). This is the DNA-repair analog.
    - A *whole-body* hit is death.
    - There is no boot-from-disk rule. The real backup is offspring elsewhere, which is how life does it. The rate is a node property (4.4a B).
- **B. Disk: one tier in v1 [decided].**
  - Owned via ALLOC (disk type); subject to slow bit rot.
  - DISK selectors: 0 = compare (mismatch count into CX), 1 = store, 2 = load. Operations cost CPU time proportional to the amount moved, like attacks (6.4 #7).
  - When a program dies, its disk holdings return to the pool and their contents become waste, so scavengers can dig up "fossils" until the space is reused.
  - Deferred: the archival tier (near-permanent but expensive). Expense needs a form without a currency (scarce capacity, slow access, high write heat), and "near-permanent" sits uneasily with the permanence principle.
- **C. Memory: RAM only in v1 [decided].** The small fast cache tier is deferred. Its benefit (cheaper instructions) overlaps CPU ownership and adds a second speed model.
- **D. Compute [decided].**
  - *CPU* in millicores (6.4 #16).
  - *GPU* (enabled right after proof of concept, per 7.1 G): one selector op (copy, add, xor, compare) applied across CX elements from the read head to the write head. Each element is cheap; setup costs extra and the GPU runs hot. It has to be claimed via ALLOC like any resource. A GPU block copy is effectively a "faster polymerase", a major innovation evolution can discover once GPUs are enabled. It is disabled in the abiogenesis test, so it can't shortcut the origin of life. *Built in Phase 2: 6.7.*
  - *Divergence penalty and tensor/NPU unit:* deferred. *The NPU was un-deferred by Damian on 2026-11-05 and built in Phase 2b (6.8), along with a 16-op GPU; the divergence penalty stays deferred.*
- **E. Bandwidth [decided].** Between nodes, edges (4.4a F). Within a node, DISK and GPU costs scale with the amount moved. No separate bus model in v1.
- **F. Waste [decided — 6.4 #9, plus disk fossils]:** unowned, readable, copyable and claimable. Sparks animate it. It is also the fossil record, now including dead programs' disk contents.
- **G. Signal channel design [decided — adopted by Damian].**
  - *Superposition instead of collision rules.* Emissions on the same channel add up, like sound. Listeners hear the sum, so crowding becomes interference without any Ethernet-style rule. Programs gain from using quiet channels: the acoustic niche hypothesis in Section 2.
  - *Per-hop attenuation.* A signal is multiplied by a fixed factor per hop, delayed by edge latency, and dropped below a per-node *noise floor* (random background on every channel; it could follow biome noise).
  - *Signals last one tick,* so sustained messages must be re-emitted. Silence is free; speech is not.
  - *Loudness costs heat.* EMIT's amplitude is CX, and heat rises with amplitude. That makes signals costly and therefore tends to keep them honest, as in honest-signalling theory.
  - *Public.* Anyone can LISTEN, so eavesdropping predators and deceptive signals are possible. Channels are a commons, never owned.
  - *Size:* a fixed number of channels per world (e.g. 16) selected by BX; values are register numbers. Off in early phases (EMIT and LISTEN set the error flag). The researcher can tap any channel; it is never routed to real speakers.
- *Built in Phase 4 (6.10):* A (volatility wipes; repair from disk; no boot-from-disk), B (one disk tier: DISK compare, store and load, fossils, slow rot) and G (signals as above, with a hard reach of 8 hops), plus a rule Damian added during the build: a much smaller program gets only pieces of a bigger program's code, from remnants, fossils or engulfed prey (salvage).

## 6. Organisms — core design decided 2026-11-05 (see 6.1–6.4)

- **Instruction set:** must be evolvable (robust to mutation). Tierra: ~32 instructions, no numeric operands, template-based addressing.
- **Critical concern:** every world feature needs a matching, *mutation-reachable* way for organisms to use it, or it becomes inert scenery.
- Genome representation; volatile memory vs disk state split.
- Energy/cost model: instruction and resource costs; how organisms earn CPU time.
- Mutation sources: copy errors, bit flips, transit corruption, scavenged fragments.
- Reproduction and death: reaper rules, aging, failure conditions.
- **Seed ancestor:** hand-written, must survive and replicate; shapes everything; may take iteration.
- Inter-organism interaction: read/execute each other's code (enables parasitism)? Attach to each other (needed for symbiosis/multicellularity)?

### 6.1 Discussion log (started 2026-11-05)
- **Each node is its own soup [decided]:** Tierra's shared-memory soup maps onto a node. Programs do not start out able to travel; moving between nodes has to evolve.
- **No separate Tierra-style time slicer [decided]:** CPU sharing is already covered by the node compute resource and heat throttling (4.5).
- **Death [decided — 6.4 #1 defines death as "no running CPU"; protective death emerges via engulf and markers; no reaper or backstop cull (#20)]:** Tierra's reaper (an error-ranked queue that culls when memory is ~80% full) adds selective pressure, but it's unclear whether it suits Nova. Damian's preferred direction: program death as a *protective* mechanism, like apoptosis in real cells, that stops programs from changing function too much once they have become critical infrastructure for larger programs. *Clarified:* this is not a mechanic we introduce. It should develop naturally in higher-order programs, to stop their parts evolving out from under them. Damian is unsure that multicellular organisms are the right frame, and is weighing whether the consuming mechanism alone is enough.
- **Consuming live programs [decided — mechanism in the engulf rules below and 6.4]:** Damian wants active programs to be able to consume each other, not only scavenge dead code. Damian likes the three outcomes below (overwrite, copy, keep running) and the economics that choose between them.
- **Code is private [decided]:** no program can read or execute another program's code. Programs own their space, their design (code) and their resources. Code moves between programs only through the engulf outcomes (overwrite, copy, keep), through INJECT (below), or by scavenging discarded waste in the soup for code and resources.
- **Invasion instructions [decided]:** protected spaces need instructions that can invade them. There are two:
  - **ENGULF** brings a whole body in.
  - **INJECT** writes a piece of the attacker's own code into another body, where the victim's CPU may run it (the virus route). The injected code gets no CPU of its own; it runs only if the victim executes it.
  
  *Resolved in 6.4:* injected code overwrites (#3); attacks cost CPU time proportional to what they move, plus heat (#7).
- **Resources are owned holdings, governed by heat [decided]:** each node has a fixed set of resources: CPU cores (or a percentage of CPU), GPU cores, MB of memory, and so on. A program can use what it owns, so owning more lets it use more. Usage is governed by heat (4.5). Too much heat frees the resource up, making it free game for other programs to claim. Efficient use therefore pays, and growing larger and more complex means finding ways to use extra resources without overheating them. Per-node limits mitigate exponential growth, and more nodes are the main outlet; tune after enough runs. Damian flags programs that learn to communicate across nodes to maintain their resources as a potentially very interesting development. This replaces the turn-based CPU split suggested below.
- **Drives [decided]:** the primary drive is replication: keeping genes duplicating, as in life. Everything else is secondary to it. We eat to survive and procreate to pass genes on, so the primary drive can't simply be "consume resources". Resources matter only as the means to survive and replicate.
- **Replication is not preprogrammed [decided]:** just as drives aren't preprogrammed, duplication shouldn't be either. Programs should arrive at it naturally, with copying their code and dividing their resources showing a clear benefit through selection. This reopens the hand-written seed ancestor (Section 6 list) and the Tierra-clone Phase 1 baseline (Section 9).
- **Births are not a preconceived rule [decided]:** how programs divide their resources and duplicate their code must develop naturally from the existing instruction set. Programs work out whether, when and how to do it, if selection favours it. The world supplies only the mechanics; it never supplies a birth strategy.

Claude's suggestions (status noted per item):
- **Migration as dispersal at birth (now covered: it is DIVIDE's BX destination, adopted with the DIVIDE mechanics):** a DIVIDE variant places the daughter on a neighbouring node, one mutation away from the ancestor's own DIVIDE. Edge transit corruption applies to the daughter's copy as it crosses.
- **Split the reaper's two jobs (room-making decided in 6.4 #20: no backstop cull).** *Making room* when a node is full comes from predation and environmental hazards (volatility, collapse, bit rot), with a neutral random cull only as a backstop for the early phases. *Protective death* is something programs can evolve, not a world rule: a world rule that kills programs for changing would also stop evolution. *Revised after Damian's clarification:* with engulf in place, protective death needs no instruction of its own. A drifted part can display an "eat me" marker (as dying cells do in biology), or the larger program can simply engulf a part that has drifted. The disk backup in Section 5 still gives programs a reference for spotting drift, and allows repair (restore instead of being eaten).
- **Consumption as ownership transfer [decided — 6.4 #6–8]:** one "engulf" instruction finds its target by template. Success is graded by how closely the predator's template matches a marker on the prey. On success, ownership of the prey's memory block passes to the predator. What happens next is up to the predator's code: overwrite it (digestion and free space for daughters), copy from it (gene theft), or leave it running (a path to endosymbiosis). Open: does an engulfed program's CPU keep running, and with whose write privileges; what does an attack cost; what defences besides changing markers. If engulfed programs keep running, engulf also gives *composition*: a program that contains running programs is a higher-order program, so multicellular-like forms need no separate attach mechanism. That makes "does an engulfed program keep running" the key decision. The question after it is how a host's running parts come along when the host reproduces.
- **What decides between overwrite, copy and keep [decided — 6.4 #8; K = 8 (#1); CPU split replaced by the ownership model]:** no rule picks the outcome. Three rules set up the economics, and selection picks per lineage.
  1. An engulfed program keeps running, as an extra CPU in the eater's body. So doing nothing means keeping it.
  2. One body, one set of rights: every CPU in a body can write anywhere in that body.
  3. A body pays for all its CPUs out of its single compute share. A passenger is a cost unless it does useful work.

  Bookkeeping rule: a CPU that errors K times in a row halts. This is how a host "stops the engine" before digesting, for example by writing a jump to a template that doesn't exist over the prey's code.

  Consequences: overwriting gains free space for daughters. Copying gains code; with code private, engulf is the only way to get at a live program's code. Keeping only pays if the passenger earns its share. Rule 2 cuts both ways: a running prey can also overwrite its eater from inside. That makes engulfing a gamble, and gives composite bodies a reason to police their parts.
- **What stays visible once code is private (decided — 6.4 #9; the seed-ancestor note applies only to the seeded fallback):** each body's leading template (the run of NOPs at its start) is its public *surface*, and everything else is private. Engulf and any other contest match against surfaces. Otherwise, template searches only look inside the searcher's own body. Waste is unowned: anyone can read it, copy from it, or claim its space with MAL. Analogy: the ownership rule is the cell membrane, and the surface template is like the surface proteins on it (what others see and bind to). Internal templates are binding sites for the program's own jumps. Seed-ancestor note: give the ancestor a surface template that none of its own code searches for, separate from its start marker. Otherwise a mutation to the surface breaks the program's ability to find its own start, which freezes its markers and stalls the arms race.
- **One contest for both invasion instructions (decided — 6.4 #6; the opcode-placement idea is obsolete):** ENGULF and INJECT use the same graded surface-match contest and costs. Put ENGULF one bit flip from MAL and INJECT one bit flip from the copy instruction, so each is a single mutation from code the ancestor already has.
- **Defence by self-check (resolved — DISK compare in table v1, 6.4 #15):** the drift check against the disk backup doubles as immunity. A body that finds code it doesn't recognise can overwrite it, much as CRISPR cuts viral DNA. The same check serves repair and apoptosis-style triggers.
- **Tierra baseline as a config mode (decided — 6.4 #26; narrowed to the light Tierra mode in 9.1):** Phase 1's Tierra clone keeps Ray's open membrane only to validate the engine against his numbers; Nova runs use private code. Tierra-style parasites can't arise under private code. They move inside instead: a prey that "wants" to be engulfed, or an INJECT lineage.
- **CPU split rule (superseded by the resource-ownership model above; kept for the record):** the split decides how many turns each body gets. Heat (4.5) decides how far each turn goes for each resource. The unit is the body (engulf rule 3), and a body's CPUs split its share equally. A body's share is proportional to its owned memory raised to a power p, a config knob (Ray's slice exponent).
  - Suggested start: p = 1. This is size-neutral, so it doesn't build in Tierra's push toward tiny programs. p = 0 (equal per body) reproduces that push, as an experiment.
  - The catch with p = 1: a body can pad its block with junk to earn CPU without having more to copy. The world already pushes back. Per-address hazards (volatility, bit rot) hit big bodies more often, and memory hoarding triggers exhaustion collapse (4.4).
  - Side effects under either rule: empty or lightly used nodes give more CPU per body, so dispersal at birth pays on its own. With p = 1, a kept passenger brings its own memory's share, so keeping is cheap. With p = 0, a passenger halves the host's time, so keeping has to be earned.
- **Making the ownership model run (decided — 6.4 #2, #16–19):**
  - *Scheduler:* each tick, a body executes instructions in proportion to the CPU it owns, in fine-grained units (e.g. millicores) so a node can hold hundreds of programs. Memory and CPU are separate holdings, so padding memory earns no CPU and no size bias is built in.
  - *Gradual first, then release* **[decided — Damian agrees a pure release is a cliff]**: heat throttles a unit progressively as it climbs, and only past the top threshold is the unit freed. This is how real CPUs behave (throttle, then shut down at their maximum temperature). It keeps a slope for evolution to climb instead of a cliff (the 4.5 principle), and gives programs a reading to sense before they lose the unit.
  - *Births:* DIVIDE hands the daughter part of the parent's holdings, with the amount taken from a register. Parental investment then evolves: many lean offspring versus a few well-provisioned ones. (Refined in "DIVIDE mechanics only" below.)
  - *Claiming:* MAL is generalised into one allocation instruction for every resource type, with the type in a register, so claiming CPU or GPU is one mutation away from claiming memory.
  - *Passengers:* a kept passenger runs on the body's cores and heats them, so a freeloader can cost its host those cores. Heat restores engulf rule 3's pressure toward useful partners.
  - *Resolved:* freed units keep their heat (6.4 #2); a weak node-level ambient leak (#17). Resources never move between nodes, so cross-node "maintenance" is coordination (kin signalling over the Section 5 channel, sending daughters to cooler nodes), not transfer.
- **Where the replication drive comes from (accepted):** the drive isn't coded anywhere, in life or in Tierra. It is what's left: whatever gets copied more becomes more common. Nova gets it from two facts.
  1. A replicator exists: seeded, or (per the decision above) emerged from the starting soup.
  2. Every program eventually dies.

  The second fact is the one at risk, since we dropped the reaper. If anything could last forever (for example, perfect repair from a disk backup), a non-replicating hoarder could sit on resources indefinitely, and the drive would weaken.
  - *Principle: nothing in the world grants permanence* **[decided — adopted by Damian, also listed in Section 1]**. Every protection (repair, archival disk, efficient use) either costs resources or has a failure rate. Aging is then accumulated damage. Repair versus reproduction becomes a trade-off that evolution settles, investing in repair only as far as it helps produce copies (the disposable soma theory). That's how "everything else is secondary" emerges without a rule.
  - *Never add an external score* **[decided — 6.4 #5]**. Any reward other than "more copies" installs a second drive. Avida's bonus CPU for logic tasks is an example; picking "best" worlds by a metric (Section 7) is the same mistake at a different level. The world's only judge is who is still around.
- **Letting replication emerge (accepted):**
  - *Rules versus starting soup* **[decided — Damian: random soup is the main mode]**. No world rule says "replicate". MAL, DIVIDE and the copy instruction are just instructions. Whether replication is preprogrammed depends only on what the starting soup contains. Proposed: Nova's main mode starts from random code with no replicator in it (abiogenesis). A seeded hand-written replicator stays available as a fallback and for engine validation against Ray's numbers.
  - *Selection can't act before heredity.* Until something copies itself, even crudely, there is nothing for selection to favour. The first replicator has to come from chance plus the world's own dynamics. Before that, the only filter is persistence: things that last longer are more common. Once a replicator appears it takes over fast. From then on, copying code and dividing resources pay through selection, because nothing is permanent.
  - *Precedent.* Google's 2024 "Computational Life" (arXiv 2406.19108) had random 64-instruction BFF programs paired up, run together and allowed to modify each other. With no fitness function, self-replicators arose, with or without background mutation. Pargellis's Amoeba (Tierra-inspired) saw opcode bias, then short building blocks spreading, then those blocks combining into replicators. Counterexample: in SUBLEQ, replicators are possible but were never seen to arise. So the instruction set has to make replicators *discoverable*, not merely possible.
  - *What discoverability needs.* Replicators must be possible in only a few instructions. Partial copying must give partial heredity, so replication can be assembled in steps rather than appearing all at once. And code must be able to act on code. BFF got that by letting programs write into each other; under Nova's private code it comes from INJECT, ENGULF, writes within a program's own body, and copying from waste. So the first replicators may look viral.
  - *Test early.* Add an abiogenesis experiment on one node before the Phase 1 ecology work: can replicators emerge under Nova's rules? If not, change the instruction set or rules, or start from the seeded fallback.
- **DIVIDE mechanics only [decided — adopted by Damian]:** the world defines what DIVIDE physically does. Every choice about using it belongs to the program.
  - *What moves:* the daughter is the block the parent most recently allocated, and that memory becomes her body. She also receives CX/64 of each of the parent's other holdings (CPU, GPU, and so on). BX is the destination: 0 = here, k = neighbour edge k.
  - *Zero means nothing.* Every encoding has some value when registers are zero. The neutral choice is "the world never acts on a program's behalf", so CX = 0 gives the daughter nothing. She is born with no CPU and is effectively waste. The real cost: simple replicators need an extra instruction to set the share (see the 6.2 sketch). Giving her everything, or half, at zero would be a built-in birth strategy.
  - *No copy required.* DIVIDE splits off whatever is in the block. A copy of the parent is one use; budding off something different (a specialised part, a decoy, a payload) is another. Selection decides which uses last.
  - *Strategies then available, none built in:* many lean offspring or a few well-provisioned ones; giving almost everything and fading (semelparity, like salmon); staying home or dispersing to a neighbour; copying faithfully or not at all.
  - *Failure:* DIVIDE with no allocated block sets the error flag.

### 6.2 Instruction set (codon table v1 accepted as the working draft on 2026-11-05; the open items at the end are still open)

**Constraints carried in from 6.1:** code is private; ENGULF and INJECT are the invasion routes; resources are owned and heat-governed; replication must be discoverable from a random soup but must not be preprogrammed; every world feature needs a mutation-reachable way to sense it and use it.

*Phase 8 (6.14):* the same instructions also run on warps, a body's code in lockstep across many lanes on the GPU, with lane-offset heads; the GPU verb's BX 20 launches a warp, and SENSE gains "my lane" and "lanes in my warp" (a CPU reads lane 0 of 1).

**Key choices:**
- *The replicator-length dial.* The length of the shortest possible replicator sits between two failure modes. At 1 instruction (a REPLICATE opcode) replication is preprogrammed. At 22+ instructions (Tierra's best) it is effectively never found: 32^22 ≈ 10^33. The target is roughly 8–12 instructions (32^8 ≈ 10^12, within reach of a busy soup's churn), built from primitives that only replicate when composed into a loop.
- *Alphabet size.* Every bit pattern is a valid instruction, as in Tierra.
  - Claude's first draft used 32 opcodes, because a smaller alphabet makes each primitive appear more often in random code. The 64-opcode penalty is 2^n for an n-instruction target: 256× at 8, ~1,000× at 10, ~4,000× at 12.
  - **Damian leans toward 64 (resolved: see "Outcome" below)** for a richer set of programs, accepting a slower origin because modern hardware has far more headroom than Ray had.
  - **Claude's proposal: a 64-slot codon table with redundancy (adopted — see Outcome below).** Give each core primitive (labels, COPY, ALLOC, DIVIDE, the flow and counting basics) two codons. Each core primitive then turns up in random code exactly as often as in a 32-set (2/64 = 1/32), so origin odds are unchanged for replicators built from core pieces. The remaining slots carry the richer instructions at 1/64 each, giving roughly 48 distinct instructions in total.
  - Arrange each pair of synonyms one bit apart, so that bit flip is a neutral mutation, like the third-position "wobble" in the genetic code. The table is config, so the redundancy can be tuned against the abiogenesis test.
  - The cost is evolutionary waiting time, not run speed: 5- and 6-bit opcodes both fit a byte, and dispatch cost is the same.
  - **Outcome [decided]:** Damian likes both, so both are kept. The table has 64 slots, and redundancy is a config setting. "Doubled core" is the suggested default for the abiogenesis test; "plain 64" (every instruction gets one slot) is an experiment. They are the same engine with a different codon table.
- *Richness of the extra slots [decided — realised by codon table v1, 6.4 #15]:* both kinds. The register selectors already give variety in what programs can *touch* (which resource, which reading, which destination). The extra slots mainly add variety in how programs *compute*: logic ops (AND, OR, XOR), multiply and compare, richer loops. If a world interaction turns out to deserve its own opcode rather than a selector, it can take a slot too. The exact split is open.
- *Verbs plus selectors.* One opcode per verb, with the specific variant chosen by a register value: ALLOC (resource type), SENSE (which reading), DIVIDE (destination: here, or a neighbour edge), DISK (store, load or compare). Benefits:
  - few opcodes, but a large space of behaviours;
  - switching variant is an INC/DEC away;
  - migration is a DIVIDE selector rather than a new instruction;
  - world features switched off by config simply make that selector set the error flag, so the opcode table never changes between experiments and genomes stay meaningful across configs.
- *Heads for copying (from Avida/BFF).* Movable read and write pointers keep the copy loop short. COPY moves one instruction from the read head to the write head and advances both. Private code applies: the read head may point into the program's own body (including engulfed parts) or into waste; the write head may point only into its own body or its daughter block.
- *Tierra templates kept* for jump targets (searching only the program's own body) and for aiming ENGULF and INJECT at surfaces.

**CPU:** registers AX, BX, CX; an 8-word stack; the instruction pointer; read and write heads; an error flag.

**Register roles:** CX is the counter or amount (INC, DEC, ZERO, IFZ, PUSH and POP act on it). BX is the selector for verbs. AX is the address or value. IFLT tests AX < BX. ROT rotates AX→BX→CX→AX, so one opcode covers every register move.

**Codon table v1 (64 slots, 6-bit codons) [decided — accepted by Damian as the working draft].** Accepting v1 also accepts the structure it embodies: verbs plus selectors, read and write heads, and Tierra templates. Default config: doubled core. 16 core instructions take two codons each (0–31, 1/32 each in random code); 32 richer instructions take one each (32–63, 1/64 each). That is 48 distinct instructions. Mutation geometry:
- **bit 0:** synonymous in the core; a near-synonym (its partner) in the rich half.
- **bit 1 in the core:** flips to a related instruction (NOP0↔NOP1, JMP↔IFZ, INC↔DEC, ZERO↔ROT, PUSH↔POP, COPY↔FIND, IFLT↔SENSE, ALLOC↔DIVIDE). NOP0↔NOP1 being a single flip keeps Tierra's property that a template bit mutates in one step.
- **bit 5:** moves between core and rich (a large change).

```
codon  meaning     | codon  meaning     | codon  meaning     | codon  meaning
-- core, doubled (two codons per instruction) --------------------------------
00-01  NOP0        | 02-03  NOP1        | 04-05  JMP         | 06-07  IFZ
08-09  INC         | 10-11  DEC         | 12-13  ZERO        | 14-15  ROT
16-17  PUSH        | 18-19  POP         | 20-21  COPY        | 22-23  FIND
24-25  IFLT        | 26-27  SENSE       | 28-29  ALLOC       | 30-31  DIVIDE
-- rich, single (partners differ in bit 0) ------------------------------------
32 CALL    33 RET      34 DJNZ    35 JZ       | flow
36 ADD     37 SUB      38 MUL     39 DIV      | arithmetic: CX = AX op BX (DIV by 0 -> error)
40 AND     41 OR       42 XOR     43 NOT      | logic
44 SHL     45 SHR      46 NEG     47 RAND     | CX bit ops; RAND = random value into CX
48 IFNE    49 IFERR    50 SWAP    51 ROTB     | AX != BX; error flag set; AX<->BX; reverse ROT
52 SETH    53 GETH     54 READ    55 WRITE    | head (BX picks) <- AX / AX <- head; CX <-> code at heads
56 ENGULF  57 INJECT   58 EMIT    59 LISTEN   | contact (aimed by template); signal channel BX
60 DISK    61 GPU      62 YIELD   63 RELEASE  | disk op BX; block op; idle rest of tick (cools); give up CX of type BX
```

**Semantics in brief:**
- *Jumps:* JMP, CALL and FIND search the program's own body outward for the complement of the template that follows them, within a limited distance. If nothing is found, the error flag is set and the instruction is skipped.
- *Heads:* the read head starts at the body's start. ALLOC (memory) puts the write head at the start of the new block. COPY copies the instruction at the read head to the write head and advances both, with a heat-dependent chance of a copy error. Private code bounds the heads: reads only within the program's own body, engulfed parts or waste; writes only within its own body or daughter block.
- *DJNZ / JZ:* decrement-and-jump-if-not-zero, and jump-if-zero, both to a template.
- *DIVIDE:* the daughter's destination comes from BX (0 = here, k = neighbour edge k). She receives CX/64 of each non-memory holding; 0 gives nothing (decided in 6.1, "DIVIDE mechanics only").
- *End of body:* an instruction pointer running past the end of its body wraps to the body's start, since private code means it can't run into a neighbour (decided — 6.4 #14).
- *INJECT:* writes CX instructions taken from the read head.
- *Disabled features* (DISK, GPU, EMIT/LISTEN in early phases) set the error flag.

**Plain-64 config:** the 16 duplicate core codons are reassigned. Candidates: IFEQ, IFGT, MOD, MIN, MAX, DUP, DROP, CLRERR, JMPF/JMPB (one-direction jumps), NOPX (an inert spacer that ends templates), and per-register INC/DEC.

**Sketch of a short replicator (not seeded, just showing that one can exist):**
```
SENSE        ; CX = my size
ALLOC        ; claim CX memory; write head -> new block
NOP1         ; loop label
COPY         ; copy one instruction, advance both heads
DEC          ; CX -= 1
IFZ          ; if CX == 0 ...
DIVIDE       ; ... split off the daughter
JMP NOP0     ; back to the NOP1 label
```
All eight are core codons (each 1/32 in random code).

**Catch (found 2026-11-05):** when this loop reaches DIVIDE, CX is 0. Under the proposed DIVIDE mechanics, the daughter would be born with no CPU and never run, so this sketch is not actually viable. A viable minimal version has to set the share first:
```
SENSE        ; CX = my size
ALLOC        ; claim CX memory; write head -> new block
NOP1         ; loop label
COPY         ; copy one instruction, advance both heads
DJNZ NOP0    ; CX -= 1; back to the label until CX == 0
INC          ; CX = 1 -> daughter gets 1/64 of my CPU
DIVIDE       ; split her off; run past my end, wrap to SENSE, go again
```
That's 8 instructions, one of them the single-codon DJNZ. Core-only versions get a few instructions longer, because IFZ can only guard one instruction. Either way, the extra INC is the price of the world not choosing a birth default. A 1/64 share makes for a very lean daughter; better provisioning is something selection has to discover.

**Formerly open, now resolved:**
- selector numbering: nearest-to-self first, plus a shuffled-selector control (6.3);
- opcode mix: uniform codons (6.4 #13);
- READ/WRITE: kept (#11);
- INJECT/ENGULF: single slots, with doubling as an experiment (#12);
- stack depth: 8 (#10);
- K: 8 consecutive errors, and a body with no running CPU dies (#1).

### 6.3 Starting soup (sparks and a sparks-only start decided; other items still Claude's draft)

The last assumptions the world makes before evolution takes over. Each one aims to be neutral under the decided principles: no built-in strategy, the world never acts on a program's behalf, and nothing grants permanence.

- **Everything is "chemistry" [decided, via the sparks-only start below].** Fill *all* of a node's memory with uniformly random codons, all of it unowned waste at t=0. Waste then gives scavengers and sparks (below) raw material immediately. Because the codon table doubles the core, core instructions appear 1/32 each and rich ones 1/64.
- **Bodies (superseded by the sparks-only start: body length becomes a spark setting).** About half the memory is carved into bodies of random length (uniform 8–128 instructions). Size is then a variable selection can act on, not a preset like BFF's fixed 64. Example: a 64K-instruction node holds ~480 starting bodies.
- **CPU state at birth.** Registers zero, IP and both heads at the body's start, error flag clear. No random register values, because those would hide arbitrary "amounts" in the start.
- **Resources (superseded by the sparks-only start: every resource begins in the free pool, and each spark's grant comes from it).** For each resource type, half is split equally among the starting bodies and half is left in the free pool. Equal shares avoid favouring size; the pool means ALLOC has something to win from tick one. Example: 4,000 millicores gives ~4 millicores per body and 2,000 in the pool.
- **Selector numbering: nearest to self first [decided — kept deliberately, with a shuffled-variant control run; see the notes below].** The rule "zero means nothing" applies to *amounts* (CX). A *selector* (BX) is an index, and some variant must be 0. Ordering rule: 0 = the variant closest to the program itself, counting outward. Examples:
  - SENSE: 0 = my size, then my holdings and heat, then the node, neighbours, signals;
  - ALLOC: 0 = memory, then CPU, GPU, disk;
  - DIVIDE: 0 = here, k = neighbour k;
  - DISK: 0 = compare (changes nothing), then store, load.

  This is an ordering principle, not a pick of "what's useful for replication". It does happen to make the 6.2 sketch work without setting BX.
- **Sparks: an ongoing abiotic flux [decided — adopted by Damian].** Without it, the random start is a one-shot lottery that ends when the starting bodies wear out. At a low configurable rate, the world takes a random stretch of waste (or free memory) and gives it a CPU and a small grant from the free pool, making a new body. Sparks:
  - keep the lottery drawing until a replicator appears;
  - re-animate leftover building blocks from dead bodies (the stepwise assembly Amoeba saw);
  - can keep running after life appears, where they act as food, much as prebiotic chemistry is eaten by life that already exists.

  A spark is the environment creating a program, not acting on behalf of one, so it's consistent with the neutrality principle. It draws only from the free pool.
- **Mutation and hazards at start:** Ray's rates as the baseline (about 1 background flip per 10,000 instructions executed; about 1 copy error per 1,000–2,500 copied), scaled by heat. BFF found replicators with or without mutation, so these are tunables, not prerequisites.
- **Where.** The abiogenesis test runs on one node. In the full world, every node starts with its own random soup by default (many origins), optionally with soup density or spark rate following the biome noise, so some nodes are "warm pools".
- **Knowing it happened.** Track genotype counts and how compressible the soup is. When replicators take over, the soup becomes repetitive and compresses much better. The Google team tracked a compression-based measure for exactly this.

**Open:** spark rate, grant size and body-length range (tuned in the abiogenesis test, 6.4 #25); the full selector list for each verb. *Resolved:* the fill and pool fractions are gone (sparks-only start); sparks continue after life (#21); mutation baseline (#22); many origins (#23); compressibility detection (#24).

**Talked through (2026-11-05), Claude's notes:**
- *Why random fill, not zeroed memory.* In codon table v1, codon 0 is NOP0, so "zeroed" memory would be a sea of NOP0 labels, not empty space. Random fill also matches Tierra, where newly allocated memory keeps whatever was there. ALLOC doesn't clear memory, so an incomplete copy inherits garbage in its unfilled part — a natural source of variation.
- *A sparks-only start* **[decided — adopted by Damian]**. Start with all memory as waste and all resources in the free pool, with a high initial spark rate tapering to the background rate. The initial carving into bodies is then just "lots of sparks at t=0". This removes two arbitrary choices (fill fraction and the equal half-share split) by folding them into spark settings we need anyway: one mechanism instead of two.
- *Selector ordering is a big lever, not a detail.* If selector 0 weren't the self-variant, the minimal replicator would need about 3 more instructions to set BX, making it ~32,768× rarer at 1/32 per core instruction. Options: keep nearest-to-self as a deliberate choice, and/or add a *shuffled-selector control run* that randomises each verb's selector numbering, to measure how much results depend on it. **Decided: both.** Nearest-to-self is the default, and every abiogenesis experiment also runs a shuffled-selector control.
- *One origin or many.* Many origins (a soup on every node) means competition between independently arisen lineages. A single origin node gives one clean family tree spreading by dispersal. Config either way.

### 6.4 Sweep decisions (2026-11-05) [decided — Damian accepted all]

These close the remaining small open items from 6.1–6.3 and 4.5.

**Found while checking rules against the principles**
1. **Death.** A body with no running CPU dies. Its holdings return to the free pool and its memory becomes waste. A CPU halts after K = 8 *consecutive* errors (tunable). Without this, a body whose CPUs had all halted would hoard resources forever, breaking "nothing grants permanence". Every cause of death runs through this one definition: being jammed by an eater, piling up errors, or code too corrupted to make progress. *Phase 8 (6.14):* with warps on, a body dies when it has neither a running CPU nor a running warp (a warp with a lane not halted, and GPU units to run on); a lane halts after K consecutive errors like a CPU, and a warp whose lanes have all halted ends.
2. **Freed units keep their heat** and cool at the normal rate. A reset on release would let a program RELEASE a hot unit and ALLOC it straight back cold, dodging heat entirely.
3. **INJECT overwrites what's at the landing spot; it never inserts.** Inserting would grow the victim, meaning the world claims memory on someone's behalf. Lasting, provirus-like infections can still evolve by landing on the victim's junk regions.

**Principles**
4. "The world never acts on a program's behalf" applies to every verb (Section 1).
5. Never add an external score; the only judge is who is still around.

**Invasion**
6. ENGULF and INJECT share one graded surface-match contest. (The earlier idea of placing them one bit from MAL and COPY is obsolete; table v1 pairs them at codons 56/57.)
7. Attacks cost CPU time proportional to what they move, plus heat. A failed contest still costs the attempt. There is no separate currency.
8. Engulf rules 1–3 are decided: an engulfed program keeps running; every CPU in a body shares its write rights; a body pays for all its CPUs.
9. A body's leading template is its public surface. Everything else is private, template searches stay inside the searcher's own body, and waste is open to all.

**Instruction set**
10. Stack depth is 8.
11. READ and WRITE stay. They're needed for drift checks, writing "eat me" markers, and building templates.
12. INJECT and ENGULF are not doubled by default; doubling them is an abiogenesis experiment.
13. The soup's opcode mix is uniform codons (via the sparks-only start).
14. An instruction pointer that runs past the end of its body wraps to the body's start.
15. Already realised by table v1: how the extra slots split between computing and touching, the generalised ALLOC, and self-check immunity via DISK compare.

**Resources and heat**
16. Scheduler: CPU is held in millicores, and each tick a body executes instructions in proportion to the CPU it owns.
17. Heat is per owned unit, plus a weak node-level ambient leak (set low). This revives 4.5's temporal niche partitioning without overriding ownership.
18. Heat raises error rates per resource (memory bit flips, CPU flaws, copy errors), with error onset staggered before throttling.
19. Passengers heat their host's cores. This falls out of the rules.
20. There is no backstop cull. Hazards, heat release, engulf and the death rule free space, and sparks recycle it. Revisit if a node clogs in testing.

**Start and runs**
21. Sparks continue after life appears, acting as food.
22. Mutation baseline: Ray's rates, scaled by heat.
23. Many origins by default; a single origin is a config option.
24. Life is detected by genotype counts plus soup compressibility.
25. Deferred: spark rate, grant size and body-length range are tuned during the abiogenesis test.
26. The Tierra clone (seeded, open membrane) exists only as an engine-validation mode. *Narrowed in 9.1 to a light Tierra mode: Ray's instruction set as a codon table plus an open-membrane flag, checked against his published 839/813 replication cost. No reaper or slicer is built.*

### 6.5 Interpreter semantics v1 (Phase 1a) [decided — built 2026-11-05 from the Phase 1a plan Damian approved, with its eight open points settled as recommended, marked (A)–(H)]

The exact rules the interpreter implements, so this document stays the source of truth. Code: `core/src/interp.cpp` (instructions), `core/src/soup.cpp` (ownership, ledger, tick), `core/src/isa.cpp` (codon tables).

**Memory and ownership**
- A node's soup is six-bit codons. Each codon is owned by a *block* or is waste. A block is a contiguous range claimed in one go (a spark, an ALLOC) and belongs to one body. A body's first block is its own code, and that block's leading NOPs are its surface.
- *Private code* (the default): a CPU executes and writes only codons its body owns, and reads its own body or waste. With `isa.membrane = open` (the light Tierra mode) it may read, search and execute anywhere; writes stay within its own body, which is Tierra's rule too.
- *Wrap (6.4 #14, generalised — (B))*: the IP and both heads wrap at the end of the block they are in. In waste, and in open mode, they wrap at the soup's end. Blocks never wrap the soup's end.
- Executing a codon the body doesn't own fails, and the IP moves on.

**CPU.** Registers AX, BX and CX (plus DX, used only by Tierra opcodes); an 8-deep stack (`isa.stack_depth`); the IP; read and write heads; an error flag saying whether the last instruction failed; and a count of consecutive errors.
- Registers are 32-bit. Arithmetic wraps; IFLT and DIV compare and divide signed.
- A new CPU starts with registers at zero and its IP and both heads at its block's start.

**Templates** (Tierra's V3+ conventions — the ones that reproduce Ray's documented 827/809, below):
- A template is the run of NOPs after an instruction, at most `isa.max_template` (8).
- The search looks for the complement, outward from the instruction and forward first at each distance, up to `isa.search_limit` (400). Forward windows start right after the template; backward windows end right before the instruction. Under private code a match must lie in the searcher's own body.
- *Success:* a jump lands on the instruction after the match, wrapping within that block.
- *Failure, or no template at all:* the error flag is set and the IP skips the instruction and its template (zero means nothing).
- Template instructions that don't jump (FIND, a DJNZ or JZ that falls through) skip their template. CALL pushes the address after its template.

**Instructions (table v1)**

| Instruction | Effect | Fails when |
|---|---|---|
| NOP0, NOP1 | nothing (template bits) | — |
| JMP | template jump | no template, no match |
| IFZ / IFLT / IFNE / IFERR | skip the next instruction unless CX = 0 / AX < BX / AX ≠ BX / the previous instruction failed | — |
| INC, DEC, ZERO | CX + 1, CX − 1, CX = 0 | — |
| ROT / ROTB | AX → BX → CX → AX / the reverse | — |
| PUSH / POP | CX onto / off the stack | stack full / empty |
| COPY | [write head] ← [read head], with the copy-error chance; both heads advance | read or write not allowed, or a hot page refuses the write (heads stay) |
| FIND | AX = address after the match | as JMP |
| SENSE | CX ← reading BX (below) | other BX |
| ALLOC | BX 0: claim CX codons of waste as a new block, the nearest free run outward from my block (forward on a tie); the write head moves to its start. BX 1: claim CX millicores, chosen at random from the pool and capped by it. CX ≤ 0 does nothing. | no room; empty pool; BX 2 (GPU), 3 (disk) or other |
| DIVIDE | the latest allocated block becomes the daughter, with one CPU at its start; she gets floor(holding × CX / 64) of each non-memory holding, with CX clamped to 0–64 | no fresh block; BX ≠ 0 (dispersal comes in Phase 3, and a node without that edge will fail the same way — (G)) |
| CALL / RET | push the address after the template, then jump / pop the IP | no match or stack full / empty stack |
| DJNZ / JZ | CX − 1, then jump unless CX = 0 / jump if CX = 0 | as JMP, when it jumps |
| ADD, SUB, MUL, DIV, AND, OR, XOR | CX = AX op BX | DIV by zero |
| NOT, SHL, SHR, NEG | act on CX | — |
| RAND | CX = a random value in [0, soup size) | — |
| SWAP | AX ↔ BX | — |
| SETH / GETH | head BX (0 = read, 1 = write) ← AX / AX ← head BX | BX > 1; SETH with AX outside the soup |
| READ / WRITE | CX ← [read head], then it advances / [write head] ← CX & 63, then it advances | not allowed; a hot page refuses the write |
| ENGULF, INJECT | contact (below; Phase 1b gives INJECT its own criteria, 6.6) | no target in range, or the contest is lost |
| EMIT, LISTEN, DISK, GPU | off in Phase 1a | always |
| YIELD | the CPU idles for the rest of this tick (and so cools) | — |
| RELEASE | BX 0: the last CX codons of the latest block (all of it if CX ≥ its length). BX 1: CX millicores, oldest first, keeping their heat. | no latest block; no millicores; other BX |

SENSE readings, nearest to self first (6.3):
0. my block's size;
1. memory I own;
2. my millicores;
3. the mean heat of my millicores;
4. the heat of the page I run in;
5. free millicores on the node;
6. waste codons on the node;
7. my CPU count.

**Scheduler (6.4 #16).**
- Each tick a body executes Σ speed(unit heat) × `cpu.instr_per_core` / 1000 instructions. Fractions carry over, and so does overdraft from attack costs, as debt.
- The budget splits equally among its running CPUs, and the remainder rotates by tick.
- Bodies run their whole budgets in creation order, starting from a body picked each tick by the node RNG (7.1 D). Bodies born during a tick first run the next tick.

**Death (6.4 #1, (A)).**
- A body dies in the sweep at the end of the tick if it has no un-halted CPU, *or owns no millicore*. So a daughter given CX = 0 dies the tick she is born, and so does a body whose CPU all overheated away.
- Its blocks become waste, with their contents kept for scavengers. Its millicores return to the pool with their heat.
- A CPU halts after `cpu.halt_after` (8) consecutive errors.

**Sparks (6.3).**
- `spark.initial` sparks fire at tick 0, then `spark.rate` per tick, which is a live knob.
- A spark turns a random stretch of waste, `spark.min_len`–`spark.max_len` codons long, into a body with one CPU and `spark.grant` millicores drawn at random from the pool.
- There is no spark while the pool is empty.

**Heat (4.5, 6.4 #2, #17, #18, (C)).**
- Every millicore and every 64-codon memory page has a heat.
- *Inputs:* a body's millicores each gain `heat.cpu_gain` × its utilisation per tick (instructions charged ÷ its nominal share). A page gains `heat.fetch_gain` per instruction fetched from it and `heat.write_gain` per codon written into it.
- *Cooling:* everything loses `heat.cool` of its heat per tick.
- *Curves, per resource:* the error multiplier rises from `warm` to `err_max` at `max`, and speed falls from `hot` to `speed_min` at `max`. Past `max` the unit is freed with its heat; a freed page's codons become waste. A page above `hot` accepts a write only with probability speed(h).
- *Defaults:* CPU warm 40, hot 60, max 100. Memory 40, 70, 100. Cooling 0.1. `heat.cpu_gain` 6, which settles a flat-out body at 60, exactly the throttle onset.

**Hazards (6.3, 6.4 #22).** Ray's baselines, each multiplied by the heat error multiplier:
- *Copy errors:* 5 × 10⁻⁴ per codon copied, flipping one random bit.
- *Execution flaws:* 10⁻⁴ per instruction; one of AX, BX or CX comes out off by ±1.
- *Bit rot:* per codon per tick, scaled by its page's multiplier. The default equals Ray's one flip per 10,000 instructions at full load.

**Contact (6.4 #3, #6–9, (D)–(F)).**
- ENGULF and INJECT read the template after them. Every other body whose surface starts within `contact.range` (256) codons of the IP is a candidate.
- The best candidate scores highest on matches / max(n, m), where n is the template length, m the surface length, and a match is a complementary bit; 0/0 counts as 1. Ties go to the nearest, then the oldest. The attack succeeds with probability equal to the score.
- *Cost:* 1 + `contact.cost_base` (8) instructions per attempt, plus `contact.cost_per_codon` (1) per codon moved on success. Overdraft is carried as debt, and the whole cost heats the attacker's millicores.
- *ENGULF* moves all of the prey's blocks, its CPUs (which keep running, now as the eater's) and its millicores (with their heat).
- *INJECT* overwrites up to CX codons of the victim's first block from the read head, starting at that block's start + AX and wrapping within the block. CX ≤ 0 does nothing.

**Light Tierra mode (9.1).** `isa.table = tierra0` and `isa.membrane = open` select Ray's instruction set 0 with the semantics his V3+ releases document:
- *adr, adrb, adrf:* AX = the address after the match, CX = the template size; the IP skips the template.
- *call* pushes the address after its template; *ret* pops; *ifz* skips one instruction; *mal* claims CX codons and puts their address in AX.
- *divide* hands the daughter `tierra.divide_share`/64 of the CPU (default 32), because Ray's divide has no amount operand.
- Every instruction costs 1.

*Result:* Ray's ancestor 0080aaa replicates in **827 instructions for its first copy and 809 for each after**, and breeds true.

*839/813:* Ray's 1991 and 1992 papers give 839/813. Those figures came from his original 1990 simulator, which is lost. Every later release of Ray's — V3.0 (1991) through V5.0 on tomray.me — lists the byte-identical genome at 827/809 under the semantics above. The 12- and 4-instruction gaps are exactly the template NOPs after adrb, adrf and call. Reaching 839/813 needs two behaviours no Ray source documents: adr stepping by one, so its template NOPs execute, and call returning onto its own template. As the brief asked, this is reported, not tuned to.

**Findings while building (for Phase 1b).**
1. *A heat cliff.* Under uniform load a body's millicores share one heat history, so if they cross `max` they are freed in the same tick and the body dies.
   - Throttling keeps the default gain below `max`; in practice only attack debt pushes units over.
   - At `heat.cpu_gain` 45, flat-out bodies lost everything within about 3 ticks and a test world collapsed.
   - A rule of "release at most one unit per body per tick" would remove the cliff, if wanted.
2. *Blind predation.* Most ENGULFs in random code are blind (no NOP follows them), and most bodies have no surface. With 0/0 = 1, random code eats nearby surfaceless bodies with certainty. Watch for this in the abiogenesis test.
3. *The sketch's lineage is bounded by its initial grant.* Each generation gets 1/64 of the previous one's CPU, and the zero-share generation dies. Sustained replication needs CPU claims or sparks. The sketch also replicates more than once only because heads wrap (B).

### 6.6 Abiogenesis rules and detection v1 (Phase 1b) [built 2026-11-05 from plan revision 3, approved by Damian as recommended; one call open, marked [open]]

The plan, its pilot findings and the design calls D1–D10 are in `docs/phase1b-plan.md`. This section is what the engine now does. Code: `core/src/soup.cpp` (rules), `core/src/isa.cpp` and `interp.cpp` (tables and selectors), `core/src/lineage.cpp`, `genebank.cpp`, `compress.cpp` (detection), `core/src/run.cpp` and `app/main.cpp` (running), `tools/abio.ps1` (the harness).

**Memory upkeep (D1) [decided].** Each tick, before it runs, a body pays `mem.upkeep` × m × (m/64)^(`mem.upkeep_exp` − 1) instructions, where m is every codon it owns (blocks being filled and engulfed parts included).
- Defaults: `mem.upkeep = 2`, `mem.upkeep_exp = 4/3` (the square-cube law: cost relative to intake grows as size^(1/3)). `mem.upkeep = 0` (with INJECT's Phase 1a contest, below) is exactly Phase 1a: the busy soup world of the tests hashes to Phase 1a's `decce3c361397145`.
- Upkeep is charged as work, so it heats the body like instructions do. Whatever the budget can't cover is carried as debt (`carry` goes negative) and the body runs nothing until it is repaid.
- Heat is what turns debt into death: an over-extended body throttles, its millicores pass `max` and are freed, and the death sweep releases its memory. A 10-millicore body holding 30,000 codons owes about 466,000 instructions and dies in its first tick. *With heat off* a debtor just stops running and keeps its memory, which is acceptable only in tests.
- No hard limits: RELEASE, growth and several daughters at once all stay as in Phase 1a, and cost upkeep.

**CPU wear and spark timing (D10) [decided as knobs; abiogenesis defaults set from the sweep in finding 2].**
- `cpu.wear` (default 0): each held millicore fails back into the pool, keeping its heat, with this chance per tick. Sampled as a Poisson process over the unit index, after bit rot. Counted in the `cpu_worn` column.
- `spark.timing` (default `first`, as Phase 1a): `last` fires the tick's sparks after the bodies have run, so chemistry takes only what life left. The spark count is drawn at the start of the tick either way.
- `configs/abio.cfg` sets `cpu.wear = 0.001` and `spark.timing = last` for the abiogenesis trials.

**Spark knobs (D4) [decided].** `spark.grant`, `spark.min_len` and `spark.max_len` are live knobs like `spark.rate`: scheduled, logged as replay `knob` records and carried in snapshots. Each length is range-checked alone; while a change leaves min above max, sparks use the range between them.

**INJECT's criteria [decided — Damian, 2026-11-05, after finding 1].** INJECT was the shortest route to replication (it copies CX codons in one instruction, and blind injection always beat a surfaceless body), so it now needs two things:
- *A weakness that matches the injector* (`contact.inject_key`, default on): the host's weakness is its surface, the only public part of a body. The injector's key is the template after INJECT, and success has probability matches / max(n, m) as before, except that 0/0 now counts as 0. A blind injector, or a host with no surface, gets nowhere, so a surfaceless body is immune to INJECT and each surface is open only to the keys that complement it. ENGULF is unchanged (0/0 = 1), so a surface is a trade-off: armour against blind eaters, a doorway for matching injectors.
- *A host the payload fits in* (`contact.inject_room`, default 1): the host's first block must hold room × CX codons. If it doesn't, nothing is written, the host is unharmed, and the attempt still costs the injector (1 + `contact.cost_base`). Phase 1a truncated the payload to the host instead.
- `contact.inject_key = false` with `contact.inject_room = 0` is exactly Phase 1a: with those and `mem.upkeep = 0`, the busy soup world still hashes to `decce3c361397145`.

**Instruction-set controls (D6–D8) [decided].**
- *Plain-64 (`isa.table = nova_v1_plain`):* codon 2k is core instruction k as in `nova_v1`; codon 2k+1 is, in order, NOPX, JMPF, JMPB, IFEQ, IFGT, MOD, MIN, MAX, DUP, DROP, INCA, DECA, INCB, DECB, ZEROA, ZEROB. Codons 32–63 are unchanged, so every one of the 64 codons is a different instruction.

  | Instruction | Effect | Fails when |
  |---|---|---|
  | NOPX | nothing; not a template bit, so it ends templates | — |
  | JMPF / JMPB | template jump, forward only / backward only | as JMP |
  | IFEQ / IFGT | skip the next instruction unless AX = BX / AX > BX (signed) | — |
  | MOD | CX = AX mod BX (signed, C remainder) | BX = 0 |
  | MIN / MAX | CX = the smaller / larger of AX and BX (signed) | — |
  | DUP / DROP | push the top of the stack again / pop and discard | empty or full stack / empty stack |
  | INCA, DECA, INCB, DECB, ZEROA, ZEROB | AX ± 1, BX ± 1, AX = 0, BX = 0 | — |
- *Doubled invasion (`isa.table = nova_v1_inv2`):* codons 58 and 59 (EMIT and LISTEN, which always fail until Phase 4) become ENGULF and INJECT, one bit-1 flip from 56 and 57.
- *Selector numbering (`isa.selectors = nearest|shuffled`, `isa.selector_seed`, 0 = the world seed):* every BX-selected verb reads its variant through a map: SENSE (8 readings), ALLOC and RELEASE (one shared map over memory, CPU, GPU, disk), SETH and GETH (2 heads) and DIVIDE (4 destinations, 0 = here). `nearest` is the identity, so Phase 1a behaviour is unchanged. `shuffled` draws a uniform permutation of each map from the selector seed, redrawn until the self variant is not at 0, so every shuffled trial tests something; with two heads that forces read and write to swap. The drawn maps are printed at the start of a run and written to `result.txt`.

**Detection (D2) [decided; extension open, see the findings].** A pure observer, one per soup (`detect.enabled`, default on): the soup tells it about new blocks, freed blocks and DIVIDEs; it never writes to the soup or draws from its RNG, and a test checks the world hash is identical with it off.
- *Inheritance links* (extended by Damian after finding 1), each counted as same-offset matches / the longer length against `detect.similarity` (0.75; two random blocks match on about 1.6%):
  - *DIVIDE:* the daughter block matches the block the dividing CPU runs in, counting only codons the parent's body wrote into her (COPY, WRITE, movii) since her ALLOC (`detect.written`, default on), so budding onto a relative's intact remains is not copying. A CPU running inside the block it divides off is not a link.
  - *INJECT (`detect.inject`, default on):* the codons an INJECT just wrote leave the victim's first block matching the injector's block (the block its CPU runs in). The victim joins the injector's lineage one generation deeper; a member re-infected by its own lineage stays as it was, and a member of another lineage moves.
- *Lineage:* the first link from an untagged parent founds a lineage, with the parent as its depth-0 founder; each inheriting daughter joins her parent's lineage one generation deeper. Members are blocks, so an engulfed replicator that keeps running stays a member. Mutants are full members.
- *Gate:* a lineage with a member at depth ≥ `detect.gate_depth` (2) and at least `detect.gate_alive` (2) members alive at once. Both numbers only grow at links, so the check runs at the exact moment within the tick. The thresholds are knobs because, with INJECT links, 2/2 is met by chance (finding 1).
- *Genebank:* every member's genotype (its codon hash when it joined) is counted while it lives; when a genotype's living count first reaches `detect.genebank_threshold` (2) it is saved to `genebank.txt` once, with a Tierra-style name (`0008aaa`: length, then letters in order of saving for that length), its first parent's name, its lineage and the genome as op names.
- *Lineage log:* `lineages.csv` gets a row when a lineage goes extinct and, at the end of a run, one per living lineage: founded and ended ticks, max depth, peak living members, members ever, founder length and genotype, and whether it met the gate.
- *Compressibility:* a greedy LZ77 bit-cost estimate of the whole soup (`compress.h`): literals at their zero-order entropy, a flag per token at the binary entropy of the match rate, 16-bit offsets and Elias-gamma lengths, over 6 bits a codon. Uniform random codons give about 1.0, a soup of copies well below.
- *CSV columns* (after the Phase 1a ones and the new `upkeep`, `cpu_worn`): `links` and `inject_links` in the window; at the window's end `max_depth` (deepest living member), `lineages` (with a living member), `top_lineage` (living members of the largest) and `genotypes_in_top`; and `compress`. `lineages.csv` also splits each lineage's links into `divide_links` and `inject_links`, and `result.txt` gives `inject_links` and the gate link's kind (`detect_via`).
- *Barrier and replay:* everything a detector produces during a tick waits in its own buffers and the run drains them at the barrier in node order. A gate event becomes an `event <tick> replicator node=… lineage=… depth=… alive=… len=… genotype=… via=divide|inject` record in replay log v2, and `nova replay` fails if the replayed events differ. Version 1 logs still load. Detector state is not in snapshots; a resumed run starts detection afresh.

**Running a trial (D3, D5, D9) [decided].**
- `nova run` takes `--set key=value` (applied before anything reads the config, so it is logged with it), `run.ticks = 0` for no tick limit, `run.seconds` (stop at the first tick boundary after that much wall-clock time), Esc in the console and `--stop-file PATH` (the harness's stop), and `run.after_replicator` (stop that many ticks after the first gate; −1 keeps going). Every stop finishes all outputs, and the replay `end` record carries the tick, so a clock- or key-stopped run replays exactly.
- `result.txt` (`output.result`) records seed, end tick, why it stopped, seconds, instructions, hash, table, selectors, links, lineages founded, max depth, and for a detection its tick, the instructions run up to the end of that tick, lineage, depth, living members, length, genotype and genome.
- `tools/abio.ps1` runs arms × seeds as single-node processes, resumable, Esc stopping all of them gracefully. How much of the machine it takes is a set of knobs (Damian, 2026-11-05): `-Percent` (default 75: as many trials at once as 75% of the logical processors, 18 on the home rig; 100 for an overnight run), or `-Parallel N` exactly, and `-Priority Idle|BelowNormal|Normal` (default below normal, so the desktop stays responsive). Arms: `baseline`, `shuffled`, `plain`, `inv2`, plus custom ones (`-ArmSet`). Every arm runs the same seeds, and seed s starts from the same random soup in each. `summary.csv` gives per arm the trials, detections, exposure (instructions until detection, or until the end for a miss) and λ = detections / exposure, the maximum-likelihood rate for exponential waits with censoring.

**Findings (2026-11-05, home rig).**
1. *Replicators appear within seconds, and the first ones copy themselves with INJECT, not COPY.* In the harness smoke test (one seed per arm, about 18 s each), the baseline arm met the gate at tick 18,126 (4.1 × 10⁸ instructions) and the inv2 arm at tick 13,206; a stop-file test of seed 5 met it at tick 2. Both detections replay exactly from their logs.
   - The baseline replicator is 32 codons with no COPY. Each cycle it ALLOCs a block the size of itself and DIVIDEs it off with half its CPU (CX = 32 at DIVIDE), as an unwritten block of waste; its blind INJECT then overwrites the nearest surfaceless body — usually that fresh daughter — with its own 32 codons. Its leading NOP0 is a surface that makes its copies immune to the same blind INJECT. Exact copies sat at 32-codon spacing across that region of the soup.
   - INJECT copies CX codons of the injector's code in one instruction, so the route is far shorter than the 8-codon COPY loop the 6.2 estimate assumed (2^-40 per window). A bare SENSE INJECT is already a virus that never divides.
   - The DIVIDE-time detector can't see this route: the daughter is waste when she is born and becomes a copy afterwards. It fired only because a later ALLOC landed on a dead copy's intact remains — written by the replicator's own INJECTs — so the daughter matched at DIVIDE. The seed-5 tick-2 detection is the same thing with a random 31-codon spark.
   - *Decided (Damian, "Yes, extend it"):* (a) an INJECT that leaves the victim's first block matching the injector's block is an inheritance link, and (b) a DIVIDE link counts only codons the parent's body wrote since the ALLOC. Both are built (the detection bullets above) and can be switched off (`detect.inject`, `detect.written`).
   - *What the extended detector then sees* (all four arms × seeds 1–3, 2 minutes each, `configs/abio.cfg`): random budding makes no DIVIDE links at all, but every arm meets the 2/2 gate at tick 0–5,000, because random code is full of short INJECT chains — a spark that happens to hold SENSE … INJECT overwrites a similar-sized neighbour with a copy of itself, and the neighbour sometimes does it again.
     - Each trial founds 270–3,800 lineages, about 90% of them depth 1.
     - The biggest are tiny: founders of 2–22 codons, with only INJECT links, peaking at 46–107 members alive and reaching depth 17 at most. Their victims must be about their own size, so they are bodies budded off by something — whether by the lineage itself or by other programs, these data can't tell.
     - A 10-generation, 10-alive bar is still crossed in 7 of the 12 trials within two minutes, by INJECT-only lineages.
     - So with INJECT links the 2/2 gate no longer separates replicators from chance, and raising the numbers alone mostly selects these small INJECT lineages.
   - *Raised with Damian:* the distinction that matters is self-replication against infection. Instead of changing the gate, Damian gave INJECT its own criteria (the INJECT rules above: a key matching the host's surface, and a host the payload fits in).
   - *With INJECT's criteria* (the same 12 trials, 2 minutes each):

     | arm | gate met | first gate (ticks) | DIVIDE links | INJECT links | lowest compressibility |
     |---|---:|---|---:|---:|---:|
     | baseline | 3 / 3 | 5,983 – 166,695 | 4,600 – 14,000 | 2 – 31 | 0.69 – 0.78 |
     | inv2 | 3 / 3 | 21,711 – 114,277 | 10,900 – 949,000 | 24 – 76 | 0.76 – 0.83 |
     | plain | 0 / 3 | — | 0 – 6 | 0 – 1 | 0.87 – 0.92 |
     | shuffled | 0 / 3 | — | 0 – 8 | 2 – 6 | 0.91 – 0.97 |

     - Keyed injections still land (5,000–140,000 a trial) but almost never leave a copy: INJECT is now gene transfer and vandalism, not replication. The INJECT chains and tiny viruses are gone.
     - The gate is met only through COPY: lineages whose daughters were written by their parents. The biggest are 6–12-codon COPY replicators with up to 21 members alive at once and up to 2,275 members over 8,500 ticks, yet depth stays at 2–4: most copies never reproduce themselves. Persistence is still the open problem, as the pilots predicted.
     - The paired controls now separate clearly (three seeds each, so directional): with the doubled core removed (plain) or the nearest-to-self numbering shuffled, no replicator appeared in any seed within two minutes, while doubling INJECT/ENGULF (inv2) did not slow emergence.
     - So the 2/2 gate means what D2 intended again, and the paired experiment can run.
2. *The D10 sweep: CPU wear is what keeps the soup alive.* `configs/abio.cfg`, 4 seeds per setting, 2 minutes each (16 processes at once), with the gate left running:

   | setting | instructions per tick | free CPU pool | sparks per tick | lowest compressibility | gate met | longest lineage |
   |---|---:|---:|---:|---:|---:|---:|
   | no wear, sparks first | ~70 | 0 | 0.002 | 0.96 | 0 / 4 | one frozen pair |
   | no wear, sparks last | ~105 | 0 | 0 | 0.97 | 0 / 4 | frozen |
   | wear 0.001, sparks first | ~21,000 | ~8 | 1.3 | 0.58 | 1 / 4 | 3,141 ticks |
   | **wear 0.001, sparks last** | **~29,000** | **10–31** | **0.6** | **0.67** | **4 / 4** | **5,742 ticks (peak 35 alive)** |

   - Without wear the soup freezes within the first few thousand ticks: the bodies alive hold all 4,000 millicores yet run about a hundred instructions a tick between them (of 100,000 possible), and sparks stop. Nothing makes held CPU cost anything, so bodies that run little keep their units indefinitely — "nothing grants permanence" broken for CPU, as D10 suspected. (Which behaviours freeze it — idling, debt, halting loops — was not diagnosed.)
   - With wear 0.001 and sparks last, every seed met the gate (first at ticks 18,126–29,555) and 17 lineages met it in all; one lineage reached depth 373. (Gate counts here are from the DIVIDE-only detector, before the extension in finding 1; the soup-health numbers don't depend on detection.)
   - Under every wear setting the soup's compressibility fell to 0.58–0.75, so copies filled it even in runs where the gate never fired — more evidence that the DIVIDE-time detector misses most replication (finding 1). Sparks first gave the most repetitive soups and the fewest detections.
   - So the abiogenesis default is `cpu.wear = 0.001`, `spark.timing = last` (as D10 recommended).
3. *Phase 1a is intact.* With `mem.upkeep = 0`, `contact.inject_key = false` and `contact.inject_room = 0` the busy soup world reproduces Phase 1a's hash; the Phase 0 demo hash is unchanged; the light Tierra mode (run without upkeep, which Tierra doesn't have) still gives 827/809.
4. *Machine:* the home rig is an Intel Core Ultra 9 285K (24 threads), 64 GB, Visual Studio 2026. Benchmarks are in 7.1 I. Windows Smart App Control is on and now and then refuses to run a freshly linked unsigned test binary; a rebuild (or the Release build) gets through.

### 6.7 The in-world GPU (Phase 2) [built 2026-11-05 from the Phase 2 plan, approved by Damian as recommended]

The plan and its calls G1–G9 are in `docs/phase2-plan.md`; this is what the engine does. It realises 5.1 D–E and 7.1 G. Code: `core/src/soup.cpp` (ledger, heat, wear), `core/src/interp.cpp` (the op), `core/src/lineage.cpp` (GPU links).

**No GPU, no change.** `world.gpus` (GPU units per node) defaults to 0. Then every GPU selector fails as it did in Phase 1 — the GPU op, ALLOC and RELEASE with the GPU selector, SENSE readings 8–10 — and the world hash, snapshots and shuffled selector maps are byte-identical to Phase 1b (the pinned hashes are unchanged, and the Phase 1b smoke trials reproduce to the tick).

**The op (G1).** `GPU`: BX picks the op through the selector map — 0 compare, 1 copy, 2 add, 3 xor — and CX is the element count.
- Compare sets CX to how many codon pairs [read head + i], [write head + i] differ and writes nothing. Copy writes [write head + i] ← [read head + i], with the copy-error chance scaled by the body's mean GPU heat. Add and xor combine into the write head, mod 64.
- Both heads advance by the elements done and wrap as COPY's do. Private code applies per element (reads from the body or waste, writes into the body; a hot page may refuse), and the op stops at the first element it may not do.
- It does as many elements as the body's GPU capacity has left this tick. Copy, add and xor leave the elements not done in CX (0 when finished), so a loop can carry on next tick; compare leaves the mismatch count.
- Cost: `gpu.setup` (4) + `gpu.cpu_per_element` (1/16) × elements, at least 1. It fails, costing 1, with no GPU, no capacity left, CX ≤ 0, an op number past 3, or nothing done.

**Capacity, holdings and heat (G2–G4).**
- Each held unit does `gpu.elements` (32) element-ops a tick at full speed, times its heat speed factor. A body's capacity is set at the start of each tick from the units it holds then; units gained mid-tick (a claim, an ENGULF, a birth) add capacity from the next tick. Unused capacity is lost.
- GPU units are held like millicores: ALLOC with the GPU selector claims CX of them at random from the GPU pool, capped by it; RELEASE gives back CX, oldest first; DIVIDE hands the daughter floor(units × CX/64); ENGULF moves them all; death returns them. Freed units keep their heat. GPU units alone don't keep a body alive.
- `gpu.wear` (0) returns held units to the pool like `cpu.wear`.
- Heat: each unit gains `heat.gpu_gain` (9) × its owner's utilisation (element-ops done ÷ units × `gpu.elements`) a tick and cools at `heat.cool`. Curve `heat.gpu.*` (warm 40, hot 60, max 100); past max a unit is freed, hot. A body using its whole capacity every tick settles where h = 90 × speed(h), about 71: throttled to roughly 79% of its capacity.
- The per-body GPU lists live beside `Body` (which is hashed raw), and GPU state is hashed and snapshotted only when the node has GPUs (G9).

**Sensing and the shuffled control (G5, G6).** With GPUs, SENSE has 11 readings: 8 = my GPU units, 9 = their mean heat, 10 = free GPU units on the node. The shuffled arm draws the SENSE map over all 11 and also draws a 4-entry GPU op map, redrawn until compare isn't left at 0. Without GPUs it draws exactly Phase 1b's maps.

**Detection (G7).** GPU writes count as the parent's writes for `detect.written`. A DIVIDE link is a *GPU link* when most of the daughter's matching codons were written by GPU ops (a later COPY or WRITE over a codon takes it back). The first GPU link of a lineage that has met the gate is a replay `event … gpu_replicator …` record, re-checked by `nova replay`. CSV columns `gpu_elements`, `gpu_released`, `gpu_worn`, `gpu_held` and `gpu_links`; `lineages.csv` has `gpu_links`; `result.txt` has `gpus`, `gpu_links` and the first `gpu_replicator`.

**Harness and bench (G8).** `tools/abio.ps1` has a `gpu` arm (`world.gpus = 400`, `gpu.wear = 0.001`). `nova bench` has a `gpu` workload: a hand-written 12-codon GPU replicator (`SENSE PUSH ALLOC ZERO INC ROTB POP GPU ZERO ROTB SENSE DIVIDE`, seeded only in tests and the bench) with 400 GPU units — 30 million CPU instructions a second on one thread, 312 million on 24, each GPU op also doing 12 element-ops.

**Findings (2026-11-05, short check: `baseline` vs `gpu`, seeds 1–3, 2 minutes each).**
1. *GPU off is exact:* the baseline trials met the gate at the very ticks they did before Phase 2 (5,983, 139,220, 166,695).
2. *The GPU speeds up emergence:* the `gpu` arm met the gate in all three seeds, at ticks 8,403, 26,257 and 47,279, for a rate per instruction about 4× the baseline's (1.5 × 10⁻⁹ against 3.5 × 10⁻¹⁰; three seeds, so directional).
3. *Random code grabs the GPU:* 370–397 of the 400 units were held on average, even with wear, and random programs ran 12–15 million GPU element-ops per trial.
4. *GPU copying arises from random code:* seed 1 reached the `gpu_replicator` milestone at tick 82,182 — a 21-codon lineage (`INC EMIT IFLT DJNZ DJNZ ALLOC ZERO ZERO INC NOP1 SENSE GPU ROT DIVIDE DIVIDE COPY COPY SENSE FIND JZ PUSH`) that made one GPU copy among three members before dying out — and that run made 3,466 GPU links in all. Like COPY replicators, GPU-copying lineages appear but don't yet persist.

*Superseded in part by 6.8:* the GPU now has 16 ops (copy moved from BX 1 to BX 4), and reductions such as compare are all-or-nothing.

### 6.8 A richer GPU and the in-world NPU (Phase 2b) [built 2026-11-05 from the Phase 2b plan, approved by Damian as recommended]

Asked for by Damian ("a richer instruction set to the gpu, and also add npu into the mix"), which un-defers the tensor/NPU unit of 5.1 D. The plan and its calls B1–B10 are in `docs/phase2b-plan.md`. Both units are virtual, implemented on the CPU (Section 3); the machine's real NPU is not used (B10). Code: `core/src/soup.cpp` (`UnitLedger`, one per unit kind), `core/src/interp.cpp` (the accelerator verb), `core/src/lineage.cpp` (`npu_user`).

**Off means unchanged.** `world.npus` defaults to 0, which leaves everything exactly as Phase 2; with `world.gpus = 0` too, everything is exactly Phase 1b. The baseline trials meet the gate at the same ticks as before either phase.

**One verb, two units (B3).** The GPU instruction (codon 61, name unchanged) is the accelerator verb: BX 0–15 runs a GPU op, BX 16–19 an NPU op, each through its own selector map. A missing unit, or any other BX, fails, costing 1. The setup cost is `gpu.setup` (4) for either.

**The GPU's 16 ops (B1, B2).** Elements run from the read head (source) to the write head (destination), private code and hot pages applying per element.

| BX | op | effect |
|---:|---|---|
| 0 | compare | CX ← source/destination pairs that differ (reduction) |
| 1 | count | CX ← source codons equal to AX mod 64 (reduction) |
| 2 | find | CX ← offset of the first source codon equal to AX mod 64, read head left on it; fails if absent (reduction) |
| 3 | sum | CX ← sum of the source codons (reduction) |
| 4 | copy | dest ← source, with copy errors at the GPU's heat |
| 5 | fill | dest ← AX mod 64 (reads nothing; the read head stays) |
| 6 | not | dest ← 63 − source |
| 7 | rcopy | dest ← source read backwards (the read head runs down, wrapping at its block's start): a reversed copy, with copy errors |
| 8–15 | add, sub, xor, and, or, min, max, mul | dest ← dest op source, mod 64 |

- *Reductions* are all-or-nothing: they need capacity for every element and every element readable, or they fail and change nothing. Heads advance over what they read; find is charged only up to the match.
- *Elementwise ops* do as many elements as this tick's capacity allows and leave the rest in CX, as in Phase 2.

**The NPU (B4–B6).** `world.npus` units per node, held exactly like GPU units: ALLOC and RELEASE with resource selector 4 (the resource map has 5 entries when there is an NPU), the DIVIDE share, ENGULF, death, `npu.wear`, per-unit heat (`heat.npu_gain` 9, `heat.npu.*` like the GPU's). Capacity is multiply-accumulates: `npu.macs` (256) per unit per tick, times its heat speed factor. CPU cost: `gpu.setup` + `npu.cpu_per_mac` (1/64) per MAC.
- Values: a codon v is the signed fixed-point number (v − 32)/32. Outputs are the sum of products divided by 32 (an arithmetic shift), saturated to −32…31 and re-centred.

| BX | op | effect | MACs |
|---:|---|---|---:|
| 16 | dot | CX ← Σ (source[i] − 32)(dest[i] − 32), signed; both heads advance | k |
| 17 | matvec | v (k codons) at the read head, M (k×k, row-major) at AX; dest[j] ← sat(Σᵢ M[j][i]·v[i] / 32); k ≤ 64 | k² |
| 18 | matmul | A at the read head, B at AX, both k×k; dest ← sat(A·B / 32); k ≤ 32 | k³ |
| 19 | argmax | CX ← offset of the largest source codon (the first of equals), read head left on it | k |

- k = CX. Every NPU op is all-or-nothing: capacity for every MAC, every input readable, every output writable and accepted, or it fails and changes nothing.

**Sensing and the shuffled control (B8).** SENSE has 14 readings with an NPU: 11 = my NPU units, 12 = their mean heat, 13 = free NPU units. Readings 8–10 still need a GPU and 11–13 an NPU, failing otherwise. The shuffled arm draws the SENSE map over the readings that exist, the resource map over 5 entries with an NPU, the 16-entry GPU map and the 4-entry NPU map, each redrawn until its first entry isn't left at 0.

**Detection and outputs (B7).** GPU writes mark codons as GPU-written; NPU outputs count as ordinary writes. The first NPU op by a member of a lineage that has met the gate is a replay `event … npu_user …` record. CSV columns `npu_macs`, `npu_released`, `npu_worn`, `npu_held`; `result.txt` has `npus` and `npu_user`.

**Harness and bench (B9).** Harness arms `npu` (`world.npus = 200`, `npu.wear = 0.001`) and `accel` (GPU and NPU). Bench workload `npu`: a hand-written 21-codon COPY replicator that takes a dot product of itself with its daughter each cycle (seeded only in tests and the bench) — 41 million CPU instructions a second on one thread, 622 million on 24.

**Findings (2026-11-05, short check: `baseline`, `gpu`, `npu`, `accel`, seeds 1–3, 2 minutes each; three seeds, so directional).**

| arm | gate met | first gate (ticks) | units held (of 400 GPU / 200 NPU) | GPU element-ops / NPU MACs per trial | milestones |
|---|---:|---|---|---|---|
| baseline | 3 / 3 | 5,983 – 166,695 (as before) | — | — | — |
| gpu | 3 / 3 | 54,645 – 82,549 | ~390 GPU | 4.7 – 9.1 million | `gpu_replicator` in all three |
| npu | 2 / 3 | 16,591 – 118,268 | ~180 NPU | 0.8 – 5.1 million MACs | none |
| accel | 3 / 3 | 111,721 – 160,658 | ~380 GPU, ~180 NPU | 5.7 – 13.4 million / 0.2 – 1.2 million | `npu_user` (seed 1), `gpu_replicator` (seed 2) |

1. *Random code grabs and uses both units:* nearly all GPU and NPU units are held, and random programs run millions of element-ops and MACs.
2. *The richer GPU still helps emergence, less than before:* about 1.3× the baseline rate, against about 4× with Phase 2's four ops — plausibly because copy moved from BX 1 to BX 4, which random code reaches less often (not tested). All three GPU seeds reached `gpu_replicator`, with up to 2 million inheritance links in a trial.
3. *The NPU doesn't help emergence:* about half the baseline rate. A plausible reading (not tested): random code spends CPU on NPU ops that do nothing for copying.
4. *A gated lineage used the NPU* (`npu_user`, accel seed 1), the first sign of NPU use inside a replicator lineage.

### 6.9 The world (Phase 3) [built 2026-11-05 from the Phase 3 plan, approved by Damian as recommended]

The plan and its calls C1–C10 are in `docs/phase3-plan.md`. Code: `core/src/graph.cpp` (generation and noise), `core/src/world.cpp` (per-node parameters, neighbour numbering, transit, the barrier), `core/src/interp.cpp` (DIVIDE to a neighbour, SENSE 14), `core/src/lineage.cpp` (lineages across nodes, `colonist`).

**Off means unchanged (C10).** `world.graph` defaults to `ring`, and `world.dispersal` defaults to on only in procedural worlds. A single-node world, and a ring world without dispersal, hash exactly as in Phase 2b: every pinned and gate hash is unchanged. Two visible changes reach every soup world: the CSV has the new columns below, and lineage ids print as `node.count` (`0.1` on a single node).

**Generation (C1; 4.1, 4.4a G).** With `world.graph = procedural`, nodes sit in a hidden unit square.
- *Placement:* a jittered grid of ceil(√N)² cells, N of them chosen at random, one node in the middle 80% of each; node ids follow the cells row by row.
- *Links:* each node to its `world.links` (4) nearest, ties to the lower id, made symmetric.
- *Ruggedness:* the ruggedness field at each link's midpoint, as a rank u among the proximity links (0 smoothest, 1 roughest). Links with u ≥ 1 − `world.ruggedness` (0.2) are cut. Isolated nodes are allowed.
- *Long links:* round(`world.long_links` (0.02) × N) extra links, each from a random node to the farthest of 8 random others. Never cut.
- *Noise:* hand-rolled 2D gradient noise (8 gradients), 4 octaves from frequency 3, one field per property seeded from (seed, field). Node properties use a field rank-normalised over the nodes, so each spans −1…1 evenly whatever the raw noise's spread.
- Generation is serial and a pure function of seed and config, so the graph is not snapshotted.

**Node properties (C2; 4.4a B).** `vary.soup_size`, `vary.millicores`, `vary.gpus`, `vary.npus`, `vary.cool`, `vary.spark_rate` and `vary.radiation` (each 0–1, default 0): node value = mean × (1 + f × noise), from that property's own field.
- Clamps: memory at least 64 codons; at least 1 millicore; at least 1 GPU (NPU) unit when the mean has any; cooling 0.0001–1. Radiation scales `hazard.copy`, `hazard.flaw` and `hazard.rot` together.
- Settings bounded by the soup size (search limit, contact range, spark lengths) are clamped per node. A seed that doesn't fit a node is skipped there. The default rot rate stays the mean's, per codon; it isn't recomputed from each node's size and millicores.
- Every node holds its own `SoupParams`. Live spark knobs reach every node: the rate times the node's factor, the lengths clamped to its size.

**Edges (C3; 4.4a F).** Latency is 1 + floor(distance × `edge.latency_scale` (20)) ticks. Bandwidth is max(1, floor(`edge.bandwidth` (32) × (1 − 0.75 u))) codons per tick, so rough ground is up to 4× slower. Capacity is `edge.capacity` (2) cargoes in transit at once per directed edge. A ring world with dispersal has latency 1, full bandwidth, and neighbours in id order.

**Dispersal (C4; 4.4a E).** DIVIDE's destination is BX through the selector map for BX 0–3, or neighbour BX directly for BX ≥ 4.
- Destination 0 is here, as before. Destination k is neighbour k, numbered 1… nearest first by hidden distance (ties: lower id). A number past the node's degree fails. Without dispersal anything but here fails, exactly as before.
- *At the origin:*
  - The detector sees the birth first (she joins the lineage), then her tag is taken.
  - Her codons are copied into cargo.
  - Her share, floor(holding × CX/64) of millicores, GPU and NPU units, goes back into the node's pools, oldest first, keeping its heat.
  - Her block is unlinked and freed; the waste keeps its contents. This counts as `sent`, not as a birth.
- *Edge full:* if the cargoes in flight on that edge plus this tick's departures on it reach its capacity, DIVIDE fails and nothing changes.
- *On arrival:*
  - A start position comes from the destination's stream, continuing its tick stream, in the fixed order. She takes the first waste run of her length from there, wrapping to the start once, as sparks do, and her codons are written into it.
  - She becomes a body with one CPU at her start, registers zero.
  - She draws her millicores and units from the destination's pools, capped by what's there, and none of a unit the node lacks.
  - She runs from the next tick. With no millicore she dies in that tick's sweep. With no room she is lost (`lost`).

**Transit (C5).** A cargo of L codons sent during tick t is due at t + ceil(L / bandwidth) + latency, and is placed at the barrier that ends that tick.
- At each barrier, every node's departures go onto their edges (node order, then the order they left, numbered with a world send sequence). Then every due cargo is delivered in the fixed order: destination id, then source id, then send order.
- Cargo is world state, hashed and snapshotted. The lineage tag it carries is detector data and is in neither: a resumed run's in-flight daughters arrive untagged, as detection starts afresh anyway.
- A snapshot of a dispersal world must carry exactly its config's edges.

**Origins and seeding (C6).** `world.origins = all` (default: sparks on every node) or `one` (only node 0 gets sparks, and the rest are lifeless random waste until colonised). `seed.node = all` (default) or k, for one seeded founder on node k.

**Sensing (C7).** With dispersal, SENSE has 15 readings: 14 = my node's degree. The shuffled control draws the SENSE map over the 15.

**Detection across nodes (C8).**
- *Ids:* lineage ids are world-unique, the founding node in the top 32 bits, written `n.k`.
- *Adoption:* a dispersing daughter's tag (lineage, genotype, her genotype's first parent, depth, gated) travels with her. On arrival she is adopted at the same depth. If the lineage has no living member there, it gets an entry on that node (founded = the arrival tick), carrying the gate state.
- *Gate:* the gate stays per node. A dispersing daughter counts as a member at the instant of her birth, as a daughter who dies at once does, so a lineage can meet its gate on the node she leaves.
- *`colonist`:* the first arrival of a member of a gated lineage on a node it wasn't founded on is a replay `event … colonist node=<arrival node> …` record, once per lineage per node. `Run::colonist()` keeps the first one.
- *Outputs:*
  - The lineage CSV has one row per lineage per node.
  - CSV columns `sent`, `arrived`, `lost`, `in_flight` and `colonised` (nodes holding a living member of a lineage founded elsewhere).
  - The per-node file `output.nodes` (on the stats window) has position, degree, properties, bodies, free memory and units, births, deaths, departures, arrivals, losses, lineages and colonists.
  - `result.txt` adds `graph`, `dispersal`, `colonisations` and the first colonist's tick, instructions, node, lineage and length.

**Running worlds (C9).** `configs/world.cfg`: 100 procedural nodes, every property varied by 0.5, many origins, 16,384 codons and 2,000 millicores a node on average, the Phase 1b rules, sparks scaled to a quarter of the abio node's memory, on all threads.
- *Harness:* `tools/abio.ps1` takes `-Threads` per trial; trials at once = Percent × processors ÷ Threads. Every trial writes `nodes.csv`, and `trials.csv` adds the colonist columns.
- *Bench:* a `world` workload, 100 procedural nodes of random soup: 73 million instructions a second on one thread (91 ticks/s), 519 million on 24 (543 ticks/s).

**Determinism (C10).** The gate adds a 24-node procedural world with every property varied, dispersal, and on every node a seeded replicator that disperses to its nearest neighbour: identical hashes and events at 1, 4 and 20 threads and under reversed and shuffled dispatch, with ~290 departures, ~280 arrivals and 14 colonist events in 300 ticks. The `dispersal` suite covers generation, properties, the exchange rule at both ends, capacity and numbering, SENSE 14, transit timing, colonists, detection on/off giving the same world, snapshot resume with cargo in flight, event replay and the outputs.

**Findings (2026-11-05, short runs of `configs/world.cfg` on the home rig).**
1. *Many origins (seed 1, 20 s, 8,531 ticks, 1.9 × 10¹⁰ instructions):*
   - Random code already disperses before any replicator exists: about 5,000–5,800 daughters per 1,000 ticks across the world, from random DIVIDEs whose BX names a neighbour.
   - The first replicator met the gate at tick 2,255, after 3.0 × 10⁹ instructions. That is inside the single-node range (6.6: 5,983–166,695 ticks at about 29,000 instructions a tick, so roughly 2 × 10⁸ – 5 × 10⁹).
   - It is an 8-codon COPY replicator (`ALLOC ROTB CALL INC IFNE DIVIDE SENSE COPY`) founded on node 33 and gated on node 32, so the first replicator was already a disperser. The first colonist followed 10 ticks later.
   - Within 750 ticks every connected node (96 of 100) held colonists, and dispersal rose about 25×, to roughly 150,000 daughters per 1,000 ticks, about 2% of births.
   - 127,000 lineages were founded (max depth 32), and the replay log holds 16,000 events, because the gate is per node, so a spreading replicator fires once per lineage per node.
2. *One origin with a seeded founder* (the 13-codon disperser on node 0, sparks only there; 60 s, 31,590 ticks):
   - The gate was met at tick 4 on node 10, and the first colonist arrived at tick 11.
   - Every connected node was colonised, and the 4 isolated nodes stayed lifeless.
   - By the end, 41,000 colonisations had happened, about 84,000 daughters left per 1,000 ticks against 9.1 million births at home (about 1%), and 1.5% of arrivals found no room.
3. *Dispersal strategies, as far as these data go:* copies mostly stay home. Births at home outnumber departures 50–100 to 1 in both runs. Even where the seed always disperses, what fills the world mostly divides at home; whether that is the seed's mutants or spark-born replicators from node 0 needs a genome-level analysis, which isn't done.

### 6.10 The individual features (Phase 4) [built 2026-11-05 from the Phase 4 plan, approved by Damian as recommended, revised by him during the build]

The plan and its calls C0–C23 are in `docs/phase4-plan.md` (revision 2). During the build Damian changed two things: a collapse leaves remnants instead of erasing code, and a new salvage rule limits how much of a bigger program's code a much smaller one can take (step 4 below). Code: `core/src/soup.cpp` (the disk, volatility, stamps, wear, collapse and ruins), `core/src/interp.cpp` (DISK, EMIT, LISTEN, the salvage rule, SENSE 15–17), `core/src/world.cpp` (corruption, link failures, signal propagation, collapse and recovery, graph rebuilds), `core/src/graph.cpp` (assembly among live sites, 3D noise), `core/src/lineage.cpp` (`disk_user`, `signaller`).

**Off means unchanged (C0, C20).** Every feature has its own switch, off by default. With all of them off every world hashes exactly as in Phase 3: all pinned and gate hashes are unchanged. Each feature's state is hashed and snapshotted only when it is on.

**1. The ambient leak (C1; 4.5, 6.4 #17).** `heat.ambient` (0 = off). Each tick a node's ambient input is `heat.ambient` × the mean CPU heat input over all its millicores, held or free. It is added to every CPU unit, memory page and GPU/NPU unit on the node. CSV `ambient` (the mean over nodes).

**2. Edges (C2–C3; 4.4a F).**
- *Corruption:* `edge.corruption`, the chance per codon per tick in transit of one bit flip, sampled at delivery over the cargo's exposure (codons × ticks) from a stream keyed by its send number. Long and rough links are riskier. CSV `corrupted`.
- *Link failures:* `edge.failure`, the chance per link per tick of going down (both directions), for an exponential downtime of mean `edge.downtime` ticks, decided at the barrier. While down, DIVIDE to it fails and it carries no signal; cargo on it when it fails is lost. CSV `transit_lost`, `links_down`.

**3. The disk and volatility (C4–C8; 5.1 A, B).**
- *Disk:* `world.disk` codons per node (0 = no disk; `vary.disk`), random and unowned at the start. A body's disk is its extents in claim order, addressed from offset 0. ALLOC disk (selector 3) claims one run from a random start (AX ← its offset); RELEASE gives up the newest codons. DIVIDE hands over the newest share, contents and all; dispersal releases it at home and claims the same amount on arrival; ENGULF takes all of it. Freed disk keeps its contents (fossils). `disk.rot` per codon per tick (default 10⁻⁶).
- *DISK:* BX through a 3-entry map (0 compare, 1 store, 2 load; the shuffled control draws it), AX = offset, CX = count, the memory side at the read head (compare, store) or write head (load), private code applying, all-or-nothing. Cost `disk.cost_base` (4) + `disk.cost_per_codon` (0.25) × CX. Transfers are exact; rot is the disk's hazard.
- *Volatility:* `mem.volatility`, the chance per page per tick that a wipe starts there, covering 1–`mem.wipe_pages` (4) whole pages, which become random codons; ownership stays. A body wiped whole this tick has its CPUs halted and dies in the sweep. `vary.volatility`.
- SENSE 15 = my disk, 16 = free disk. Event `disk_user`: a gated lineage's member loading into its own block. CSV `disk_stores`, `disk_loads`, `disk_compares`, `disk_rot`, `wipes`, `wiped`, `wipe_deaths`, `disk_held`.

**4. Remnants and salvage (C22–C23; added by Damian during the build).** `salvage.ratio` K (0 = off).
- *Stamps:* every memory and disk codon carries the complexity of the program whose code it was (0 = nobody's). Complexity is genome length, the program's first block (Damian had no preference; total size would include an eater's prey the moment it engulfs).
  - Stamped when code is left behind: at death, at RELEASE, when heat shuts a page, when a dispersing daughter leaves her block, when a disk share is freed. Unstamped codons take the program's genome length; stamped ones keep theirs. At ENGULF the prey's codons take its genome length.
  - Cleared by writes, wipes, and memory claims (ALLOC, sparks, arrivals: claiming memory is not salvage, Damian's choice). Claimed disk keeps its stamps, so loading fossils is salvage.
- *The rule:* a codon taken through a head (COPY, READ, INJECT's payload, GPU and NPU reads, DISK) with stamp C > K × the taker's genome length r comes through intact with chance K × r / C, otherwise as a random codon. Execution and template searches are not salvage. So a small program gets fragments of a big corpse or prey, and an engulfed big program can't copy itself faithfully inside a small eater. CSV `garbled`.

**5. Signals (C9–C11; 5.1 G).** `signal.channels` (0 = off).
- EMIT adds CX (signed) to channel BX's sum on the node this tick, costing `signal.cost_base` (2) + `signal.cost_per_amp` (1/32) × |CX| instructions (time and heat). LISTEN reads the channel's field on my node this tick into CX.
- At the barrier each node's sums go out along paths computed by a breadth-first search over the links that are up, to `signal.reach` (8) hops (a hard reach added while building). Each target gets its fewest hops and the quickest of those paths: amplitude × `signal.attenuation` (0.25)^hops arrives after 1 + the path's latencies (my own node: the next tick). Contributions below the target's `signal.floor` (1; `vary.noise`) are dropped, and every channel carries background noise uniform within the floor. Flooding along fewest-hop paths has no echoes.
- Pending signals are world state (a ring of slots longer than any path's delay). Each node computes its own field at the start of its update, so this runs in parallel.
- Event `signaller`: a gated lineage whose members emitted on `signal.sustained` (100) distinct ticks. `output.signals` is the researcher's tap: per window and channel, emissions, total |amplitude| and mean |field|. CSV `emits`, `listens`.
- The `nova_v1_inv2` control table has no EMIT/LISTEN (its codons 58–59 are the doubled ENGULF/INJECT).

**6. The node lifecycle (C12–C18; 4.4a A, C, D).** Needs `world.graph = procedural`.
- *Wear:* `node.endurance` codon writes (memory writes and disk stores; `vary.endurance`). Hazards (copy, flaw, rot, volatility) × 1 + `node.decline` (4) × wear². Worn out (wear ≥ 1): collapse.
- *Starvation:* a meter that ebbs like heat (*Damian, after the short check*). Each tick it loses `node.starve_ebb` (0.00025) of itself and gains 1 if the free CPU pool is below `node.starve_below` × millicores; reaching `node.starve_ticks` collapses the node. Constantly starving it settles at 1 / ebb, so it fills only if the node starves more than starve_ticks × ebb of the time (half, at the defaults). A brief recovery dents it instead of emptying it.
- *Health:* SENSE 17 = 100 × the lower of 1 − wear and 1 − starvation meter / starve_ticks.
- *Collapse,* at the barrier in node order: every body dies an ordinary death, so its code stays behind as remnants and its disk as fossils (*revised by Damian*: 4.4a C had the code lost). Cargo to or from it is lost; the node is fallow (no tick, no edges); event `collapse`.
- *Creation and recovery, one mechanism:* the generator's positions are sites. `world.land` (1 = off) is the fraction that are land at once: the top of a land field, drifting through a 3D noise at `world.drift`. Every `node.recover_every` ticks, in site order, a fallow site that has been fallow `node.fallow_min` ticks and is land now becomes a node while the live count is under `world.budget` (default: the live count at the start). It takes its properties from the fields as they are now (drifting too), and its ruins: memory and disk with their stamps, weathered by rot over the fallow time (*Damian's choice*), the overlap kept if its size changed. A fresh pool, initial sparks, an empty detector; event `recovered`.
- *Rebuilds:* the graph among live sites keeps the generation's cut threshold and long pairs, so with every site live it is exactly Phase 3's graph. Cargo and downtimes on surviving links carry across; neighbour numbering, ports and signal paths are recomputed.
- CSV `collapses`, `recoveries`, `live_nodes`; the per-node file adds `live`, `health`, `life`, `born`; `result.txt` adds `collapses`, `recoveries`, `disk_user`, `signaller`.

**Running (C21).** Harness world arms `leak`, `transit`, `disk`, `salvage`, `signals`, `lifecycle` and `phase4` (all), at the plan's suggested values; `configs/world.cfg` lists the keys, off. Bench workload `world4`: the world with all of them on.

**Determinism.** The gate adds a 24-node procedural world with every feature on: identical hashes and events at 1, 4 and 20 threads and under reversed and shuffled dispatch, with every feature firing within 300 ticks. Suites `leak`, `edges`, `disk`, `salvage`, `signals` and `lifecycle` cover each feature, including snapshot resume through link failures, stamps, signals in flight, and collapses and recoveries.

**Findings (2026-11-05, home rig).**
1. *Short check:* each arm against the baseline on `configs/world.cfg`, seed 1, 2 minutes, 8 threads each. One seed per arm, so the first-gate ticks say nothing about emergence rates; what each feature did is the point.

   | arm | first gate (tick) | what the feature did |
   |---|---:|---|
   | baseline | 2,255 | — |
   | leak | 1,948 | ambient input about 0.27 heat a tick: weak, as decided |
   | transit | 404 | 11,852 codons flipped in transit, 718 cargoes lost, 17 links down at the end |
   | disk | 1,640 | 1.39 million DISK ops by random code; 4,443 wipes killed 22,499 bodies; `disk_user` reached |
   | salvage | 1,637 | 255,300 codons garbled |
   | signals | 5,055 | 144 million EMITs and 124 million LISTENs; `signaller` reached |
   | lifecycle | 1,766 | no collapse in 29,879 ticks (see 2) |
   | phase4 (all) | 1,468 | 780,569 garbled; `disk_user` and `signaller`; no collapse |

   - Random code reaches every new verb: the disk and the signal channel are in heavy use well before any replicator exists, as 6.2's "mutation-reachable" asks.
   - A gated lineage restored its own code from disk (`disk_user`), the first sign of repair inside a replicator lineage.
2. *The lifecycle at the suggested values doesn't fire in minutes:*
   - Wear: health was 95–99 after 30,000 ticks, about 1,000 codon writes per node per tick, so endurance 10⁹ wears a node out after roughly 10⁶ ticks (about an hour of world time). That suits 12-hour runs.
   - Starvation: 70 of 80 nodes sat below the 0.2% threshold at the end, but CPU wear frees a few units nearly every tick, so the consecutive count rarely built. *Damian then made the count a meter that ebbs like heat.* Rerun at the same settings (endurance off, 2 minutes, 83,698 ticks): 1,092 collapses, the first at tick 3,975, and the live count fell from 80 to 43–68 as collapses outpaced recovery. A busy node starves more than half the time, so at these values starvation now dominates; `node.starve_below`, `node.starve_ticks` and `node.starve_ebb` tune it.
3. *The lifecycle at endurance 3 × 10⁷* (one 2-minute run, 59,598 ticks, salvage on): 106 collapses and 105 recoveries from tick 17,058, the live count held at its budget of 80. Returning nodes were recolonised (10,689 colonist events after returns) and grew gated lineages again (24,747 replicator events). The data don't yet separate colonists from local abiogenesis on the ruins.
4. *The `signaller` milestone is easy to meet:* a replicator with an EMIT anywhere in its genome emits on 100 ticks quickly. Before it means communication, "sustained signal use" may need a stricter reading (for example emitters and listeners in one lineage, or a far higher count).

### 6.11 The real GPU and NPU (Phase 5) [built 2026-11-06 from the Phase 5 plan, approved by Damian as recommended]

The plan and its calls C1–C13 are in `docs/phase5-plan.md`. Damian asked for it ("I want the resource landscape to use everything I have got"), chose Windows' own Direct3D 12, DXCore and DirectML (no CUDA), and asked for weights suited to high-volume devices. Code: `core/src/accel_dx.cpp` (discovery, the probes, the GPU and NPU backends; the only file that includes the Direct3D headers), `core/src/accel_ref.cpp` (the CPU reference), `core/src/interp.cpp` (issue), `core/src/soup.cpp` (gather, completion, result slots), `core/src/world.cpp` (the ops in flight, the devices, fallback, verification), `app/bench.cpp` (`nova bench devices`), `app/main.cpp` (`nova devices`).

**Off means unchanged (C9).** `accel.mode = instant` (the default) is exactly Phase 4: every pinned and gate hash is unchanged, and nothing opens a device.

**1. Asynchronous ops (C2; `accel.mode = async`).**
- *Issue*, when the verb runs: the checks are as in 6.7 and 6.8, the op is sized to this tick's capacity, the CPU cost is charged and the heads advance as if it were done. The op is queued with its read and write addresses. Nothing is read or written yet. Elementwise ops that hit a refused codon stop there (CX = what's left); reductions and NPU ops are all-or-nothing.
- *Gather*, at the barrier ending that tick: every live node's queued ops, in node order then issue order, read their inputs from memory as it is then, with the salvage rule applied from a stream keyed by the op.
- *Complete*, at the barrier ending tick t + `accel.latency` − 1 (default 2; 1 is the same barrier): outputs are written in node then issue order, only where the issuing body (same serial) still owns the codon. Hot pages refuse writes as usual, and copies make copy errors from a stream keyed by the op. GPU-written marks are set, so an async GPU copy still makes a GPU link.
- *Results that were registers* (compare, count, find, sum, dot, argmax) go to the issuing CPU's result slot, dropped if that CPU has died since. SENSE 18 reads the slot, SENSE 19 my ops still pending (both fail in instant mode).
- Ops in flight, their inputs and addresses, and the result slots are world state: hashed and snapshotted.

**2. The high-volume profile (C3; Damian).** In async mode the accelerators default to expensive per op, nearly free per element; each key is still settable.

| | instant (6.7, 6.8) | async default |
|---|---:|---:|
| `gpu.setup` (instructions per op) | 4 | 32 |
| `gpu.cpu_per_element` | 1/16 | 1/1024 |
| `gpu.elements` (per unit per tick) | 32 | 4096 |
| `npu.cpu_per_mac` | 1/64 | 1/16384 |
| `npu.macs` (per unit per tick) | 256 | 65536 |
| NPU size limits (matvec k / matmul k) | 64 / 32 | 256 / 64 |

**3. The devices (C1, C4, C5).**
- `accel.gpu = auto | cpu | <part of a name>` (auto: the hardware GPU with the most dedicated memory, the RTX 5080 here). `accel.npu = auto | cpu | gpu` (auto: the NPU if its self-test passes, else the GPU).
- *The GPU:* one HLSL compute shader (Shader Model 5.1, compiled at start-up by `d3dcompiler_47`) does all 20 op kinds, one thread per element (or per matvec row, or matmul output). Reductions use integer atomics, which are order-independent and so exact; argmax packs (value, reversed index) so an atomic max finds the first maximum. Each barrier's batch is one upload, one dispatch and one readback, and up to 8 batches can be in flight.
- *The NPU* (Intel AI Boost) through DirectML. Its only exact form is a float16 multiply compiled as a graph with every sum below 2048 (finding 1 below), so values u = codon (0…63, the signed value is u − 32) are split into base-8 digits, sums are cut into chunks of 32, and each chunk needs four digit products (each at most 32 × 49 = 1,568). The CPU puts them back together exactly. One shape does all the work: an item is [128 × 32] × [32 × 128], whose rows hold up to 64 vectors' high and low digits and whose columns likewise. A matmul (k ≤ 64) is one item per chunk of the inner index; a matvec (k ≤ 256) one per (64 rows, chunk); a dot one per 64 chunks, read off the diagonal. argmax has no multiplies and runs on the CPU.
- *What the NPU driver allowed (found while building):* it compiles batched multiplies only with the batch in the second dimension, compile time grows steeply with the batch (32 items 74 ms, 1,024 items 6 s), and executing a batched multiply removes the device. So each item is its own dispatch. Recording and closing a dispatch costs the driver about 1 ms of CPU, so each slot's dispatch lists (16 items each) are recorded once and replayed, and only a small copy-in and copy-out list is recorded per batch. A batch takes up to 256 items; ops past that run on the CPU.
- *The NPU's self-test:* at start-up it runs known-answer batches of every NPU kind through the real path, from 1 item to all 256 and past them, with all-0 and all-63 extremes. Any refusal or wrong answer sends NPU ops to the GPU with a note.

**4. Determinism and fallback (C7, C8).**
- Everything the devices do is integer arithmetic on inputs read at a barrier, written back in a fixed order at a barrier, so the hash is the same on the GPU, the NPU or the CPU reference, at any thread count. Which device ran an op is never hashed or saved: ops in flight when a snapshot was taken finish on the CPU reference after a resume.
- A device that won't start, or fails mid-run, leaves its ops to the GPU or the CPU reference, with a note in the run's output. The results are the same.
- `accel.verify = N` checks every Nth device batch against the CPU reference. A difference uses the reference's answer, is reported, and stops the run (`stop = accel_verify`).

**Running (C11, C12).** The run prints where the ops go (`nova: accelerators async, latency 2: GPU ops on NVIDIA GeForce RTX 5080, NPU ops on Intel(R) AI Boost`). `result.txt` adds `accel_mode`, `accel_latency`, `accel_gpu`, `accel_npu`, `accel_device_ops`, `accel_npu_ops`, `accel_reference_ops`, `accel_verified` and `accel_mismatches`. `configs/world.cfg` lists the keys, off. Harness arms `gpu_hw`, `npu_hw` and `accel_hw` are the `gpu`, `npu` and `accel` arms in async mode. `nova devices` reports what each adapter accepts; `nova bench devices` times the devices against the CPU reference and suggests capacities; bench workload `worldacc` is the `world` workload with async accelerators on the devices.

**Determinism.** The gate adds a 24-node async GPU + NPU world: identical hashes and events at 1, 4 and 20 threads and under reversed and shuffled dispatch on the CPU reference, and the same world on the devices at 20 threads matching it tick for tick. Suite `async` covers issue, timing, result slots, ownership, every op against its instant twin, and snapshot resume with ops in flight. Suite `devices` checks the GPU on 1,761 random ops (and 3 batches in flight at once) and the NPU on random batches against the reference, and whole async worlds on the CPU, on the GPU and NPU, on the GPU alone and with every batch verified, all with the same hash. It skips what the machine lacks.

**Findings (2026-11-06, home rig).**
1. *What the adapters accept (`nova devices`):* the RTX 5080 and the Intel integrated GPU run the compute shader and DirectML's int8 → int32, float32 and small-sum float16 multiplies exactly. The NPU refuses single operators and the integer and float32 forms. It runs float16 multiplies compiled as graphs, exactly while sums stay below 2048, once each graph's initialization has its own submission (without that every result was NaN).
2. *Device speed (`nova bench devices`, Release):*

   | where | batch | per batch | throughput |
   |---|---|---:|---:|
   | CPU reference, 1 thread | 64 GPU ops × 4,096 | 0.05 ms | 5.4 G elements/s |
   | RTX 5080 | the same | 0.26 ms | 1.0 G elements/s |
   | CPU reference, 1 thread | NPU mix (8.7 M multiply-adds) | 4.7 ms | 1.9 G MACs/s |
   | RTX 5080 | the same | 0.42 ms | 20.9 G MACs/s |
   | Intel AI Boost NPU | the same | 16.3 ms | 0.54 G MACs/s |

   One op's round trip: 0.08 ms on the GPU, 3.4 ms on the NPU; the NPU's start-up and self-test take about 0.5 s. The GPU is 11× the CPU on multiply-heavy work but loses on small elementwise batches, where the transfer dominates (codons travel as 32-bit values). The NPU is exact but slow at this work: the digit form multiplies the work by four, and every item is a dispatch.
3. *A real world (`configs/world.cfg`, 100 nodes, 400 GPU and 200 NPU units a node, async, 3,000 ticks, 24 threads):* random code issued 1.6 million device ops (7,663 on the NPU), every one verified against the reference with no difference. The hash was identical on the CPU reference, the GPU and NPU, and the GPU alone. Wall time: CPU reference 6.3 s, GPU alone 6.4 s, GPU + NPU 9.1 s. A tick here is about 2 ms, shorter than the NPU's round trip, so at latency 2 the barrier waits for the NPU. At latency 3 (a different world, ticks about 3.6 ms) GPU + NPU cost 16% instead of 45%. So far the devices are correct and in use, but they don't yet speed the simulation up: random code's ops are small, and the CPU reference does them in microseconds.
4. *Short paired check* (`tools\abio.ps1`, `configs/world.cfg`, arms `accel` and `accel_hw`, seeds 1 and 2, 20,000 ticks, 8 threads each; two seeds, so the ticks are indications only):

   | arm | first replicator (tick) | `gpu_replicator` (tick) | GPU links | `npu_user` | device ops (on the NPU) |
   |---|---:|---:|---:|---|---:|
   | accel (instant) | 2,186 / 729 | 7,418 / 3,827 | 584 / 592 | yes / yes | — |
   | accel_hw (async) | 4,966 / 11,824 | 14,820 / none | 1 / 9 | yes / yes | 13.3 M (85,626) / 9.9 M (101,227) |

   - Random code takes to the asynchronous accelerators at once: about 10 million device ops per trial, 1% of them on the NPU, and an NPU-using replicator lineage in both seeds.
   - Copying with them is rarer: a few GPU links against about 590, and a GPU-copying lineage in one seed of two, late. With the high-volume profile a small op is mostly setup and its result arrives a tick or more later, so the simple copy loops that stumble onto the instant GPU don't pay. Damian expected this: slow at first, more efficient as selection finds uses. Whether that happens is a question for the long runs.
   - The async trials ran about 30% fewer instructions in the same time (the NPU wait, see 3).

### 6.12 The analysis UI (Phase 6) [built 2026-11-06 from the Phase 6 plan, approved by Damian as recommended and unparked by him]

The plan and its calls C1–C12 are in `docs/phase6-plan.md`. Code: `core/src/view.cpp` (the view frame, the live session, a finished run's files, the phylogeny layout, colours, the chart log), `core/src/png.cpp` (the PNG writer), `app/canvas.cpp` (GDI drawing into an off-screen bitmap), `app/viewer.cpp` (the panels, immediate mode), `app/view_cmd.cpp` (`nova view`: the window, the sim thread, headless rendering). The Win32 parts link only `user32` and `gdi32`, Windows' own libraries; nothing was added to the include allowlist.

**One window, `nova view` (C1–C3).**
- `nova view <config>` runs the world live and shows it, writing the outputs the config names, so a watched run is a real run with a replay log (`--no-output` turns them all off, `--set output.x=` one). `nova view <snapshot>` continues a snapshot live with its outputs off. `nova view --run <dir>` browses a finished run's files without simulating. `--render out.png [--size WxH] [--color NAME]` draws one frame into a PNG instead of opening a window, after `--ticks N` ticks of a live run.
- *Threads (C2):* a sim thread owns the `Run` and steps it (its pool does the nodes). About 30 times a second, after a step, it builds a *view frame* if the window has asked for one, then carries on; while paused it builds one when the selection changes or a command arrives, and 4 a second otherwise. Run controls and knob changes go into a queue that the sim thread applies between ticks, knobs through `Run::schedule_knob` at the current tick, so they are logged and replay.
- *Run controls (C3):* pause and resume (also Space), single step, a speed limit (1 to 2,000 ticks a second, or flat out), snapshot now (`<output.snapshot>.<tick>`, or `nova_view.snapshot.<tick>`), and stop, which finishes every output as `nova run` does (`stop = view_stop` in `result.txt`; closing the window gives `view_close`). `run.ticks`, `run.seconds` and `run.after_replicator` pause the run rather than end it. The title bar and toolbar show the tick, ticks and instructions a second, the first replicator's tick and, when paused, the world hash.
- Per-monitor DPI aware; Segoe UI for labels, Consolas for genomes; a dark theme. The panels are resized by dragging the borders between them.

**The panels (C4–C8).**
- *Map:* nodes at their hidden coordinates (a ring world on a circle), edges between them, long links in violet, failed links dashed red, fallow sites hollow, cargo in flight as dots moving along their edges (interpolated by send and due tick), and a fading ring where a node just collapsed (red) or came back (green). Colour by bodies, births, colonists, health, the largest lineage (territories, a stable colour per lineage id), free CPU, waste, memory, millicores, climate, sparks, radiation, disk, volatility or signal activity; the range runs from the lowest node to the 95th percentile, so one crowded node doesn't flatten everyone else. Size by bodies. Hover for a node's numbers, click to select, wheel to zoom about the cursor, drag to pan, double-click to reset. Nodes holding the selected genotype get a blue ring.
- *Charts:* four stacked charts, each with up to four series picked from any of the 62 `stats.csv` columns (the same row builder writes both; summed columns are shown per tick). Auto-scaled, a hover read-out, the first event of each milestone (R replicator, G gpu_replicator, N npu_user, C colonist, D disk_user, S signaller) as a marked line, and collapses and recoveries as ticks along the bottom. Samples every 100 ticks (`--sample N`); after 4,096 points every other one is dropped, so the whole history stays at a coarser step.
- *Inspector:* the selected node's properties, health, life, lineages and neighbours (clickable), its memory as a strip of one pixel per codon coloured by owner, lineage, page heat, salvage stamp or codon, and its bodies. A click on a body (in the list or the strip) shows its genome as op names, its CPUs (registers, heads, stack pointer, errors, state, instructions executed) and holdings, and its lineage, depth and genotype.
- *Genome browser:* every genotype the genebank saved, with name, length, lineage, tick, node and parent; sortable by any column, filterable to the selected lineage or by a length, a length range (`20-40`) or a lineage (`38.1`). Selecting one shows its genome, rings the nodes holding living copies and draws its lineage's phylogeny. The run keeps its saves in memory for the viewer (`Run::keep_genebank`) and hands them over at each frame.
- *Phylogeny:* the selected lineage's saved genotypes as a tree by parent link, time across and one row per genotype depth first; a click selects that genotype. A genotype saved on several nodes appears once; a parent that wasn't saved hangs its children from the root.
- At start-up the viewer picks the busiest node, the lineage with the most saved genotypes, a member body and that lineage's latest genotype, so no panel starts empty.

**Finished runs (C9).** `output.snapshot_every = N` (new; needs `output.snapshot`) also writes `<output.snapshot>.<tick>` every N ticks, in `nova run` and `nova view` alike; a snapshot that can't be written stops `nova run` (`stop = snapshot_error`) and pauses the viewer. `nova view --run <dir>` reads `stats.csv` into the charts, `nodes.csv` into the map with a slider over its windows, `replay.log` for the events and the graph (a world built from its config, only for its edges and positions), and `genebank.txt` into the browser and the phylogeny. The memory strip and bodies need a live run or a snapshot.

**More live knobs (C12).** `cpu.wear`, `heat.ambient` and, in a dispersal world, `edge.corruption` are live, beside `spark.rate`, `spark.grant`, `spark.min_len` and `spark.max_len`: one number for every node, nothing to allocate. Each is logged, replayed and carried in snapshots like the rest. The toolbar's second row shows them with a field to type a value (Enter applies) and − / + buttons (halving and doubling rates, ±25% for lengths and grants).

**The testable core (C10, C11).** The panels draw only from view frames, built by plain functions with no Win32 in them (`ViewSession` for a live run, `RunArchive` for files). The viewer itself takes made-up input in the tests. The PNG writer is hand-rolled: 8-bit RGB, stored deflate blocks, CRC-32 and Adler-32, no zlib.

**Determinism.** Watching reads the world between ticks and changes nothing: suite `view` steps a 24-node world with every Phase 4 feature, builds a frame after every tick with a changing selection, applies five knobs from a pretend UI at the current tick, and gets exactly the hash of the same run with the knobs scheduled up front, at 20 threads against 4; its replay log replays. Suites `view` (PNG bytes and checksums, colours, chart sampling, the phylogeny layout, frames against the world, knobs, numbered snapshots, the genebank log, a finished run's files) and `viewer` (clicks select nodes and become commands, typing sets a knob, the wheel zooms about the cursor, every colour mode draws, the browser's filter, the log cap, a render to PNG) cover the rest. Every pinned and gate hash is unchanged.

**Findings (2026-11-06, home rig).**
1. *It runs:* `nova view configs/world.cfg` in a window ran 1,015 ticks in 8 s on the Debug build, closed cleanly with every output finished, and its replay log replayed exactly. Watching costs little: in 10 s of Release, `nova run` reached tick 5,927 and the window 5,142 (13% fewer, the window's start-up included). A Release render of `world.cfg` at 20,000 ticks (49 s) shows about 26,000 living lineages as territories and a 20-genotype phylogeny; one with every Phase 4 feature at 40,000 ticks shows 20 fallow sites, collapse and recovery rings and failed links.
2. *The genebank grows fast:* at `detect.genebank_threshold = 2`, `world.cfg` saved 170,738 genotypes in its first 20,000 ticks, about 10 a tick, mostly 7- and 8-codon replicators. A 12-hour run would save tens of millions, and `genebank.txt` would grow to gigabytes. So the viewer keeps the newest 200,000 saves (and events), and reading a finished run's `genebank.txt` keeps its newest 200,000. The threshold is one of the settings to tune before the long runs (Section 10).

### 6.13 The landscape (Phase 7a) [built 2026-11-06 from plan revision 2, approved by Damian]

The plan and its calls C1–C12 are in `docs/phase7-plan.md`. Damian replaced revision 1's parallel worlds with one world generated like a game level, whose landmasses grow their own ecologies; this section is its first step, 7a. Code: `core/src/landscape.cpp` (land, nodes, the triangulation, paths, bridges, distributions), `core/src/world.cpp` (the mode, ports, bridge schedules, the lifecycle on fixed paths, each site's shares), `core/src/view.cpp` and `app/viewer.cpp` (the land on the map), `app/main.cpp` (`nova land`, `nova run --profile`).

**A new mode, `world.graph = landscape`.** `ring` and `procedural` are untouched: every existing config, test and pinned hash is unchanged.

**1. The land (C1).** The world is `land.aspect` × 1. A noise field, warped by two more (`land.warp`) so coastlines wander, sinks toward the borders (`land.border`) and is cut at `land.fraction` on a raster `land.resolution` cells high: land and water. `land.scale` sets the features across the height. A flood fill labels each landmass.

**2. Nodes (C2).** `world.nodes` points are thrown at random onto land, none closer than a spacing that starts where they should fit and shrinks 15% at a time until they all do. A spacing below a quarter of a raster cell is refused ("lower world.nodes, or raise land.fraction or land.resolution"). Nodes are numbered row by row. The only spatial lookups happen here, once.

**3. Paths, generated once (C3).** A Delaunay triangulation (hand-rolled Bowyer–Watson with a walking point location) gives each node its natural neighbours. Then:
- paths over water are dropped;
- each node draws how many it wants: `paths.mean` and `paths.spread` (a lognormal), hermits (`paths.isolated`) none, hubs (`paths.hubs`) `paths.max`;
- the shortest first, a path is kept while both ends still want one;
- hubs then add roads to their neighbours' neighbours on their landmass;
- `paths.long` adds long links within a landmass;
- `paths.connected = true` keeps each landmass in one piece: a spanning tree over its land paths, then the shortest crossings of its own bays, then the nearest node for any piece still apart. Hermits then get the tree's paths.

Latency comes from length (`edge.latency_scale`) and bandwidth from a ruggedness field (`edge.bandwidth`), as in Phase 3. The paths are kept as flat arrays and never searched or rebuilt while the world runs.

**4. Bridges (C4).** Triangulation edges between two landmasses, no longer than `bridge.max_length`, are the candidate crossings. The shortest become bridges, up to `bridge.count` per pair of landmasses, without sharing an end; `bridge.isolated` of the landmasses get none, and hermits never get one. Each bridge opens for `bridge.open_ticks` of every `bridge.open_every` ticks (`bridge.phase`: together, staggered or random), with its own `bridge.latency_scale` and `bridge.bandwidth`.
- A shut bridge refuses departures; cargo already on it arrives. (A failed link still drops what it carries.)
- Bridges are numbered after a node's land paths, so they never shift a land neighbour's number.
- Signals don't cross bridges.

**5. The lifecycle on fixed paths (C5).** A collapse takes the node's paths down, dropping what they carry, until the site recovers; nothing is rebuilt and neighbour numbers never change. A site keeps its place, paths and resources for every life. The land doesn't drift.

**6. Scarce, abundant, barren (C6).** Sparks and every resource (`spark.*`, `res.memory.*`, `res.cpu.*`, `res.gpu.*`, `res.npu.*`, `res.disk.*`) share one model:
- each node's weight is lognormal (`spread`; 0 even), mixed with a noise field by `smooth` (0 independent, 1 rich and poor regions);
- `hotspots` clusters boosted by `hotspot_boost`;
- the lowest `barren` fraction get none: memory keeps a 64-codon minimum soup, CPU one millicore, and GPU, NPU and disk can be zero;
- totals are the configured means times the nodes (`world.soup_size`, `world.millicores`, ...), split into whole shares that add up exactly. A node's initial sparks scale with its spark weight.

In a landscape the resource and spark `vary.*` keys are replaced by these (setting one is an unused-key error); climate, radiation, volatility, endurance and noise still vary.

**7. The map (C10).** Land and water are drawn behind the nodes; the map fits the land's own box; landmasses are labelled where there's room; bridges are amber, solid when open and dashed when shut. Zoomed out over many nodes, nodes become dots and short paths are hidden; node size follows their spacing. An overview map in the corner shows the whole land and the view's frame, and a click or drag there moves the view. Double-clicking a node fits its landmass.

**8. Profiling and output (C11).**
- `nova run --profile` splits each tick into the parallel update and each barrier step.
- `nova land <config>` prints what a landscape's settings generate (landmasses, paths per node, hermits, hubs, bridges) in about 0.1 s, to tune them without running.
- `stats.nodes_every = N` writes the per-node rows every Nth window, each covering every tick since the last.
- `nova run` and `nova view` drop the run's events once printed or shown (the replay log keeps them).
- Presets: `configs/landscape.cfg` (continents), `configs/archipelago.cfg` (many islands, rare random bridges, a few isolated) and `configs/maze.cfg` (one winding land, sparse paths, spark hotspots), all at 1,000 nodes. Bench workload `landscape`.

**Determinism.** Generation is a pure function of the seed and the settings. Suite `landscape` checks:
- the triangulation (every triangle counter-clockwise with an empty circumcircle, against brute force);
- land, node spacing and the refusal when nodes don't fit;
- paths: none over water, hermits without paths, the degree spread, `paths.connected` giving one component per landmass;
- distributions adding up exactly, barren nodes, determinism;
- a 90-node landscape with every Phase 4 feature, cycling bridges and barren GPU and NPU nodes: the same hash at 1, 4 and 20 threads and under shuffled dispatch, an exact snapshot resume, bridges shut on schedule with no departure while shut (and cargo crossing them), nothing on a fallow site's paths, the paths unchanged through collapses, and every soup's books in order;
- `stats.nodes_every`.

Suite `viewer` adds a landscape: the land sent once, the overview map moving the view, and double-click fitting a landmass.

**Findings (2026-11-06, home rig).**
1. *Generation is quick:* 1,000 nodes in 0.1 s, 10,000 in 0.85 s.
2. *The presets (seed 1, `nova land`):*
   - continents (`landscape.cfg`): 16 landmasses with nodes (672, 152, 61, 61, 33 ...), 27 bridges, 32 nodes with no path;
   - `archipelago.cfg`: 60 (290, 248, 157 ...), 56 bridges;
   - `maze.cfg`: one landmass where 81% of nodes have one or two paths.

   Noise land sunk toward the borders tends to join into one continent unless the land fraction is about 0.3 or less; `nova land` shows that in a tenth of a second.
3. *Speed* (`nova run --profile`, Release, 24 threads, outputs off):

   | nodes | ms per tick | parallel update | the barrier's largest step | peak memory |
   |---:|---:|---:|---|---:|
   | 1,000 | 11.7 | 99.2% | delivery 0.4% | about 0.3 GB |
   | 10,000 | 71.4 | 98.0% | delivery 1.0% | 2.45 GB |

   The barrier isn't worth parallelising yet (C11: profile-driven), so nothing was. At 10,000 nodes replicators and colonists appear within 300 ticks.

**Real resources (Phase 7b, C7–C9) [built 2026-11-06].** Code: `core/src/budget.cpp` (budgets, the calibration, the fit checks), `core/src/mapped.cpp` (`Backed<T>`, memory-mapped files), the disk tier in `core/src/soup.cpp`, the threads and priority in `app/main.cpp`.

- **Budgets become ordinary settings (C7).** Before the world is built, `Run::init` resolves:
  - `budget.memory` (a percentage of the RAM available at start, or an amount such as `24GB`) into `world.soup_size`, at `kRamPerCodon` = 20 bytes a codon;
  - `budget.disk` (a percentage of the free space where `disk.path` points, or an amount) into `world.disk`, at about 5 bytes a codon of file, 9 with salvage stamps;
  - `budget.gpu` and `budget.npu` (async only) into `gpu.elements` and `npu.macs`. A third of a second measures the device, a quarter of a second the interpreter, and the world is sized so a fully busy world keeps the device about that busy at the tick rate the CPU allows.

  Each resolved budget becomes `off`, its request is kept as `budget.<x>_was`, and the setting it made replaces any value it had. So the config a snapshot carries and the replay log rebuild exactly the same world, on any machine. `nova run` prints what each budget became.
- **The disk tier on real disk (C8).** `disk.path` puts each node's disk arrays (contents, owners, the used bitmap, stamps) in files in a folder of the run's own under that path; they are memory-mapped and deleted when the run ends. Only the storage changes: the same world gives the same hash with its disk in RAM or in files, tick for tick, through collapses and snapshots (suite `budget`). Snapshots still carry the disk's contents.
  - *A change from the plan:* C8 planned per-extent ownership so a large disk wouldn't need per-codon arrays in RAM. With the arrays in files they no longer cost RAM, and keeping them per codon kept every rule, and every existing hash, as it was.
- **Not swamping the machine (C9).**
  - A world whose RAM (at 20 bytes a codon) would pass 90% of what's available, or whose disk files would pass 95% of the volume's free space, is refused with what it would need.
  - `budget.threads` (default `all_but_one`; or `all`, a count, a percentage) sets the threads when `run.threads` and `--threads` don't.
  - `budget.priority` (default `below_normal`; `normal`, `idle`) sets the process priority. The harness passes its `-Priority` this way.
  - The run's genebank log and events are handed off (Phase 6 and 7a).

**Findings (Phase 7b, 2026-11-06, home rig).**
1. *The disk in files costs nothing measurable:* `configs/landscape.cfg` with a 4,096-codon disk a node ran 500 ticks at 9.9 ms a tick with the disk in RAM and the same in files on E:, with the same hash, leaving no files behind.
2. *A big budget:* `budget.memory = 8GB` and `budget.disk = 20GB` on E: made soups of 429,000 codons and disks of 4.2 million codons a node.
   - Private memory peaked at 2.5 GB, a third of the budget: 20 bytes a codon is a ceiling a world grows into as its populations fill, not where it starts.
   - The 20 GB of disk pages count in the working set (22.5 GB). They are file-backed, so Windows writes them out when it needs the RAM.
   - Start-up wrote every disk codon's random contents and took about 26 s.
   - Ticks took 189 ms against 11.7 at 16,384 codons: the per-page heat and rot passes grow with the soup, so a big memory budget buys room, not speed.
3. *Calibration (Release):* the RTX 5080 did 7.5 × 10⁸ element-ops a second and the NPU 8.2 × 10⁸ multiply-adds on the calibration batches. For the 24-node test world at its CPU-bound rate (about 13,000 ticks a second) that sized `gpu.elements` at 5 and `npu.macs` at 3 for 50% and 25%.

### 6.14 Programs on the GPU (Phase 8) [built 2026-11-06 from plan revision 2, approved by Damian]

The plan and its calls C1–C14 are in `docs/phase8-plan.md`. Damian's direction: GPU-rich, CPU-poor nodes should be able to hold life, on a GPU that *differs* from the CPU (serial work on the CPU, parallel on the GPU), with births allowed on either, on the real GPU from the start. Code:
- `core/src/warp_isa.inc`: the warp interpreter, one source compiled as C++ for the reference and as HLSL for the GPU;
- `core/src/warp_layout.inc`: the image it runs on (offsets, bits, Op numbers);
- `core/src/warp.cpp`: packing, unpacking and the warps' bookkeeping;
- `Soup::host_step` in `core/src/interp.cpp`: host instructions;
- `WarpGpu` in `core/src/accel_dx.cpp`: the kernel on Direct3D 12;
- `core/src/world.cpp`: the tick split, `warp.device` and its automatic choice;
- `core/src/budget.cpp`: `budget.gpu` sizing `warp.capacity`.

**Off unless `gpu.warps = true`.** With it off nothing changes: every existing test and pinned hash is as before.

**1. Warps (C1–C3).** A warp runs its body's code on `warp.lanes` lanes (default 32, up to 64), each a full CPU state.
- *Lockstep:* each step runs the instruction at the lowest IP among the lanes still running, for every lane at that IP. Lanes that branch apart take turns, and lanes that reach the same address again merge.
- *Offset heads:* lane L's heads sit L codons past the warp's. COPY, READ and WRITE move a head on by the lane count, inside the block that holds it (wrapping at its end), or along the soup outside the body's blocks. SETH puts lane L's head L past AX; GETH reads it back L before.
- *SENSE 20 and 21:* my lane, and the lanes in my warp. A CPU reads lane 0 of 1.

**2. Two forms of life (C4, Damian).** A body is born on a CPU (one CPU) or on a GPU (one warp).
- A daughter takes the form of what divided her: a warp's DIVIDE gives a GPU-born daughter.
- *Either resource will do:* a newborn holding only the other form's units is born in that form instead. One holding neither is born on a CPU and dies as before.
- *Sparks:* each spark draws `spark.gpu_grant` GPU units (default 0) as well as its millicores. Holding both, it starts on the GPU with chance `spark.gpu_born` (0.5). With warps on, a spark needs either pool to have units.
- *Arrivals* carry their form in the cargo (hashed and snapshotted with it).
- *Launch:* the GPU verb with BX 20 starts a warp at the code after its template's match. Every lane starts with the launcher's registers and heads, lane L's heads L past. It costs `gpu.setup` and needs GPU units; a body holds at most `warp.max` warps (4).
- *ENGULF* takes the prey's warps, up to `warp.max`. An eater born on a GPU takes the prey's CPUs too.
- *Upkeep:* a body with no running CPU pays its memory upkeep from its warps, at the cost of a one-lane step an instruction. A body with both pays from its warps whatever its CPUs fall short of.
- The form sits in the body's former padding word, so worlds without warps hash as before.

**3. A warp's world is its own body (C5).** Lanes fetch, read, write and search templates only in codons their body owns; anything else fails. So every body's warps run independently.

**4. Lanes read before they write (C6).** Within a step all lanes read first, then write. When two write one codon, the highest lane's value stays.

**5. World-changing instructions wait for the host (C7).** The host's instructions are ALLOC, RELEASE, DIVIDE, ENGULF, INJECT, DISK, EMIT, LISTEN, the GPU verb, and Tierra's mal and divide.
- A warp stops in front of one. After the round, the host does it once, in the schedule's order, with the lowest lane's registers.
- Every lane at the instruction then takes:
  - any register value the instruction changed;
  - any head move, as the same displacement, so each lane keeps its offset (an ALLOC leaves lane L's write head L into the new block);
  - the error flag.
- The instruction's CPU cost is charged to the warp's capacity at the serial rate, as debt into the next tick.
- Block ops (BX 0–15) fail in a warp.
- `warp.rounds` (default 1) gives warps that waited another round in the same tick.

**6. Costs (C8).** A GPU unit gives `warp.capacity` (256) of warp work a tick, at its heat-adjusted speed, after the body's block ops. A step costs `warp.step_cost` (16) plus `warp.lane_cost` (1) for each lane it runs, and fetches its instruction once for all of them. A body's running warps split its work equally, and a last step's overshoot is debt, as on a CPU. Warp work heats the body's GPU units as block ops do.
- `budget.gpu`, with warps on, also sizes `warp.capacity`, and needs no async mode for it. The calibration times the warp kernel on 4,096 bodies in COPY loops whose every lane agrees.

**7. Randomness (C9).** A lane's copy errors, flaws, salvage and RAND each draw from a 32-bit hash of the body's key for the tick, its warp, the step, the lane and the purpose, at the CPU's rates. Hot pages' refusal chances become whole-number thresholds before the round. Everything a warp does is whole-number arithmetic, bit for bit the same on any device.

**8. The real GPU (C10).** The kernel is `warp_isa.inc` itself, embedded at build time between a head that binds its macros to HLSL and a `main` that picks the body record: one thread group per body, one thread per lane, the body's warps one after another.
- *The tick:*
  1. every node's first half on the threads (sparks, CPUs);
  2. each round: every node packs its bodies' records; one dispatch runs every warp in the world; every node unpacks and does its host instructions;
  3. every node's second half (heat, hazards, deaths).

  `Soup::tick` does exactly that node by node on the reference.
- *A round's data:* each body's record (its blocks with ownership, its pages' thresholds and heat, SENSE readings taken just before the round, its key and quotas) goes up and comes back each round, copied in parallel on the threads.
- *Lanes live on the GPU* between rounds, in one heap with a region per node. The soup knows, warp by warp, which side holds the newer lanes (`Soup::lane_state`: the host, the GPU or both):
  - lanes the host changed go up with the round that runs them;
  - lanes the host needs come back first: a host instruction, a node the CPU runs this round, a hash, a snapshot, a check or the viewer (`World::warp_sync`);
  - the kernel returns each warp's live and running lanes, so the host's bookkeeping never reads lanes the GPU holds;
  - a node that outgrows its region gets a new heap, its lanes copied on the GPU.
- *Kernel details:*
  - records of at most 2,048 words run from groupshared memory, the rest from memory;
  - a body of many non-overlapping blocks gets a sorted index of them, so it is searched by halving rather than scanned;
  - the compiled kernel is kept in memory and as `nova_warp_<key>.cso` beside the executable. FXC's optimizer is skipped by default (`NOVA_WARP_O=3` turns it on; it made no difference to speed).
- *What the compiler (FXC, shader model 5.1) required:*
  - every value that steers a loop with barriers is decided by one thread and handed to all through groupshared memory, with a barrier after each hand-over;
  - a lane's stack stays in memory, because a local array indexed at run time can't live in such a loop;
  - since HLSL evaluates both sides of `&&` and both arms of `?:`, neither may have a side effect.
- *Which device:* `warp.device` is `auto` (the default), `gpu`, `cpu` or `mixed`. None of them changes the world.
  - `gpu` runs every node's rounds on the GPU; `cpu` never opens it.
  - `auto` shares each round: the GPU takes the nodes whose longest body ran at most a limit of steps last round, and the CPU threads run the rest on the reference *at the same time*. The GPU runs a body's steps one after another, so long chains go to the CPU.
  - *Retuned in Phase 9b (Damian):* the limit is searched for, measured. A round's cost is its wall time per warp step it ran, so a growing world doesn't read as a worse split. Each round moves the limit by 2^stride the way that helped last time; a dearer round turns back and halves the stride, a cheaper one keeps on a little faster. Rounds are compared only tick to tick. The search starts with every node on the GPU and can settle there (above twice the longest chain), probing back down. (Before, it aimed for the GPU's device time to match the CPU's and sent the longest node to the CPU whenever all were on the GPU: in GPU-heavy worlds that cost a third.)
  - `auto` also times whole ticks run that way against ticks without the GPU at all (the plain reference, as `cpu`), and runs the quicker. The first 10 ticks alternate; then it tries the other again every 25th tick while the two are within a quarter of each other, every 100th while one is up to twice as quick, every 400th beyond (it was every 500th).
  - `mixed` is for tests: nodes go to either at random every round, so lanes cross both ways all the time.
- *Checking:* `warp.verify = N` also runs every Nth GPU round on the reference and compares them word for word. The reference's result is kept, and a difference stops `nova run`.
- *A device error* stops the world, because the lanes the GPU held are lost: `nova run` stops with `warp_device_error`, and the run resumes from its last snapshot on either device. A GPU that won't start leaves warps to the reference, with a note.

**9. Detection (C13).** Lane writes count as GPU writes, so warp copying makes `gpu_replicator` links. A new milestone, `gpu_native`, marks the first DIVIDE link by a GPU-born parent in a lineage that has met the gate. `result.txt` records it with the warps' device and rounds, and the harness collects it.

**10. Seeing it (C14).**
- *Inspector:* a body's form and warps, and its lanes, with those of each warp's next step marked.
- *Map:* the colour "GPU lanes" (lanes still running on a node).
- *`stats.csv` and the charts:* `warp_steps`, `lane_instr`, `warp_host`, `launches` and `gpu_births` (sums), and `warps` and `gpu_bodies` (at the window's end), after every earlier column.
- *`--profile`:* the warps' CPU passes and GPU round, how `auto` shared the work (ticks with and without the GPU, node-rounds on each), the bytes and lanes a round moves, the device's own timestamps for upload, kernel and download, and the longest body's steps. `result.txt` records the rounds and node-rounds on each device and `warp_ticks_without_gpu`.
- *Preset and arms:* `configs/gpu_world.cfg`, the landscape with its GPU units placed against its CPU through a new distribution setting, `res.<x>.against = <another>` (rich where the other is poor), and a third of its nodes CPU-barren. Harness arms `warps` and `gpu_world`.

**Determinism (C12).** Suite `warp` checks:
- lockstep with divergence and merging;
- offset heads (a 32-lane COPY copies 32 codons in one step);
- read-then-write and the highest lane winning;
- own-body memory;
- host instructions;
- block ops failing in a warp;
- launches and `warp.max`;
- both forms of birth with the fallback;
- GPU-born sparks;
- the death rule;
- costs, debt and upkeep;
- ENGULF taking warps;
- the viewer's frame;
- a hand-written 32-codon GPU-born replicator that copies itself in one step on nodes with no CPU and reaches `gpu_native`;
- a busy procedural world (sparks of both forms, dispersal, disk, salvage, heat) giving the same hash at 1, 4 and 20 threads, under shuffled dispatch, through a snapshot and on either device;
- on the GPU, 120 rounds each checked word for word against the reference;
- `warp.device = mixed` at 20 threads (lanes crossing both ways every round, every third GPU round checked), through a snapshot, and `auto` switching between ticks with and without the GPU: the same hash;
- `gpu.warps = false` hashing exactly as no key at all.

Suite `budget` checks `warp.capacity` from `budget.gpu`.

**Changes from the plan.**
- *Lanes on the GPU, as C10 planned, came second:* the first build kept them on the host and sent them every round. After the first findings Damian asked for the speed-ups (2026-11-06), so lanes now live on the GPU and the GPU and the CPU share rounds. The cost: a device error now stops the run instead of handing over to the reference.
- *`warp.device = auto` measures and chooses* (the plan defaulted to the GPU), because in today's worlds the reference is quicker.
- *Damian on speed (2026-11-06):* the GPU being slower than the CPU at serial work is a feature, not a bug: the resources aren't built equally, and each has its strengths. In-world costs are still set by hand; see section 10.
- *`res.<x>.against`* was added to the landscape distributions for the preset.

**Findings (2026-11-06, home rig, Release, `configs/gpu_world.cfg` at 1,000 nodes).**
1. *GPU-born replicators appear almost at once:* at tick 18, from random sparks. One 32-lane COPY copies a whole short genome in a step, so a SENSE–ALLOC–COPY–DIVIDE program replicates; `gpu_replicator` and `gpu_native` fired together.
2. *Life takes the CPU-barren regions (the paired check, 3,000 ticks):*

   | | CPU-barren nodes (300): bodies | lineages there | other nodes (700): bodies |
   |---|---:|---:|---:|
   | warps on | 11,995 (40 a node) | 15 | 90,061 |
   | warps off | 281 (0.9 a node) | 1 | 115,573 |

   Warps also take memory on CPU-rich nodes.
3. *Speed:* a whole tick over 300 ticks. Every row gave the same world (hash 294579aef4c0e500):

   | `warp.device` | ms a tick |
   |---|---:|
   | `cpu` (warps on the reference) | 16.5 |
   | `gpu`, first build (lanes sent every round) | 45 |
   | `gpu`, lanes kept on the GPU | 41.8 |
   | the GPU and the CPU sharing every round | 33.7 |
   | `auto` (284 of 300 ticks without the GPU) | 17.2 |

   - *The first build:* the GPU round took 31 ms, a 17 ms kernel and 112 MB of records and lanes each way (about 25,000 live warps × 32 lanes × 116 bytes), whose copying took about 13 ms.
   - *Keeping lanes on the GPU* cut a round to 50 MB each way: the bodies' records. But almost every warp stops at a host instruction every round (about 9,000 warps' lanes sent and 9,000 fetched a round), so the lanes still make the trip, and fetching them takes 3.8 ms a round.
   - *The kernel's time is the longest body's chain of steps:* 3,168 steps at about 5 µs each, in a body of 94,733 words with hundreds of blocks. A GPU runs one body's steps one after another, waiting on memory.
   - *Sharing the round* sends the long chains to the CPU. That cuts the GPU's longest chain to about 160 steps and its kernel to 9 ms. But a GPU tick still carries about 9 ms on the device and 7 ms of packing, unpacking and host passes. That is more than the CPU spends on all the warps: about 6 ms, as bodies average about 10 steps a tick. So `auto` runs ticks without the GPU here and stays within 1 ms of `cpu`.
   - *Profile-driven fixes:* packing had a per-codon block lookup that was quadratic for many-block bodies (12.9 → 2.4 ms a tick, on both devices); lanes now move packed and in parallel; the sorted block index (kernel 33 → 17 ms); groupshared records (no change: not the bottleneck).
4. *Calibration:* the RTX 5080 runs 6.3 × 10⁹ warp work a second when every lane agrees.

### 6.15 Measured costs (Phase 9) [built 2026-11-06 in two steps (9a, 9b) from plan revision 2, approved by Damian]

The plan and its calls C1–C11 are in `docs/phase9-plan.md`. Damian's direction:
- Running on real time would be the most realistic, programs developing on real architecture, but it gives up determinism.
- So every instruction costs world time measured on his machine, once, outside the simulation, in a fixed table.
- Actions that take longer than a tick get a start, a time acting and an end, so their targets can respond: room for defences against being eaten.
- ENGULF isn't a machine instruction, so it takes time by the prey's size.

Code:
- `core/src/cost.cpp`: the tables and recipes;
- `Exec::step` and `run_cpu` in `core/src/interp.cpp`: charging time, actions in progress;
- 9b: the warps' clock in `core/src/warp_isa.inc` (`WC_OPTIME`, `WW_TLEFT`), their waits and actions in `core/src/warp.cpp` and `Soup::host_step`, the device ops' start-up and share in `Exec::device_time`;
- `app/bench_costs.cpp` and `gpu_microbench` in `core/src/accel_dx.cpp`: `nova bench costs`.

**Off unless `cost.table = home_v1`.** `classic` (the default) is every instruction costing 1, exactly Phase 8: every test and pinned hash is unchanged, and naming it outright hashes as no key.

**1. The machine, measured once (C1, C8, C9).** `nova bench costs` times native code, as if Nova's instructions were the machine's own, pinned to a P-core and then an E-core. Each figure is the median of 9 repeats; a run with a repeat spread over 10% is flagged. The table `home_v1` holds the medians of four runs on the home rig (2026-11-06), picoseconds:

| operation | what it is | P-core | E-core |
|---|---|---:|---:|
| alu | an add (a dependent chain) | 190 | 218 |
| mul | a multiply | 578 | 655 |
| div | a 32-bit divide | 2,650 | 4,424 |
| load | a load from the nearest cache (a pointer chase in 4 KB) | 755 | 872 |
| store | a store | 95 | 109 |
| jump | a jump the hardware can't predict, over one it can | 4,560 | 5,000 |
| scan | a codon searched | 275 | 437 |
| move | a codon moved in bulk (its byte and its owner) | 16 | 92 |
| draw | a random draw (splitmix64) | 2,650 | 3,489 |
| call | a kernel call (a round trip into Windows) | 389,000 | 628,000 |
| share | a cache line passed between two P-cores | 40,000 | |
| disk_read, disk_write | a random 4 KB read and write on E:, unbuffered | 45.6 µs, 12.3 µs | |
| disk_read_codon, disk_write_codon | a byte in sequence (4.1 and 5.0 GB/s) | 241, 200 | |

- The P-core runs at about 5.3 GHz by the add; loads further out take 1.0 ns (64 KB), 3.6 ns (1 MB), 20 ns (16 MB) and 85 ns (256 MB).
- Runs never measure, so every run, replay and snapshot uses these exact numbers. A re-measure would be a new table (`home_v2`).
- `nova costs [classic | home_v1]` prints a table: each operation and each instruction's own part, in picoseconds and in adds.

**2. Each instruction is what it would be as machine code (C4; the recipes are in `cost.cpp`).** Its own part:
- an add for most;
- a multiply, a divide (DIV, MOD);
- a store or a load (the stack, WRITE, READ, SENSE);
- a load and a store (COPY);
- a draw (RAND);
- a cache line between cores (EMIT, LISTEN).

Then, as it runs:
- a load for each template codon it reads, a scan for each place a search looks at, a jump when one is taken (JMP, CALL, RET and the like);
- a kernel call for every world change (ALLOC, RELEASE, DIVIDE, ENGULF, INJECT, DISK), plus a move for each codon it claims, gives up or hands over;
- for DISK, a disk access plus its codons in sequence.

So, in adds: a multiply is 3, a divide 14, a mispredicted jump 24, a kernel call 2,047, and a random disk read 240,000.

**3. World time (C2, C3).**
- Costs are whole picoseconds. A tick is `cpu.instr_per_core` adds on a P-core: 4.75 µs at the presets' 25,000. 1,000 millicores give that much time a tick, scaled by heat as before.
- Memory upkeep stays a rule, charged as that many adds.
- An overshoot is debt, as before. But one instruction can now run far past its tick (a kernel call is 2,000 adds), so heat sees at most a full tick of work in the tick that ran over, and the ticks that pay the debt off count as busy. Without that, a small body making a kernel call late in a tick read as 260% busy and overheated.

**4. Actions that take time: a start, a time acting, an end (C7, Damian).** Two kinds of instruction become actions in progress:
- a CPU's ENGULF or INJECT, always;
- any instruction longer than a whole tick: today a DISK op (a random read is about 10 ticks).

How one goes:
1. *Start:* ENGULF and INJECT pick their target as before (the best-matching surface in range), but don't roll yet. With no chance at all (no target, no matching bit, a payload that doesn't fit) they fail at once, as a lost contest does. Otherwise the action starts and the target is marked.
2. *Acting:* the actor's CPU waits, running nothing, its time spent. The target, and everyone else, carries on. Two new SENSE readings: 22, ticks until the soonest action on me ends (0 for none); 23, what it is (1 ENGULF, 2 INJECT). Warps get them too.
3. *End,* at the actor's turn: the action lands only if it still holds. For contact:
   - the target is alive and the same body;
   - it is still in range;
   - the roll, against its surface *as it is now*, succeeds;
   - an INJECT's payload still fits.

   A long instruction is simply done then, against the world as it is. Its instruction is the one it started, whatever has since been written over it.

ENGULF's and INJECT's pace are world rules, not measurements (Damian): `engulf.codons_per_tick` (16) of the prey, `inject.codons_per_tick` (16) of the payload. Either takes at least one tick, so every body gets a turn in between, wherever it sits in the schedule (bodies run one after another within a tick). The whole prey goes at the end, not bite by bite.

Further rules:
- An actor that dies or is eaten mid-action loses it, swept at the end of the tick.
- A contact action whose target is gone ends at once, failed.
- *Defences need no new instruction.* A failure whose chance fell since the start (the target rewrote its surface, moved or shrank) counts as a defence: `actions_defended`.
- A warp's ENGULF, INJECT and long instructions are actions too (9b): begun at its host instruction, the warp waiting at it; at the end tick's first round they land or fail as the host's end of the instruction, and the warp runs on.

**4b. The GPU and NPU (C6, 9b).** `nova bench costs` times the RTX 5080 with native HLSL microbenchmarks on its own clock (one 32-lane group, each lane its own dependent chain; median of 5), and both devices through Nova's backends for start-up and share. `home_v1` holds the means of two runs, picoseconds:

| operation | what it is | ps | adds |
|---|---|---:|---:|
| gpu_alu | a GPU lane's dependent add | 1,393 | 7 |
| gpu_mul | ... multiply | 2,028 | 11 |
| gpu_div | ... 32-bit divide (emulated on a GPU) | 30,900 | 163 |
| gpu_l1 | a load from the lane's nearest cache (4 KB) | 18,800 | 99 |
| gpu_mem | a load from 1 MB (the L2): a body's memory | 145,700 | 767 |
| gpu_vram | a load from 256 MB of video memory | 368,900 | 1,942 |
| gpu_thr | a lane-op's share of one lane, the whole GPU busy (1.54 × 10¹³ a second over 10,752 lanes) | 700 | 3.7 |
| gpu_start | a lone GPU op's round trip | 67 µs | 352,600 |
| gpu_share | each further op in a batch of 64 | 47 ns | 247 |
| npu_start | a lone NPU op's round trip | 3.37 ms | 17.7 million |
| npu_share | each further op in a batch of 16 | 10.3 µs | 54,000 |
| npu_mac | a multiply-add, the whole NPU (through DirectML's float16 graphs) | 2,200 | 11.6 |

So a GPU lane is about 7 times slower than a P-core at serial work, and far from its memory. The whole GPU does 1.54 × 10¹³ lane-ops a second, against the P-core's 5 × 10⁹ adds.

*Warps: two limits, as on a real GPU.*
- *Latency (each warp's own clock):* a warp has a tick of time a tick, and each step takes its instruction's time on a GPU lane (`cost.cpp`'s GPU recipes): an add 1.4 ns; the stack from the nearest cache; COPY, READ and SENSE from the body's memory in the L2 (147 ns); a divide 31 ns; RAND eight adds. So a warp of adds takes about 3,400 steps a tick, one copying about 32. A warp's template searches aren't counted (each lane searches on its own).
- *Throughput:* one GPU unit is one of the GPU's shader lanes: a tick of its time a tick, at `gpu_thr` a lane-op. A step fills the warp's whole width whatever lanes it runs, since lanes that branched away still hold their slots: 32 lanes × 700 ps. So a body's units bound how many full-warp steps its warps take, and one warp can't fill many units.
- The kernel has the latency clock built in: `WC_OPTIME` holds a time per Op, and each warp is given its time left (`WW_TLEFT`) and returns what it took. `classic` gives no clock and zero times, so warps there run exactly as in Phase 8 (the same hashes, and the GPU still verified word for word).
- *Host instructions:* a warp at one waits a GPU round trip (67 µs) plus the host's time for it, in whole ticks: about 15 ticks at the presets' 4.75 µs. A GPU program asking the CPU for something is expensive.
- *Launch:* a kernel call for the launcher; the new warp starts after the GPU's start-up.
- `warp.capacity`, `warp.step_cost`, `warp.lane_cost`, `gpu.elements` and `npu.macs` are set from the table (an NPU unit is a thousandth of the NPU).

*Block ops and NPU ops: start-up once, then each op's share (Damian, revision 2).* A body's ops on a device in one tick are a batch run in parallel:
- the first pays the device's start-up, each further op only its share;
- in instant mode the CPU then waits for the work on the body's units: elements at `gpu_thr` a lane, multiply-adds at `npu_mac` per thousandth of the NPU;
- an op longer than a tick is an action, done at its end. A lone GPU op is about 15 ticks and a lone NPU op about 710, so blocking on a device alone is costly, and batching pays;
- in async mode the CPU only sends ops (a kernel call opens the batch, then each op's share), and results come `accel.latency` ticks later as before. That latency stays a setting: the async barrier gives every op one latency.

**5. Seeing it (C11).**
- `stats.csv` gains `actions_started`, `actions_landed`, `actions_failed` and `actions_defended` after the warp columns.
- A new milestone, `defended`: the first gated lineage whose members made `detect.defended` (3) attacks fail by changing in time. `result.txt` records it and `cost_table`.
- The inspector shows what a body is doing and what is aimed at it, with the ticks left.
- `--profile` shows world time against real time.

**Findings (2026-11-06, home rig, Release, 2,000 ticks, the paired check).**
1. *A tick holds far less life.* `configs/landscape.cfg` (1,000 nodes):

   | | `classic` | `home_v1` |
   |---|---:|---:|
   | instructions a tick | about 11 million | about 1.1 million |
   | births a tick, ticks 1,500–2,000 | about 3,700 | about 19 |
   | ENGULF tries a tick | about 48,000 | about 320 |
   | first replicator | tick 504 (depth 4 by 2,000) | none in 2,000 ticks |
   | wall time a tick | 13.5 ms | 2.0 ms |

   Random code averages about 10 adds an instruction (jumps, searches, kernel calls). But the real brake is the kernel call: every ALLOC and DIVIDE costs about 2,000 adds, so a replication costs over 4,000 adds against about 30 instructions before. A 10-millicore spark gets 250 adds a tick, so one kernel call takes it 8 ticks.
2. *The world runs about 420 times slower than real time:* a tick is 4.75 µs of world time and took 2.0 ms.
3. *Actions:* about 160,000 began in the first 500 ticks, 80% landed, and about 1,700 failed as defences over 2,000 ticks. Nothing has been selected for that yet: it is random code that happened to change in time.
4. *`configs/gpu_world.cfg` in 9a:* a GPU-born replicator at tick 1,217 (`classic`: 18), with warps still on their Phase 8 costs. Superseded by 6 below.
5. *What this asks of the tuning (Damian: "we will need to heavily tune"):* whether every world change should cost a kernel call (a user-space allocator is nearer 20–50 ns), the tick's length, spark grants, and ENGULF's and INJECT's pace, all against what eating and replicating gain.

**Findings, 9b (2026-11-06, `configs/gpu_world.cfg` on `home_v1`, 200 ticks, Release).**
6. *GPU life now does far more than CPU life.* A warp's own clock gives it up to about 3,400 steps of adds a tick (Phase 8's capacity gave a few). Over the 200 ticks:
   - warps ran 5.9 million steps and 186 million lane instructions a tick, while CPUs ran 0.54 million instructions a tick;
   - 83% of births were on the GPU; at tick 200, 17,656 of 75,902 bodies were GPU-born, holding 26,759 warps;
   - `gpu_native` came at tick 79 (`classic`: 18; 9a: 1,217);
   - 164,480 actions began, 89,113 landed, and 782 failed as defences.
7. *For the first time the real GPU pays off.* A tick with every warp on the CPU reference took 124 ms; on the RTX 5080, 60 ms (a 52 ms kernel; the longest body ran 7,437 steps a round); every run gave the same world (hash 7af62b05a409c136). `warp.device = auto` first took 87 ms: its rules were made for worlds where the GPU lost. Retuned (6.14, "Which device"):

   | `gpu_world` | ticks | `cpu` | `gpu` | `auto` before | `auto` retuned |
   |---|---:|---:|---:|---:|---:|
   | `home_v1` | 200 | 124 | 60 | 87 | 70 |
   | `home_v1` | 400 | | 152 | | 157 |
   | `classic` | 300 | 16.9 | | 17.2 | 17.8 |

   On `home_v1` it runs ticks without the GPU while the world is small (48 of the first 200), then puts about 91% of node-rounds on the GPU; by 400 ticks it is within 3% of all on the GPU. On `classic` it runs 290 of 300 ticks without the GPU; the gap is its first 10 alternating ticks, which fade over a long run.
8. *The cost of simulating it:* `gpu_world` on `home_v1` runs 12,600 times slower than real time on the GPU (60 ms for 4.75 µs of world time), against about 420 for the landscape without warps. Most of that is warps' work, the life that runs on the GPU.

**Determinism (C10).** Suite `costs` checks:
- the tables and recipes;
- each instruction's charge (a JMP's template, places searched and jump);
- a tick's budget (100 adds or 33 multiplies);
- ENGULF as an action: the prey marked and sensing it, the eater waiting, landing at its end, a bigger prey taking longer;
- a defence: the prey's rewritten surface makes it fail;
- an actor dying mid-action, and a target eaten first;
- INJECT as an action;
- a DISK store over three ticks;
- a busy timed world with the same hash at 1, 4 and 20 threads and under shuffled dispatch, and exact through a snapshot taken with actions in progress;
- `defended` firing;
- 9b: the device table and what it sets; a warp's clock (13 steps of adds in a 19,000 ps tick) and its units' throughput binding instead (7 steps on 8 units); a host instruction's 15-tick wait; a warp's ENGULF as an action; a block op as a 15-tick action;
- 9b: a busy timed warp world with the same hash at 1, 4 and 20 threads, under shuffled dispatch and through a snapshot, and on the real GPU with every round checked against the reference (200 of 200, none differed) and split between the GPU and the CPU at random.

## 7. Technical direction [decided via 7.1]

- C++ (or Rust) core; Python only for analysis. *Superseded by 7.1 A and B: C++20, no STL, no third-party, no Python.*
- Components: per-node soup, interpreter, scheduler (cycles paid with resources), reaper, mutation, resource layer, graph, migration, lifecycle, observation. *Updated list in 7.1 C; the reaper and slicer are removed.*
- Parallelism: partition graph into regions per CPU thread; batch cross-region migrations at intervals; also many independent worlds. *7.1 D/E: per-tick shared task queue (not static regions) with a tick barrier and per-edge outboxes.*
- GPU: hard for the interpreter (divergence, irregular memory); GPU-native design might give ~5–50x, needs benchmarking. Resource math is GPU-friendly. Not v1. *7.1 G: the interpreter stays on the CPU; the in-world GPU comes right after proof of concept and is the first candidate for real-GPU batching.*
- Real tensor cores/NPUs: not useful for the interpreter.
- Island model: prefer occasional migration between worlds over resetting to the "best" world (resets destroy lineages; metric selection invites Goodhart). Keep isolated controls.
- Data: snapshots + stats; files or SQLite; replay and lineage tracing. *7.1 B/H: files only (no SQLite); genebank; genotype-level lineage.*
- Everything config-driven and seed-reproducible.

### 7.1 Reconciling Section 7 with the decisions [decided — B, K and the GPU timing first, then everything else, including Claude's GPU and knob-logging suggestions, accepted by Damian, 2026-11-05]

Context from memory:
- Damian's global rule is minimal dependencies, and any new dependency (library, package or required installed tool) is discussed first.
- His personal C++ projects, ScratchML and itracker, go further: no C++ STL and no third-party libraries. Thin C-runtime headers (cstdint, cstdio, cmath, …) and platform headers are allowed.
- Hardware: i7-12700H (14 cores: 6 performance + 8 efficiency; 20 threads), 40 GB RAM, RTX 3050 Ti Mobile (~4 GB).

- **A. Language and toolchain.** C++20 with MSVC (Visual Studio 2026 Community, v18), CMake + Ninja, x64: the same stack as itracker, including a `verify.ps1` build-and-test loop and a hand-rolled test harness. Drop Rust.
  - *Updated in Phase 1a:* this line said VS2022 until then. VS 2026 is the only Visual Studio on the machine, and `verify.ps1` finds it through vswhere. It ships without the Clang component, so verify's advisory clang-format step is skipped (and says so).
- **B. Dependencies [decided — Damian: same rules as ScratchML and itracker].** Nova is written with **no C++ STL** (no `std::` containers, algorithms, strings or iostreams) and **no third-party libraries**. Thin C-runtime headers (`cstdint`, `cstddef`, `cstdio`, `cstdlib`, `cmath`) and platform headers (`windows.h`, intrinsics) are allowed: the line is no-STL, not no-C-runtime. Data structures, RNG, hashing, config parsing, the compressor estimate and the test harness are all hand-rolled. Any exception gets discussed with Damian first. In addition:
  - *SQLite is dropped* in favour of Nova's own versioned binary snapshots plus CSV stats, so there's no library to discuss.
  - *No Python for analysis* **[decided — Damian]**. Analysis lives in Nova's own tooling. Until the UI exists (K below), stats export as CSV, which opens in Excel with no new tool.
  - *Config* is a hand-parsed `key = value` text file, so no JSON library is needed.
- **C. Components, updated to the decisions:**
  - codon decoder and interpreter (table v1, swappable by config);
  - per-node soup and ownership map;
  - resource ledger and free pool (millicores, memory, disk, GPU);
  - heat model (per-unit heat, ambient leak, error-rate curves, throttle-then-release);
  - hazards (bit rot, volatility wipes, wear);
  - contest resolver (ENGULF/INJECT surface match);
  - death sweep (no running CPU, then release);
  - sparks;
  - dispersal transit queue;
  - signal medium (superposition, attenuation, noise floor);
  - node lifecycle (drifting noise, starvation counter, wear collapse, creation);
  - observation hooks.

  *Removed:* reaper and slicer.
- **D. Determinism under parallelism (key).** "Seed-reproducible" must hold regardless of thread count or timing.
  - *Tick barrier:* within a tick, each node updates using only its own state. Cross-node effects (dispersal arrivals, signals) go into per-edge outboxes and are applied at the barrier in a fixed order by node id.
  - *Counter-based RNG:* every random draw is a hash of (world seed, node id, tick, event counter), using a hand-rolled SplitMix64-style mixer. No shared random stream, so no ordering races.
  - *Fair order within a node:* bodies run in a fixed cyclic order, but the starting position rotates each tick from the node's RNG. A fixed start would give one body a permanent first-mover edge (it would always ALLOC first).
- **E. Parallelism on this machine.**
  - Nodes are tasks in a thread pool with a shared per-tick queue, not a static partition into regions. With 6 performance and 8 efficiency cores, static regions would leave the fast cores waiting on the slow ones every tick.
  - Paired experiments (a run and its shuffled-selector control, doubled vs plain codon tables) run as separate processes.
- **F. Memory is not the constraint.** One byte per codon means a 64K-instruction node is 64 KB, so 1,000 nodes is about 64 MB of soup. Ownership, heat and body state are small by comparison. The real budget is how much gets logged.
- **G. GPU and NPU.**
  - **In-world GPU timing [decided — Damian]:** the GPU is a valuable resource. Programs that claim GPU cores gain extra compute for their otherwise serial instruction stream, because one block op does many elements' work in a single instruction. It comes in right after proof of concept: the first feature enabled once the abiogenesis test has produced replicators. The engine carries the GPU resource type, the GPU instruction and its heat model from day one.
  - *Claude's suggestions (accepted):*
    - keep the GPU off during the first abiogenesis test, since a block copy would make replication much shorter; then run abiogenesis with GPU on versus off as a paired experiment, to measure how much it changes the origin;
    - implement the virtual GPU ops on the CPU first. They're integer ops on codons (copy, add, xor, compare), so they're deterministic and cheap to build.
  - *Real GPU (an optimisation, later):* the in-world GPU's work is data-parallel, unlike the interpreter, so it's the natural first candidate for the real RTX 3050 Ti: batch every body's block ops at the tick barrier and run them in one CUDA pass. CUDA is a toolkit dependency. ScratchML's contract already allows it, but per memory 1642 it gets confirmed for Nova when we get there.
  - The interpreter stays on the CPU. It is branchy and the soups are small, a poor fit for a GPU. Not v1.
  - Later, measure before porting anything. Heat and resource math, or running many worlds side by side, are the candidates, and ScratchML's CUDA work is a head start.
  - The NPU isn't a fit: DirectML is the only documented path, and it suits tensors, not interpreters.
  - *Superseded by Phase 5 (6.11), at Damian's request (2026-11-06):* the in-world GPU and NPU ops now run on the real devices, asynchronously and in batches, through Windows' own Direct3D 12 and DirectML rather than CUDA (not installed, and not needed). The interpreter still runs on the CPU. The home rig has an RTX 5080 and the Core Ultra 9 285K's NPU.
- **H. Data and lineage.**
  - Versioned binary snapshots of the whole world, for replay.
  - CSV stats per window of ticks.
  - A Tierra-style *genebank*: save a genotype once its count passes a threshold, rather than logging every individual.
  - Lineage recorded genotype-to-genotype.
  - The compressibility measure (6.4 #24) uses a hand-rolled LZ-style estimate, so there's no zlib.
- **I. Performance target.** A lean switch-dispatch interpreter should manage roughly 10^8 instructions per second per core (an estimate, to be benchmarked), or about 10^9 per second across the machine. Benchmark this early: the abiogenesis test's waiting time depends on it.
  - *Measured in Phase 1a (2026-11-05)* with `tools\bench.ps1`: a Release build on the i7-12700H, 5 s per measurement, and nodes of 4,000 millicores at 25,000 instructions per core per tick. Millions of instructions per second:

    | workload | 1 thread | 20 threads (40 nodes) |
    |---|---:|---:|
    | tierra — Ray's ancestor, open membrane, no heat or hazards | 53 | 449 |
    | sketch — the 6.2 replicator under Nova's rules, heat and baseline hazards | 12–16 | 127 |
    | random — a sparks-only random soup, Nova's rules (the Phase 1b workload) | 38 | 223 |

  - **The Phase 1b budget should start from about 3.5 × 10⁷ instructions per second on one core and about 2 × 10⁸ across the machine** for the random-soup workload. That is roughly a third of the per-core estimate and a fifth of the machine-wide one.
  - *Where the time goes:* template searches dominate Tierra-style code (one search per 10 instructions in the ancestor's copy loop). Free-run scans for ALLOC once dominated crowded soups; they are now word-at-a-time with an exact failure cache.
  - *Scaling:* 20 threads give about 6–8× one thread. The machine has 6 P-cores with hyperthreads plus 8 E-cores, and with only two nodes per thread the slowest node sets each tick's pace. The tick barrier itself is not a factor at these task sizes (a tick is milliseconds), so the spin-then-block barrier Phase 0 flagged is not needed.
  - *Noise:* single-thread figures vary by about ±20% between runs, depending on whether Windows places the thread on a P-core or an E-core.
  - *Measured on the home rig (2026-11-05)*: Intel Core Ultra 9 285K (8 P-cores + 16 E-cores, 24 threads), same settings, 5 s per measurement, all 24 threads (48 nodes). Millions of instructions per second, Phase 1a code before and Phase 1b code after:

    | workload | 1 thread before → after | 24 threads before → after |
    |---|---:|---:|
    | tierra (light Tierra mode, no upkeep) | 81 → 82 | 1,440 → 1,318 |
    | random1a (the Phase 1a random soup, `mem.upkeep = 0`) | 79 → 74 | 796 → 759 |
    | random (the `configs/abio.cfg` soup: upkeep, wear 0.001, sparks last, the detector) | — → 52 | — → 607 |
    | sketch (now under memory upkeep, so a different workload) | 37 → 39 | 319 → 609 |

    The Phase 1b engine costs about 5–7% on the old random soup (the detector's hooks and the selector-map lookups), inside the noise band. The abiogenesis soup itself runs about 23,000 instructions a tick and is slower per instruction (more births, deaths and sparks, upkeep for every body, and the detector comparing every DIVIDE).
  - *Phase 1b budget:* 18 trials at once get roughly 25 million instructions per second each, so a 12-hour trial is about 10¹² instructions. The smoke runs met the gate after 4 × 10⁸ – 10⁹, so as things stand a trial ends (50,000 ticks after its first detection) within minutes, not hours.
  - *Phase 3 world (2026-11-05, home rig, 3 s per measurement):* the `world` workload, `configs/world.cfg`'s 100 procedural nodes of random soup, runs 73 million instructions a second on one thread (91 ticks/s) and 519 million on 24 (543 ticks/s), about 7× one thread. A 12-hour world run is about 2 × 10¹³ instructions.
  - *Phase 4 (2026-11-05, same settings):* `world` 83 million instructions a second on one thread and 531 million on 24; `world4`, the same world with every Phase 4 feature on at its suggested value, 67 and 382 million. Everything on costs about a fifth to a quarter per instruction (salvage checks on reads, the signal scatter, the disk).
  - *Phase 5 (2026-11-06, same settings):* `world` 84 and 538 million instructions a second (1 and 24 threads), `world4` 76 and 459. `worldacc`, the world with 200 GPU and 200 NPU units a node in async mode on the RTX 5080 and the NPU, runs 57 and 305 million (76 and 349 ticks a second): the accelerator verbs themselves, plus the barrier waiting on the NPU (6.11 finding 3). `nova bench devices` times the devices alone (6.11 finding 2).
  - *Phase 7a (2026-11-06, same settings):* `landscape`, `configs/landscape.cfg`'s 1,000-node landscape, runs 78 million instructions a second on one thread (14 ticks a second) and 898 million on 24 (147 ticks a second, from tick 0): with 1,000 nodes every thread stays busy, so it scales 11.6× (the 100-node `world`: 6.3×). Longer runs slow down as the soups fill: 11.7 ms a tick over the first 1,000 ticks, and 71 ms at 10,000 nodes (6.13 finding 3).
- **J. Island model:** kept. Occasional migration between worlds is fine; picking a "best" world stays banned (6.4 #5). Isolated controls stay.
- **K. Analysis UI [decided direction — Damian; built later].** The goal is Nova's own UI with visualizations and knobs to tweak settings. The "fancy stuff" comes after the core works.
  - *Tech:* a native Win32 window drawn with GDI meets the no-STL, no-third-party rule.
  - *Claude's suggestion (accepted):* changing a knob mid-run is logged as a timed event in the run record, so a replay reapplies it at the same tick and runs stay exactly reproducible.
  - *Until then:* Section 8's data side (stats, genebank, lineage, snapshots) is still built early; only the visual layer waits.
  - *Built in Phase 6 (6.12, 2026-11-06):* `nova view`, a native Win32 window drawn with GDI, with the map, charts, inspector, genome browser, phylogeny and logged knobs. Section 8's "who eats whose waste" view, the species and partnership detectors, comparing runs side by side and listening to the signal channel are still to come.

## 8. Observation tooling [decided: build the data side early; the visual UI comes later, 7.1 K]
Biome map (hidden coords) with population, migration, territories, collapse/creation overlays; population graphs; genome browser; phylogeny; "who eats whose waste" view; detectors for species/partnerships/dependencies; optional signal-channel listening.

## 9. Phased build plan [decided via 9.1]
Build engine for full vision, enable features one at a time as experiments. Scope creep is the #1 risk. *(The original steps below are superseded by 9.1.)*
1. Tierra-style baseline on one node (reproduce parasites/arms races).
2. Observation tools.
3. ~100-node graph, 1–2 resources, procedural gen, migration.
4. Add individually: waste/scavenging, memory volatility, disk tiers, edge corruption, node lifecycle, virtual GPU/tensor, signal channel.
5. Scale to 1000+ nodes and parallel worlds.
*Update (6.1):* Nova's main mode starts from a random soup with no replicator in it **[decided]**. Step 1 therefore splits in two. (a) Validate the engine against Ray's published numbers, using a seeded replicator and Tierra's open membrane as a config mode. (b) Run an abiogenesis test on one node under Nova's rules: do replicators emerge? Run it alongside a shuffled-selector control (6.3) to measure how much the selector ordering matters. (b) is Claude's suggestion and hasn't been discussed yet. If replicators don't emerge, adjust the instruction set or rules, or fall back to a seeded start.
Milestones: first self-replicator to emerge from random code, first parasite, stable specialists, migration patterns, scavengers, regional species, first obligate partnership, attached/multicellular-like forms.
Rough timescales: Tierra-level phenomena seconds–minutes; ecology minutes–hours per run; transitions days–weeks if ever. Weeks–months of tuning before a world reliably produces ecology.

### 9.1 Revised build plan [decided — accepted by Damian, 2026-11-05, with local git only and no SVN]

The original steps above predate today's decisions. Principles carried over: build the engine for the full vision but enable features one at a time as experiments, and treat scope creep as the #1 risk. Every feature switch is config, and every run is seed-reproducible.

**The validation catch.** A faithful Tierra clone would mean building Tierra's reaper queue, time slicer, 32-instruction set and open membrane — mostly the parts Nova *removed* — just to reproduce Ray's parasites. *Decided instead: a light Tierra mode.*
- Ray's instruction set is a second codon table (tables are already config), and open read/execute is a config flag.
- Run Ray's ancestor and check the *exact* replication cost Ray published: 839 instructions for the first copy, 813 for each after. That is a precise, cheap test of the interpreter, template search and copy semantics.
- No reaper or slicer is built, and reproducing Tierra's parasites is dropped as a goal, because Nova's world differs on purpose.

**Phase 0 — Skeleton and the determinism guarantee.**
- *Repo and tools:* a local git repo only, **no SVN**. Nova is a personal project, not company or client work (Damian). C++20, MSVC, CMake + Ninja; `verify.ps1`; hand-rolled test harness.
- *Hand-rolled core:* the no-STL containers, the counter-based RNG, the `key = value` config parser, the tick loop with barrier and outboxes, and the thread pool.
- *Data side from day one:* snapshots, CSV stats, the replay log (including knob events), the genebank and the compressibility estimate, all needed to *see* the Phase 1 results.
- *Gate:* the same seed gives an identical world-state hash at 1, 4 and 20 threads.

**Phase 1a — Interpreter on one node.**
- Codon table v1 decoder, and the private-code read/write bounds.
- The ownership ledger (millicores, memory), heat (per-unit, throttle-then-release, error-rate curves) and hazards (bit rot, copy errors, execution flaws).
- ALLOC/DIVIDE mechanics, ENGULF/INJECT with the surface contest, the death sweep, and sparks.
- *Gates:* the light Tierra mode matches Ray's 839/813; the hand-written 8-instruction sketch replicator works under Nova's rules (seeded, as a smoke test only); benchmark instructions per second per core.
- *Built 2026-11-05.* Exact semantics are in 6.5.
  - *Gate 1:* the ancestor replicates in 827 instructions, then 809 per copy, breeding true. These are Ray's documented figures; 839/813 is not reproducible from his documentation (6.5).
  - *Gate 2:* the sketch makes exact copies in every generation that receives CPU, and the zero-share generation dies.
  - *Gate 3:* the benchmark results are in 7.1 I.
  - *Gate 4 (Phase 0's, kept):* a busy soup world gives identical hashes at 1, 4 and 20 threads, and under reversed and shuffled dispatch. Phase 0's demo world (kept as `world.model = demo`, (H)) still hashes to bcaf7639570ecb07.

**Phase 1b — The abiogenesis test (proof of concept).**
- One node, sparks-only start, Nova rules, GPU off.
- Paired runs: the shuffled-selector control; doubled versus plain codon table; INJECT/ENGULF doubled versus single.
- Detection: genotype counts plus compressibility. Spark settings are tuned here.
- *Gate (the milestone):* the first self-replicator emerges from random code. *Fallback:* if a fixed compute budget passes without one, revise the instruction set or rules, or start from a seeded replicator.
- *Built 2026-11-05* from plan revision 3 (`docs/phase1b-plan.md`); rules, controls, detection and the harness are in 6.6. Smoke runs met the gate within seconds through INJECT-based copying; Damian then gave INJECT its own criteria (a key matching the host's surface, a host the payload fits in), after which replication goes through COPY and the paired controls separate (6.6, finding 1).

*Long runs wait [decided — Damian, 2026-11-05]:* the 12-hour paired experiments (Phase 1b's first experiment, and the paired runs of later phases) wait until everything is built, when the settings get tuned together. Until then each phase is checked with short runs of minutes.

**Phase 2 — GPU (the first feature after proof of concept).**
- In-world GPU block ops (CPU-implemented) are enabled.
- Paired abiogenesis: GPU on versus off.
- Watch for the first lineage that claims GPU units.
- *Built 2026-11-05* from `docs/phase2-plan.md`; semantics and the first short paired check are in 6.7. The GPU sped up emergence about 4× in that check, and the `gpu_replicator` milestone was reached from random code within two minutes.
- *Phase 2b (added by Damian, built 2026-11-05):* a 16-op GPU and the in-world NPU, as one accelerator verb (6.8, `docs/phase2b-plan.md`). Short check: the richer GPU helps emergence about 1.3×, the NPU not at all, and a replicator lineage used the NPU (`npu_user`).

**Phase 3 — The world.**
- A ~100-node graph from procedural generation (resource and climate noise fields), edges, the dispersal exchange and transit, many origins, and node properties.
- *Milestones:* first colonists, first dispersal strategies.
- *Built 2026-11-05* from `docs/phase3-plan.md`; semantics and the first short runs are in 6.9. From random code on 100 nodes the first replicator was already a disperser and colonists reached every connected node within 750 ticks; a seeded founder on one node colonised the whole connected world. About 1–2% of copies disperse.

**Phase 4 — Add individually, each its own experiment:**
- volatility wipes plus the disk tier (repair from disk);
- the node lifecycle (wear, starvation collapse, creation/recovery as one mechanism);
- edge corruption and link failures;
- the ambient heat leak;
- the signal channel.
- *Built 2026-11-05* from `docs/phase4-plan.md` (revision 2); semantics and the short checks are in 6.10. Damian added remnants and a salvage rule during the build (a much smaller program gets only pieces of a bigger one's code), and made collapses leave ruins. Each feature is its own switch and harness arm.

**Phase 5 — The real GPU and NPU (added by Damian, 2026-11-06).** The in-world GPU and NPU ops on the real devices through Direct3D 12 and DirectML, asynchronous and batched, with a high-volume cost profile. Damian moved it ahead of the UI ("I want the resource landscape to use everything I have got").
- *Built 2026-11-06* from `docs/phase5-plan.md`; semantics and findings are in 6.11. Every device result matches the CPU reference bit for bit, so the hash doesn't depend on the device. The NPU's driver forced a one-dispatch-per-item design; in the first checks the devices are correct and busy but not yet faster than the CPU reference.

**Phase 6 — Analysis UI** (was Phase 5; plan approved, parked, then unparked by Damian, 2026-11-06). Native Win32 + GDI: biome map, population graphs, genome browser, phylogeny, and knobs that are logged as replayable events.
- *Built 2026-11-06* from `docs/phase6-plan.md`; what it does and the first findings are in 6.12. Watching a run changes nothing (a test checks the hash with knobs turned from a pretend UI), and the viewer browses finished runs and continues snapshots.

**Phase 7 — Scale** (was Phase 6; plan in `docs/phase7-plan.md`). 1,000+ nodes, parallel worlds with island migration. *Revised by Damian (2026-11-06), plan revision 2 approved:* one big world generated like a game level (land and water, separate landmasses, paths generated once by triangulation, nodes with 0 to 10+ paths, bridges between landmasses that open on a schedule), distributions that make nodes scarce, abundant or barren in sparks and each resource, and budgets mapped to the machine's real RAM, a real disk file and the real devices (CPU stays virtual). Built in two steps: 7a the landscape, 7b real resources.
- *7a built 2026-11-06*; what it does and the first findings are in 6.13. A new `world.graph = landscape` beside the old modes, presets for continents, an archipelago and a maze, `nova land` to see what settings generate, and `nova run --profile`: at 10,000 nodes the tick is still 98% parallel.
- *7b built 2026-11-06* (6.13, "Real resources"): budgets for memory, disk, the GPU and the NPU become ordinary recorded settings; the disk tier can live in memory-mapped files at `disk.path` with no change to any result; worlds too big for the machine are refused; runs leave a thread free at below-normal priority.

**Phase 8 — Programs on the GPU** (added by Damian, 2026-11-06; plan in `docs/phase8-plan.md`). GPU cores that run a body's code in lockstep across many lanes, with an instruction set as rich as the CPU's: the CPU for serial work, the GPU for parallel, so a GPU-rich, CPU-poor node can hold life. *Revised by Damian, plan revision 2 approved:* births on a GPU or a CPU (two forms of life), and the real GPU now rather than later.
- *Built 2026-11-06* in two steps (8a: warps and the two forms on the reference; 8b: the real GPU); what it does and the first findings are in 6.14. One interpreter source compiled for both the CPU and the GPU gives the same world bit for bit on either. GPU-born replicators appeared from random sparks within 18 ticks and took the CPU-barren regions. In today's worlds the CPU reference runs warps faster than the GPU, so `warp.device = auto` measures both and uses the quicker. *Speed-ups (Damian, 2026-11-06):* lanes now stay on the GPU, and the GPU and the CPU share each round; `auto` also runs whole ticks without the GPU when that is quicker.

**Phase 9 — Measured costs** (added by Damian, 2026-11-06; plan in `docs/phase9-plan.md`, revision 2 approved). Rather than run on real time (a rewrite that gives up determinism), every instruction costs world time measured once on the real CPU, memory, disk, GPU and NPU, in a fixed table in the source (`home_v1`). A body's GPU work in a tick is one batch: start-up once, then each op's share run in parallel.
- *9a built 2026-11-06* (6.15): the table for the CPU, memory and the disk; world time; ENGULF, INJECT and long instructions as actions with a start, a wait and an end; the SENSE readings for them; the `defended` milestone. `classic` (the default) keeps every hash. On `home_v1` a tick holds a tenth of the instructions and replication gets much dearer (a kernel call per world change), so the balance needs tuning before it becomes the default.
- *9b built 2026-11-06* (6.15, 4b): the GPU and NPU measured (native microbenchmarks on the GPU's clock, start-up and share through the backends); each warp's own clock and its units' throughput, built into the shared kernel; host instructions waiting a GPU round trip; warps' actions; device ops in per-body batches. Tuning comes after real stress tests (Damian). Slow actions and actions on other bodies take ticks, with a start, a time acting and an end, so targets can sense them and respond: room for defences to evolve.

**Milestones (updated):**
- first self-replicator from random code;
- first GPU-using lineage;
- first colonists of another node;
- first ENGULF predator;
- first INJECT (viral) lineage;
- first kept passenger (endosymbiont);
- succession on a recovered node;
- first sustained signal use;
- stable specialists and regional species;
- first obligate partnership;
- higher-order (composite) programs.

**Timescales:** the abiogenesis wait is the big unknown until Phase 1a's benchmark. Set the Phase 1b compute budget from that measurement rather than in advance.

## 10. Open questions
Diversity/complexity/novelty metrics; spark settings and other starting parameter values (tuned in the abiogenesis test). *From Phase 4 (6.10):* the `signaller` milestone may need a stricter reading before it means communication; and starvation's settings (with the ebbing meter it collapses busy nodes every few thousand ticks at the suggested values) stay as they are until a solid 12-hour run, then get tuned with the rest (Damian, 2026-11-06). *From Phase 5 (6.11):* making the devices pay off: send the NPU only batches big enough to beat its 3.4 ms round trip (routing never changes a result), pack codons four to a word for the GPU, run the CPU reference's share in parallel across nodes, and choose `accel.latency` against the tick time. The Intel integrated GPU is not used yet. *From Phase 6 (6.12):* `detect.genebank_threshold = 2` saves about 10 genotypes a tick in `configs/world.cfg`, so `genebank.txt` would reach gigabytes in a 12-hour run; the threshold gets tuned with the rest before the long runs. *From Phase 7 (6.13):* a disk in files is written whole at start-up (about 26 s for 20 GB); starting it lazily (pages read as their seeded contents until first written) would make big disks start at once. `kRamPerCodon` (20) is a ceiling measured on 16,384-codon soups; a long run should confirm where full populations of big soups land. Landscape settings want tuning with the rest (`nova land` makes that quick). *From Phase 8 (6.14):* the GPU pays off only when warps do much more work a tick than today's (long lane-parallel loops in many bodies). Lanes now stay on the GPU and the GPU and the CPU share rounds (built 2026-11-06). The next profile-driven steps for that day, none of which can change a result:
- bring back the lanes of warps waiting on a host instruction with the round's own readback, not in a second trip (3.8 ms a round);
- move only the parts of body records that changed (50 MB each way a round at 1,000 nodes);
- make the host's packing and unpacking passes cheaper (7 ms a tick);
- shorten a long body's chain of steps on the GPU.

Damian sees the GPU's slowness at serial work as a feature (6.14). The warps' in-world costs (`warp.step_cost`, `warp.lane_cost`, `warp.capacity`) are still set by hand; Phase 9b measures them.

*From Phase 9 (6.15):* the balance between actions and instructions needs heavy tuning (Damian): ENGULF's and INJECT's pace (`engulf.codons_per_tick`, `inject.codons_per_tick`), and how the per-tick rules (heat gains, upkeep, spark grants) sit against a world where a kernel call costs 2,000 adds and replication costs far more than it did. Also open: whether a CPU waiting on the disk should count as busy (it does today) or idle and cooling, and the P-cores and E-cores as two kinds of CPU. *9b:* a warp's template searches aren't yet counted on its clock; async device results keep the hand-set `accel.latency`. (`warp.device = auto` was retuned for GPU-heavy worlds the same day.)

Also, replication on warps is easy (one 32-lane COPY copies a short genome whole), so whether `gpu_native` comes too soon is for the tuning with the rest. *Resolved 2026-11-05:* Section 6 (organisms); the energy/cost model (owned resources governed by heat; no abstract energy); the node creation rule, collapse consequences and recovery details (4.4a); signal channel design and the v1 resource tiers (5.1); technical direction, dependencies and analysis tooling (7.1); the phased build plan (9.1).
