# Plan: natural disasters

> **Revision 6, for Damian's approval (2026-11-07).** A world rule change for the next run, behind keys. The design came from a conversation with Damian; his decisions are marked (Damian). The open details carry recommendations. All of these additions are Damian's:
> - revision 2 added symptoms before every disaster (X10);
> - revision 3 made lightning a storm that moves over an area (X6);
> - revision 4 gives each disaster its own sound in the signal field (X11);
> - revision 5 folds in the lineage tracker's fixes, found in the long run (X12);
> - revision 6 links lineages by descent rather than exact likeness (X12), and makes every disaster key a toggle or knob on the configuration page, set before a run begins (X8, `docs/config-plan.md`).
>
> - **Why:** rare, random disturbances, not triggered by success, the way fires, floods and storms shape real ecologies. They reset patches, cull at random, and reward resilience: dispersal, dormancy, backups. Starvation collapse, now removed, did the opposite: it hit the most successful nodes.
> - **What it builds on:** the node collapse and recovery machinery (6.10), dormancy and drift (6.17), and the disk tier (6.10).
> - **Source of truth:** `docs/design.md` 6.10, 6.16, 6.17.

## 0. Decided with Damian

- **Five disasters, all rare,** from harshest to mildest:
  1. **Power surge:** a region collapses.
  2. **OS crash:** a node reboots and loses some of its programs.
  3. **Burnout:** a node's cores go offline for repair, and their programs go dormant.
  4. **Disk failure:** a node's disk is lost.
  5. **Lightning:** a burst of random codons.
- **Surges** reuse collapse: the area is measured in hops, and they're the rarest event.
- **Burnt cores aren't destroyed.** They go through a repair period and come back to the free pool, so each node's resources ebb and flow.
- **Burnout victims** rest at home for their dwell, and drift if the cores aren't back in time. Programs without reserves get a mix of outcomes: some get a grace reserve, some die.
- **OS crash:** the node reboots briefly. A random 10–60% of its programs are deleted (ordinary deaths), and the survivors restart.
- **Disk failure** means the disk itself.
- **Lightning** mangles programs rather than killing them cleanly: most die, some mutate.
- **Every disaster can be detected beforehand, through environmental symptoms.** Simple life has no sense for them. Telling a symptom from ordinary noise takes complexity: a program has to evolve its own tools for it. That's a long-term goal (X10).

## 1. Design calls

**X1. When and where.**
- Each kind has a mean interval, world-wide, in ticks: a Poisson process drawn at each barrier.
- Each event picks a random live node from a world stream keyed by its kind and the tick, as link failures already do. So the events are the same at any thread count and in any dispatch order.
- Effects are applied at the barrier, one event at a time, in a fixed order of kinds.
- *Defaults for a 1,000-node world:*

  | disaster | `disaster.*_every` | times a day, at ~16 ticks a second |
  |---|---:|---:|
  | power surge | 50,000 | 28 |
  | OS crash | 5,000 | 280 |
  | burnout | 2,000 | 700 |
  | disk failure | 10,000 | 140 |
  | lightning storm (forming) | 5,000 | 280 storms, about one or two out at a time |

- An interval of 0 turns that kind off.
- *Recommendation:* as above, world-wide intervals rather than per node, so "how often somewhere" is what the key says. Tune after the first run.

**X2. Power surge (Damian: regional, by hops, rarest).**
- The epicentre is a random live node. Every live node within `disaster.surge_hops` hops along paths (default 1–2, drawn per event) collapses at that barrier through the normal collapse: every body dies an ordinary death, the code stays as ruins, the node goes fallow, and cargo on its paths is dropped.
- The nodes come back through the normal recovery rule: fallow for `node.fallow_min`, then returning every `node.recover_every` ticks while under the budget.
- Dormant programs drifting through at that moment survive and can recolonise. Resting ones die with the node.
- *Recommendation:* as above.

**X3. OS crash (Damian, as discussed).**
- One node reboots for `disaster.reboot` ticks (drawn from 20–100). During that time nothing on it runs, as on a fallow node, but its state is kept. It accepts no arrivals: daughters sent there are lost, and dormant drifters pass through.
- On reboot, a random share (drawn from 10–60%) of its programs die ordinary deaths: remnants left, scavengeable, reanimatable.
- The survivors restart: each thread back at its body's start with registers cleared; warps end and are relaunched by their programs; actions in progress are dropped.
- *Recommendation:* as above.

**X4. Burnout (Damian: recoverable, programs go dormant).**
- On one node, programs are picked at random until about `disaster.burnout_share` of its cores (drawn from 10–40%) are taken, millicores and GPU units together.
- Every core those programs hold goes offline for a repair time (drawn from 200–2,000 ticks). Offline cores belong to no one and aren't in the free pool.
- When repaired, they come back to the free pool, cool, at the barrier.
- The programs go dormant as a frozen one does, with two differences:
  - They rest at home for their dwell first. If cores of their kind come free in time, from repairs or deaths, they wake there; otherwise they drift as usual.
  - A program with no reserves (Damian: a mix) gets a grace reserve with chance `disaster.grace_chance` (default 0.5); the rest die. The grace reserve is `disaster.grace` codons of memory reserve (default 32, about 32 ticks at the default clock).
- GPU-born programs caught in a GPU burnout wait for GPU units.
- Why whole programs rather than single cores: a dormant program can't hold cores. If only some of its cores burnt and it kept the rest, those would go back to the pool and it would wake on them at once.
- *Recommendation:* as above.

**X5. Disk failure (Damian: the disk).**
- One node's disk loses everything. Every codon becomes INERT (fossils included), and every program's disk holding is released.
- Resting dormant programs there lose their disk reserve.
- Nobody dies directly. Strategies that store data, or keep disk as a dormancy reserve, take the hit.
- *With the disk tiers (`docs/world-plan.md` W4):* a failure takes one tier, chosen by size, so it's usually the fast disk. An archive failure is rarer but possible, so near-permanent never means permanent.
- *Recommendation:* as above.

**X6. Lightning storms (Damian: like a real storm, it moves slowly over an area; mangles rather than kills).**
- *Forming:* a storm forms over a random live node, with a lifetime (drawn from 2,000–10,000 ticks) and a heading (a random direction on the map).
- *Moving:* every so often (drawn from 200–800 ticks per move) it moves to a neighbour along a path. It picks the neighbour whose direction on the map best matches its heading, and the heading drifts a little each move (a random turn of up to about 30°). So a storm travels in a rough line and wanders, crossing a landmass rather than hopping at random.
  - It can't move off a hermit or into a fallow node. With nowhere to go it stays put.
  - It dies out at the end of its lifetime.
- *Striking:* while over a node it strikes now and then (a Poisson process, about one strike per 100–400 ticks). Each strike randomises a run of codons (drawn from 64–512) at a random place in memory.
  - Ownership stays as it is, so programs caught in it carry mangled code: most fail, some mutate and live. Nothing is killed directly.
- *More than one storm* can be out at once: they form independently and pass each other.
- *Recommendation:* as above. Storm state goes into snapshots (X9), and the viewer's map shows each storm and its track (for us).

**X7. Seeing it.**
- *Events* in the replay log and console:
  - `surge node=… hops=… nodes=… deaths=…`
  - `crash node=… deleted=… restarted=…`
  - `burnout node=… cores=… dormant=… graced=… died=…`
  - `diskfail node=…`
  - `storm formed node=… heading=…`, `storm moved from=… to=…`, `storm ended node=…`, `lightning node=… codons=…`
- *Stats columns:* per window, `surges`, `crashes`, `burnouts`, `disk_failures` and `strikes`, plus the programs each killed or sent dormant, and a gauge of `cores_offline`.
- *The viewer:* a map marker on nodes struck in the last few thousand ticks, coloured by kind.
- *Recommendation:* events and stats in this plan; the map marker if it's cheap, else a follow-up.

**X8. Keys and defaults.**
- `disaster.enabled` is the master switch, plus each kind's switch, interval and size range. Symptoms (X10) and sounds (X11) have switches of their own.
- On by default, per your standing rule that new features are on by default. Off in the test suites' legacy defaults, so every existing hash holds.
- *On the configuration page (Damian):* every one of these is a checkbox or a knob in a Disasters group (`docs/config-plan.md`). They're set before a run begins and fixed for the whole run.
- *Recommendation:* as above.

**X9. Snapshots and determinism.**
- New state, in snapshot format v4 (v3 and older still load, with no disasters pending):
  - each node's reboot end;
  - each offline core with its repair tick;
  - the dormant programs' grace flags;
  - each storm: node, heading, lifetime left, next move (tick and destination), next strike;
  - each symptom episode: kind, where, start, end, and whether it's a false alarm.
- Every draw comes from a keyed world stream. Every effect is applied at the barrier in a fixed order.
- *Recommendation:* as above.

**X10. Symptoms before every disaster (Damian).**
- *The principle:* no new sense. A disaster's warning shows only as a change in something the world already has, and programs already observe or suffer: heat, errors, bit flips, disk decay, signal noise.
- *The ramp:* the change is local to the area about to be hit, starts weak and grows toward the event over a lead time, and sits on top of the normal fluctuations.
  - A single reading tells a program nothing.
  - Telling a symptom from noise takes memory (past readings, kept in its own memory or on disk), timing, comparison, and for the best warnings, agreement with neighbours over signals.
  - Simple programs just suffer the symptoms; detecting them is a tool life has to evolve.
- *The symptoms:*

  | disaster | its symptom | lead time | where a program could notice it |
  |---|---|---:|---|
  | power surge | brownouts across the region: cores run hotter (an extra ambient heat input, rising), and the signal noise floor rises | 1,000–5,000 ticks | its cores' heat (SENSE 3), its page's heat (4); LISTEN reading noise; neighbours reporting the same over signals |
  | OS crash | instability: spurious instruction faults and stray halts become more frequent on the node | 200–1,000 | its own error register and IFERR; neighbours on the node dying faster (free millicores, SENSE 5) |
  | burnout | the cores about to burn run a fever: their heat climbs above what their work explains | 200–1,000 | its cores' heat (SENSE 3; GPU units' heat, 9) against its own work |
  | disk failure | the disk decays faster: rot on that node's disk climbs | 500–2,000 | DISK compare against data it stored |
  | lightning storm | static: single-bit flips in memory become more frequent, and the signal noise floor rises. It's strongest under the storm, and builds on the nodes ahead of it (the neighbours along its heading) as it approaches | while it travels: a node ahead feels the static grow for a move or two (several hundred ticks) before the storm arrives | its own data or code changing (a compare), copy errors; LISTEN reading noise; neighbours already under the storm reporting over signals, along a direction that persists |

  - The victims are fixed when the symptoms start: the region, the node, the cores, the disk. So the warning is honest: the cores running a fever are the ones that will burn.
  - A storm's warning is honest the same way: the static builds on the node it will move to next (its next move is decided ahead of time), and a storm's steady heading makes its path something a tool can learn to predict.
- *False alarms:* `disaster.false_alarms` of symptom episodes (recommended 0.25) fade without a disaster, as real weather does. A tool then has to weigh evidence rather than react to any symptom.
- *Why it's worth evolving:* a warning only pays if there's something to do with it. Today a program can send its daughters away (DIVIDE to a neighbour), keep copies on disk, or change what it does. Choosing to go dormant would be the natural next response, but for now dormancy stays automatic (Damian), so this plan adds no new instruction.
- *Watching for it (the long-term goal):* per symptom episode, `stats.csv` and the event log record where and when it ran and whether it ended in a disaster. That lets an analysis compare behaviour inside episodes with outside: dispersal out of the area, deaths, DISK compares, signals. It also lets us look for lineages whose code reacts. The viewer's map could shade nodes in an episode, for us, not for the programs.
- *Recommendation:* as above, with symptom strength set so that at its peak it's well above normal variation (easy to detect late, with little time left) and early on it's within it (detecting early takes good tools).

**X11. Sounds (Damian: some lifeforms use sound).**
- *The idea:* disasters make sounds in the signal field programs already use. EMIT and LISTEN (6.10) work on shared channels, fade with each hop (`signal.attenuation`), are delayed by path latency, and sit under each node's noise floor. The world emits into it as a program would, so a disaster's sound arrives at nearby nodes faded and late, like any signal.
- *No new instruction:* a program hears a sound only by LISTENing on the right channel at the right time. Programs that use signals to talk to each other will also hear disasters on their channels as interference. That's a pressure to pick quieter channels, or to learn what the noise means.
- *Each disaster has its own voice:* a band of channels and a pattern over time.

  | disaster | its sound | when | how far |
  |---|---|---|---|
  | power surge | a hum that rises through the brownout, then a bang at the surge | through its symptom episode, loudest at the end | the region and beyond: a strong source fades over several hops |
  | OS crash | stuttering clicks during the instability, then silence while it reboots (its programs fall silent too) | symptoms, then the reboot | the node and its neighbours |
  | burnout | a crackle from the fevered cores, rising | its symptom episode | the node, faintly its neighbours |
  | disk failure | a grinding: a regular pulse that speeds up | its symptom episode | the node |
  | lightning storm | thunder: a loud burst at each strike, plus a rumble wherever the storm is | while the storm lives | far: thunder carries further than static, so nodes several hops ahead hear it coming before any static reaches them |

- *Honest, and part of the warning:* each sound is tied to its disaster's symptom episode or event. False alarms make their sounds too and then fade, as a storm can rumble and pass. So hearing is one more piece of evidence a tool can weigh, alongside heat, errors and static.
- *Different senses, different reach:*
  - Sound reaches further and arrives sooner than the local symptoms, but it's shared and noisy.
  - Local symptoms are certain but late.
  - A program that both listens and watches its own heat or errors has the best warning.
- *Needs signals on:* `signal.channels` above 0 (16 in the presets). Without signals, disasters are silent.
- *Seeing it:* the stats and event log record each sound's source (the disaster) and loudness, so we can tell environmental sounds from programs' own when analysing who reacts.
- *Recommendation:* as above, with each kind's band and loudness set at build time from the channels' normal traffic in the first run, so sounds stand out near their source and fade into the noise a few hops away.

**X12. The lineage tracker's fixes (Damian: fold them in).**
- *Found in `runs\long1`:* two faults in the lineage tracker. It only observes the world, so neither touches the hash, births or deaths. They do distort the lineage figures, and the disasters' analysis depends on those figures.
- **The dormancy leak:**
  - *What happens:*
    - A dormant program with no open path (every path from its node down, or a hermit node) rests at home, and its block stays counted in its lineage.
    - When it wakes there, `on_arrive` counts it a second time without removing the first count.
    - Each home wake adds a phantom member, so the lineage never reaches zero and never ends.
  - *What it does to the stats:* `top_lineage` has sat at 4,533 since tick 350,000, on a node holding about 200 bodies, for a lineage with no members left (`genotypes_in_top` 0).
  - *The fix:* a program leaves its lineage when it goes dormant, wherever it rests, and rejoins on waking, as an arrival does. Burnout (X4) needs this, because its victims rest at home first.
- **Copies the tracker misses (descent, not likeness; Damian agreed):**
  - *What happens:* the tracker compares a daughter with her parent codon by codon from the start, against the longer of the two. So it misses three kinds of copy, and each starts a new lineage at depth 0:
    - a partial copy (half a genome);
    - a copy shifted by a codon or two, from a head off by one;
    - a rotation, a copy that starts part-way through the parent's code.
  - *What it does to the stats:*
    - Lineage founders are mostly missed copies, not new life. Between ticks 100,000 and 250,000, spark-born bodies can be at most about 22% of founders. Sparks give every length from 8 to 64 equally, but founders peak at 32 codons (20%), 31 (7%) and 16 (7%) and fall to 0.4% per length above 45.
    - The 11-codon ring replicator copies itself by rotation. The genebank records its rotations with no parent, `lineages` has inflated (24,000 → 71,000), `max_depth` has stuck at 305, and `replicator` events fire again for each rotation.
    - So life is far more lineage-based than the figures say.
  - *The fix:* a daughter joins her parent's lineage when at least `detect.similarity` (0.75) of her own codons were written by her parent's body (`detect.written`, as now) and match the parent at the best alignment. The share is measured against her length, not the longer of the two, so a partial copy counts as a descendant.
  - *Alignments tried:*
    - the straight comparison first;
    - then shifts of up to `detect.shift` (8) codons either way;
    - then rotations, where only places where her first 4 codons appear in the parent are candidates, at most 8 tried.
  - *Unchanged:* budding onto someone else's remains still doesn't count, because those codons weren't written by the parent.
  - *Recommendation:* as above. The cost (a few hundred comparisons per birth, outside world time) is checked in the measured run.
- *Snapshots and the CSV:* the lineage tracker isn't saved in snapshots, and the column names stay as they are, so neither the snapshot format nor the CSV changes.

## 2. Steps
0. **The lineage tracker's fixes** (X12), before burnout relies on them.
1. **The schedule (X1), the events and stats (X7), the keys and snapshots (X8, X9).**
2. **Power surge and OS crash** (X2, X3).
3. **Burnout** (X4).
4. **Disk failure and lightning** (X5, X6).
5. **Symptoms** (X10): each disaster's precursor episode, the false alarms, their record in the stats and log.
6. **Sounds** (X11): each disaster's voice in the signal field.
7. **Tests,** then a measured run from the late snapshot with disasters on and off (deaths by cause, recolonisation after surges, dormant programs waking after burnouts), design.md and the README.

## 3. Tests
- `disaster.enabled = false` is today's world exactly: the same hashes in every suite.
- Each disaster on a small world at high rates:
  - a surge collapses the epicentre and its neighbours within the hops, and they recover;
  - a crash halts a node, deletes about the drawn share, and restarts the rest;
  - a burnout takes whole programs' cores offline, makes them dormant (resting first, some graced), and returns the cores after repair;
  - a disk failure leaves INERT and no holdings;
  - a storm moves along paths with a persistent heading, strikes only where it is, randomises the drawn runs, and dies out at the end of its lifetime;
  - each symptom raises its quantity on the right nodes (or cores, or disk) over its lead time, before its disaster, and false alarms fade with none;
  - each sound arrives on its channels at the source at full strength, and fainter and later a hop away; disasters are silent without signals.
- The lineage tracker (X12):
  - a dormant program that rests and wakes at home leaves its lineage's count where it was, and the lineage ends when its last member dies;
  - a daughter that is a rotation, a shift or a partial copy of her parent joins the parent's lineage one level deeper;
  - an unrelated block, or one the parent didn't write, still doesn't;
  - every existing suite keeps its hashes. Suites that count detector events get their expected counts updated, with the reason noted.
- Determinism: a busy dispersal world with every disaster frequent gives the same hash at 1, 4 and 20 threads, under shuffled dispatch, and through a snapshot taken during a reboot with cores offline.

## 4. Risks
- **Too frequent, and the world never settles.** The defaults are guesses scaled to about a day's run. The first run's numbers tell us where to set them.
- **A surge right after another** can wipe a whole landmass; recovery and dormant drifters are the way back. If needed, a cooldown between surges.
- **Symptoms too strong or too weak.** Too strong, and even simple code reacts by accident, so no tool is needed. Too weak, and nothing can learn them. The first run measures each symptom against normal variation, and the strength is a knob per kind.
- **Burnout's grace** keeps some bare programs alive a little longer than the frozen rule would. That's intended (the hardware failed, not the program), but it's a knob if it lets weak programs linger.

## Result (2026-11-07)

Implemented all five disasters, symptoms and sounds, descent-based tracking and founder origins. Snapshot and replay compatibility, deterministic dispatch, and combined-feature tests cover the integration. The disk warning's tier choice and the four-codon descent floor remain open for Damian. See design sections 6.19 and 6.22.
