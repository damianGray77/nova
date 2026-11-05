# Nova — Design

> **Status:** Phase 0 (the skeleton and determinism gate) and Phase 1a (the interpreter on one node; semantics in 6.5) are built. Next: Phase 1b, the abiogenesis test (9.1).
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
  - *GPU* (enabled right after proof of concept, per 7.1 G): one selector op (copy, add, xor, compare) applied across CX elements from the read head to the write head. Each element is cheap; setup costs extra and the GPU runs hot. It has to be claimed via ALLOC like any resource. A GPU block copy is effectively a "faster polymerase", a major innovation evolution can discover once GPUs are enabled. It is disabled in the abiogenesis test, so it can't shortcut the origin of life.
  - *Divergence penalty and tensor/NPU unit:* deferred.
- **E. Bandwidth [decided].** Between nodes, edges (4.4a F). Within a node, DISK and GPU costs scale with the amount moved. No separate bus model in v1.
- **F. Waste [decided — 6.4 #9, plus disk fossils]:** unowned, readable, copyable and claimable. Sparks animate it. It is also the fossil record, now including dead programs' disk contents.
- **G. Signal channel design [decided — adopted by Damian].**
  - *Superposition instead of collision rules.* Emissions on the same channel add up, like sound. Listeners hear the sum, so crowding becomes interference without any Ethernet-style rule. Programs gain from using quiet channels: the acoustic niche hypothesis in Section 2.
  - *Per-hop attenuation.* A signal is multiplied by a fixed factor per hop, delayed by edge latency, and dropped below a per-node *noise floor* (random background on every channel; it could follow biome noise).
  - *Signals last one tick,* so sustained messages must be re-emitted. Silence is free; speech is not.
  - *Loudness costs heat.* EMIT's amplitude is CX, and heat rises with amplitude. That makes signals costly and therefore tends to keep them honest, as in honest-signalling theory.
  - *Public.* Anyone can LISTEN, so eavesdropping predators and deceptive signals are possible. Channels are a commons, never owned.
  - *Size:* a fixed number of channels per world (e.g. 16) selected by BX; values are register numbers. Off in early phases (EMIT and LISTEN set the error flag). The researcher can tap any channel; it is never routed to real speakers.

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
1. **Death.** A body with no running CPU dies. Its holdings return to the free pool and its memory becomes waste. A CPU halts after K = 8 *consecutive* errors (tunable). Without this, a body whose CPUs had all halted would hoard resources forever, breaking "nothing grants permanence". Every cause of death runs through this one definition: being jammed by an eater, piling up errors, or code too corrupted to make progress.
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
| ENGULF, INJECT | contact (below) | no target in range, or the contest is lost |
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
- **J. Island model:** kept. Occasional migration between worlds is fine; picking a "best" world stays banned (6.4 #5). Isolated controls stay.
- **K. Analysis UI [decided direction — Damian; built later].** The goal is Nova's own UI with visualizations and knobs to tweak settings. The "fancy stuff" comes after the core works.
  - *Tech:* a native Win32 window drawn with GDI meets the no-STL, no-third-party rule.
  - *Claude's suggestion (accepted):* changing a knob mid-run is logged as a timed event in the run record, so a replay reapplies it at the same tick and runs stay exactly reproducible.
  - *Until then:* Section 8's data side (stats, genebank, lineage, snapshots) is still built early; only the visual layer waits.

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

**Phase 2 — GPU (the first feature after proof of concept).**
- In-world GPU block ops (CPU-implemented) are enabled.
- Paired abiogenesis: GPU on versus off.
- Watch for the first lineage that claims GPU units.

**Phase 3 — The world.**
- A ~100-node graph from procedural generation (resource and climate noise fields), edges, the dispersal exchange and transit, many origins, and node properties.
- *Milestones:* first colonists, first dispersal strategies.

**Phase 4 — Add individually, each its own experiment:**
- volatility wipes plus the disk tier (repair from disk);
- the node lifecycle (wear, starvation collapse, creation/recovery as one mechanism);
- edge corruption and link failures;
- the ambient heat leak;
- the signal channel.

**Phase 5 — Analysis UI.** Native Win32 + GDI: biome map, population graphs, genome browser, phylogeny, and knobs that are logged as replayable events. (A minimal live view can come earlier if Phase 3 needs eyes on the map.)

**Phase 6 — Scale.** 1,000+ nodes, parallel worlds with island migration. Real-GPU batching of in-world GPU ops is an optimisation if profiling warrants it (confirm CUDA as a dependency first).

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
Diversity/complexity/novelty metrics; spark settings and other starting parameter values (tuned in the abiogenesis test). *Resolved 2026-11-05:* Section 6 (organisms); the energy/cost model (owned resources governed by heat; no abstract energy); the node creation rule, collapse consequences and recovery details (4.4a); signal channel design and the v1 resource tiers (5.1); technical direction, dependencies and analysis tooling (7.1); the phased build plan (9.1).
