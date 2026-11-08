# Plan: gestation, ray hazards and voluntary dormancy

> **Built 2026-11-08** (steps 1-5; design.md 6.24 says what changed in the build: V4's choice is a second switch, `life.sleep`, and W adds `mem.wipe_flicker`). Step 6's short sweep ran the same day (one seed, 50,000 ticks each), then Damian turned every feature on by default: rays (with R7's indels), gestation 25, wipe warnings 200, voluntary sleep beside the automatic rule (older files keep them off: legacy.h). The long comparison is still to do.
>
> **Revision 2, APPROVED by Damian (2026-11-08)**, with two answers changed from revision 1:
> - Volatility wipes stay wipes, but now come with warning symptoms (new section 3).
> - Dead matter shields too, and rays always start outside allocated space (R1, R3).
>
> Four world-rule changes, each behind keys, each off by default until measured. Source of truth: `docs/design.md` 6.2 (the instruction set), 6.6 (upkeep), 6.10 (hazards), 6.16 (Phase 10's decay and bites), 6.17 (dormancy), 6.22 (disasters); `docs/dormancy-plan.md` (today's dormancy).
>
> - **Why:** in long2 (the first all-features run, 01:48 on 2026-11-08), life settled on the shortest copier that works. The median genome went from 32 codons to 11, and genomes over 32 fell from about 3% of saves to 0.3%. Nothing durable passed 32 codons. The world rewards one thing, copying, and today the cheapest copier wins it.
> - **The ideas (Damian):** make copying take longer, so it stops being the default win. Make hazards act like radiation that a program can shield against with NOPs. Make dormancy something a program chooses, with a timer, rather than something done to it.
> - **Parked:** interrupts (hardware interfaces behind a call protocol). The NPU stays as it is.

## 0. What long2 showed (the numbers behind each change)

**Births**
- One birth costs about 18,700 plain-instruction units under `home_v1`, roughly 90 ticks at 10 millicores.
- ALLOC and DIVIDE cost about 2,050 units each. COPY costs about 21 a codon, so copying an 11-codon genome is about 1% of a birth.
- A costlier COPY would therefore mainly punish long genomes: a 64-codon copy would pay 6 times what an 11-codon one does.

**Upkeep** is 2 × L × (L/64)^(1/3) instructions a tick. That is about 12 for 11 codons and 128 for 64 codons. For a program holding 10 millicores (about 250 instructions a tick), 128 is half its budget.

**Hazards** per 1,000 ticks over the last 100 windows (about tick 600,000-700,000):

| Hazard | Codons per 1,000 ticks |
|---|---|
| Cosmic rays (bit rot, living and dead code) | 108,000 flips |
| Storm static | 5,900 flips |
| Lightning | 4 strikes, about 1,160 codons |
| Volatility wipes (bank flicker) | 1.19 million |
| Decay of waste toward INERT | 2.2 million bits |

**Dormancy** per 1,000 ticks:

| Outcome | Count |
|---|---|
| Programs going dormant | about 250,000 |
| Woke | about 225,000 (90%) |
| Expired | about 25,000 |
| Eaten | about 750 (0.3%) |

All of it is automatic: the watchdog sends frozen bodies to sleep.

**Saturation:** 91% of P millicores are in use, and 545 of 1,000 nodes are at least 90% full of living code. Small nodes copy less and attack more:
- Divides per profile sample are 0.12 on the smallest nodes against 0.30 on the largest.
- ENGULF tries per sample are 0.33 against 0.21.

## 1. Gestation (G)

**G1. The rule.** A daughter can be divided off only `life.gestation` ticks after the ALLOC that made her block (Body.latest).
- A DIVIDE issued before then waits. Its CPU (or warp) is idle until she is ripe, then the DIVIDE completes as today, through the timed-action machinery that disk and device ops already use.
- Each new ALLOC restarts the clock, because it makes a new latest block.
- Dispersal (DIVIDE to a neighbour) waits the same way.
- *Why wait rather than fail:* a failing DIVIDE would charge about 2,050 units each try and push programs into retry loops. A waiting DIVIDE costs nothing, and anything a program does between ALLOC and DIVIDE that fits inside the gestation is free, because the birth can't come sooner. That is a reason to carry more code: defending, gathering resources, repairing, sensing.
- *Recommendation:* wait.

**G2. Size.** The wait is `life.gestation` + `life.gestation_per_codon` × her length. The default per-codon term is 0.
- A flat wait flattens the advantage of being tiny. Today an 11-codon birth takes about 90 ticks and a 64-codon birth about 150 (1.7 times as long). With a 500-tick gestation they take about 590 and 650 (1.1 times).
- *Recommendation:* flat, with the per-codon term there to experiment with.

**G3. Where the clock lives.** The block's allocation tick goes into `Block.pad`, which is unused (always 0) today. It holds the tick's low 32 bits, which is enough for any run below 4 billion ticks.
- No snapshot layout change is needed. Blocks in older snapshots read 0, so they count as ripe.

**G4. Warps.** DIVIDE is a host instruction for GPU-form bodies, so the warp waits in front of it as it already does for a timed action (Phase 9b). The GPU kernel is unchanged.

**G5. A companion knob, not part of G.** `mem.upkeep_exp` (4/3 today) is how fast upkeep grows with size. A gestation run at 1.0 tests whether upkeep, rather than birth speed, is what keeps genomes short. It is config only.

## 2. Ray hazards (R)

**R1. A ray.** Cosmic rays, storm static and lightning become rays. Corpse decay, volatility wipes (which get warnings instead: section 3) and disk rot stay as they are.
- **Where it starts (Damian):** always outside allocated space.
  - A ray's point and direction are drawn as today's flips are (the same Poisson process over the soup, per node, from the node's stream).
  - If that point is inside a block (living code), the ray starts instead at the last unallocated codon before it, looking back against its direction. So it always enters a program from outside.
  - Dead matter (waste, INERT included) is not allocated space.
  - On a node with no unallocated codon behind the point, it starts at that end of the soup.
- It then travels codon by codon with energy E. At each codon:
  - It flips one random bit with chance `hazard.ray_flip`. NOP0 and NOP1 flip with the smaller chance `hazard.ray_flip_nop`.
  - It loses energy by what it passes through:

| It passes through | Energy lost | Key |
|---|---|---|
| Living code | The least | `hazard.ray_absorb` |
| Dead matter, INERT included ("still matter", Damian) | More | `hazard.ray_absorb_waste` |
| NOPs | The most | `hazard.ray_absorb_nop` |

- It stops when its energy is spent or it reaches the end of the soup.

**R2. Energy by source.** Each source has its own starting energy:

| Source | Key | Starting energy |
|---|---|---|
| Cosmic rays | `hazard.ray_cosmic` | Very high: long paths, the hardest to shield |
| Lightning | `disaster.strike_energy` | Medium |
| Storm static | `disaster.static_energy` | Low |

- Today a strike randomises a run of 64-512 codons outright. As a ray it randomises each codon it passes (rather than flipping a bit) and loses energy the same way, so NOP padding shortens the run.
- *Defaults, calibrated in step 2:* an unshielded run of code takes, on average, today's damage from each source. Otherwise the world's background mutation rate would change silently.
- The arrival rate of rays is today's flip rate divided by the mean number of flips an unshielded ray makes.

**R3. What a shield does.** Every ray enters a program from outside (R1), so a program padded with NOPs loses fewer codons to every ray, in proportion to its padding. The dead matter and living neighbours around it also take some of each ray's energy.
- A cosmic ray's high energy still gets through thin padding, so a shield gives resistance, not immunity.
- A program in a crowded stretch of memory is partly shielded by its neighbours' code, which takes the hits instead.
- Padding costs upkeep and copy time, which is the trade-off: shielding should pay where radiation is high (`vary.radiation`) and along storm tracks.
- NOPs are also labels (FIND, JMP, CALL) and INJECT surfaces, so padding changes how a program addresses its own code and how easily it is injected. That is expected, and it gets measured.

**R4. Heat.** Today a hot page's codons flip more often (the page's error multiplier, `mem.err_mult`). A ray keeps that: its flip chance at a codon is scaled by the page's multiplier.

**R5. Cost.** There are about 110 cosmic flips a tick world-wide today. A ray walks tens of codons, so this is microseconds a tick. No snapshot state is added: rays live within a tick.

**R6. Keys.**
- `hazard.rays`, a switch, off by default, which keeps today's flips.
- `hazard.ray_cosmic`, `hazard.ray_flip`, `hazard.ray_flip_nop`.
- `hazard.ray_absorb`, `hazard.ray_absorb_waste`, `hazard.ray_absorb_nop`.
- `disaster.strike_energy`, `disaster.static_energy`.

**R7. Indels (Damian, 2026-11-08, after the calibration sweep).** Bit flips only substitute; they never change a genome's length, and a copier's daughters inherit its length exactly (it copies its own size). So a cosmic ray's hit on living code, while the ray has `hazard.ray_indel_energy` (256) left, is with chance `hazard.ray_indel` (0.1) an indel instead of a flip, half each:
- an insertion copies the codon in place: the codons on one side move along one, and the block claims the free codon at that end;
- a deletion removes it: the codons on one side move back one, and the block lets that end's codon go, as heat does.
- Only in a block that owns its whole range, with a free codon at one of its ends (the end first, else the start; a deletion uses the same end), so neither kind is likelier in a crowded soup; else a flip. A one-codon block isn't shortened.
- A copier that copies its own size passes the change on: its daughters are a codon longer or shorter.
- Stats `ray_inserts` and `ray_deletes`.

**Rays on by default (Damian, 2026-11-08: "please stop defaulting new features to off. Turn rays on").** `hazard.rays` is true; snapshots before v6 and replay logs before v4 resume and replay with it off (legacy.h's second text).

## 3. Wipe warnings (W)

**W1. Why (Damian).** Wipes stay what they are: bank failures that turn whole pages to random codons. Today one strikes with no warning, which leaves nothing to respond to. They should give prior symptoms, as disasters do.

**W2. The warning.** A wipe is drawn as today (`mem.volatility` per page per tick) but lands `mem.wipe_warning` ticks later (default 0, meaning today's instant wipe). Over that warning time, the doomed pages show symptoms that ramp up. They use only quantities programs already read or suffer, as disaster symptoms do (`disaster.symptoms`), so no new SENSE reading is added (Damian, Phase 4):
- the pages' heat rises toward hot. SENSE already reads it, and hot pages already slow and fault the code fetched from them;
- the pages flicker: single-bit flips at a rate that rises toward the wipe.

**W3. False alarms.** A share `mem.wipe_false_alarms` of warnings fade without a wipe, as disasters' do. The default is the disasters' 0.25, so a symptom is a strong hint, not a certainty.

**W4. What it allows.** A program that senses the heat or suffers the flicker can:
- leave, by dividing or dispersing elsewhere;
- back its code up to disk and repair afterwards;
- go to sleep (V) and wake once the page has been rewritten.

All three already exist as instructions; the warning gives them a reason.

**W5. State.** Pending wipes (page, landing tick, false alarm or not) are world state: hashed and saved. This adds a section to snapshots, so the snapshot format goes to version 5. Version 4 files load with no wipes pending.

**W6. Keys.** `mem.wipe_warning` (ticks; 0 means instant, as today) and `mem.wipe_false_alarms`.

## 4. Voluntary dormancy (V)

**V1. The instruction.** YIELD with a sleep selector: BX = 1 (through the resource-style selector map) and CX = N > 0.
- The program goes dormant for N ticks, capped at `dormant.sleep_max` (default 5,000).
- BX = 2 does the same, but it drifts as a spore, as automatic dormancy does today.
- Any other BX (or CX = 0) is today's YIELD.
- *Why YIELD:* the 64-slot codon table stays as it is, and YIELD already means "stop for now".
- *Alternative:* a new codon in place of a little-used one. Not recommended.

**V2. Asleep.** As today's dormancy (`docs/dormancy-plan.md` D1):
- It leaves its cores to its node and keeps its code and its reserves.
- Its reserves wear by `dormant.mem_loss` / `dormant.disk_loss` a tick.
- With no reserve left it dies, so a program that sleeps without fuel dies. To sleep usefully it has to set aside memory first.
- BX = 1 rests where it is, and stays edible. BX = 2 drifts.

**V3. Waking.** When its timer runs out it wakes if a core of its kind is free, as today (D5). If none is free, it waits, still on its reserve clock, until one frees up or its reserves run out.
- It never wakes early. A wake-on-signal option can come later.

**V4. Automatic dormancy.** `life.dormancy` becomes a choice:

| Value | Meaning |
|---|---|
| `auto` | Today's rule (the default, so every hash holds) |
| `voluntary` | Only the instruction; frozen bodies die as before dormancy existed |
| `both` | The instruction and the automatic rule |
| `off` | No dormancy |

The experiment runs `voluntary`, so sleeping has to evolve.

**V5. Warps.** A warp lane running YIELD with BX = 1 or 2 and CX > 0 stops in front of it as a host instruction. The host then sends the whole body to sleep, with N from the first such lane.
- This changes `warp_isa.inc` (shared by the reference and both kernels). Every kernel is checked word for word against the reference, as for every ISA change.

**V6. State.** The timer goes into `Dormant.spare` (u16, unused today), so the cap is at most 65,535. A new flag `kDormantVoluntary` is added.
- No layout change: older snapshots read 0, meaning no timer.

**V7. Detection.** A `sleeper` milestone: the first voluntary sleep in a gated lineage. New stats columns: `sleeps`, `sleep_woke`, `sleep_expired`.

## 5. Determinism, snapshots, keys

- Every draw comes from the node's own stream, in today's fixed order. Hashes are identical at 1, 4 and 20 threads and through a snapshot, with each feature on and with all three on.
- Each feature is off by default:
  - `life.gestation` = 0
  - `hazard.rays` = false
  - `mem.wipe_warning` = 0
  - `life.dormancy` = `auto`

  So every existing hash, snapshot and replay log is unchanged. The one format change is W5's pending wipes (snapshot version 5).
- If a feature later becomes a default, its "off" line goes into `legacy.h`, and the snapshot and replay formats get the version bump that file describes.

## 6. Steps

1. **Gestation (G1-G4):** the rule, the clock in `Block.pad`, waiting DIVIDE on CPUs and warps.
   - Tests: a seeded copier divides exactly G ticks after ALLOC; a second ALLOC restarts the clock; a warp copier waits the same way; old snapshots load ripe; hashes with it off are unchanged.
2. **Rays (R1-R6):** the ray walk from outside allocated space, absorption by living code, waste and NOPs, per-source energies, strikes as rays.
   - Tests:
     - A ray that would start inside a block starts outside it.
     - With no NOPs, the mean damage per living codon from each source matches today's within 5% (the calibration).
     - A NOP-padded block, and a block in thick waste, takes measurably fewer hits.
     - Determinism.
3. **Wipe warnings (W1-W6):** delayed wipes, heat and flicker symptoms, false alarms, pending wipes in snapshots (version 5).
   - Tests:
     - A wipe lands exactly `mem.wipe_warning` ticks after it's drawn.
     - The symptoms ramp up on the doomed pages only.
     - The false-alarm share holds.
     - Version 4 snapshots load.
     - With `mem.wipe_warning` = 0 every hash is unchanged.
4. **Voluntary dormancy (V1-V7):** the YIELD selector, timer, waking, the `life.dormancy` choice, warps.
   - Tests: sleep N then wake; no-reserve sleep dies; drift variant; the warp path matches the reference; old snapshots.
5. **Docs:** `design.md` (new sections) and `keys.md` regenerated.
6. **Measures** (section 7).

Each step ends with the full suite green and a commit. No step changes long2: it runs on the build it started with.

## 7. Measures

**Short runs to calibrate** (the 1,000-node landscape, 50,000 ticks each):
- Gestation at 0, 100, 300 and 1,000 ticks.
- Rays on and off.
- Wipe warnings at 0, 50 and 200 ticks.
- `life.dormancy` = `auto` and `voluntary`.

**Long comparison** (300,000 ticks, the varied `landscape.cfg`):
- A baseline.
- The chosen combination.
- If gestation alone moves genome size, also gestation with `mem.upkeep_exp` = 1.0.

**What counts as success:**

| Measure | Today | Target |
|---|---|---|
| Median genome length | 11 | Rises |
| Share of saves over 32 codons | 0.3% | Above 3%, held |
| Ancestry chains of over-32 genomes | Short | Long |

Also tracked:
- NOP share in genomes on high-radiation nodes against low.
- Use of voluntary sleep: the milestone, then the share of lineages that keep it.
- Lifespan.
- ENGULF/INJECT rates.
- Lineage depth.
- Run speed.

**What counts as failure:** copiers just get slower and smaller, which would mean birth time wasn't what limited size.

## 8. Decided with Damian (2026-11-08)

1. **Gestation length:** sweep 100, 300 and 1,000 ticks in short runs, then pick.
2. **Before ripe:** a DIVIDE waits, it doesn't fail (G1).
3. **Sleep encoding:** YIELD with a selector (V1).
4. **Volatility wipes:** they stay wipes, not rays. They get prior symptoms (section 3): "as long as wipes have prior symptoms to indicate they are about to happen, then this is enough."
5. **Dead matter:** it shields too, since it "is still matter". Rays always start outside allocated space, and dead matter doesn't count as allocated (R1, R3).
6. **GPU-form bodies:** voluntary dormancy covers them from the start (V5).
7. **Not in this plan:** the NPU stays as it is, and interrupts are parked.
