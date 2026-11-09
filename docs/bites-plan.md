# Plan: bites as wounds, NOPs as tissue, parasites inside their hosts

> **Built 2026-11-09** (steps 1-7; design.md 6.26 says what the build settled: bleeding's schedule, how repairs are counted, the detector's per-CPU parasite tag). Step 8, the calibration sweep, is still to run. A finding: the bud-and-inject replicator works again through its daughters' open ends (the I1 risk).
>
> **Revision 3 (2026-11-08), APPROVED by Damian ("approved").** Changed from revision 2 (Damian):
> - An internal NOP that bleeding reaches becomes the new outer skin: the end is closed there (B6).
> - Tests: no fallback to today's rules. Every suite moves to the new rules; old run data is incompatible with the application after this (section 4).
> - Disasters, storms above all, made frequent enough to cause about a tenth of deaths (section 7).
>
> **Revision 2.** Revision 1's five choices were answered: B1, B2 (with a rounded-up third), B4 and I3 as recommended. Changed from revision 1 (Damian):
> - A wound stays open until the program repairs it with a NOP, not for a fixed time, to encourage self-repair (B5).
> - A program not sealed by NOPs slowly loses memory, like bleeding out (B6).
> - INJECT overwrites just inside the wound, not at an offset of its choosing (I2).
>
> Decided in conversation (Damian):
> - NOPs aren't immune to bites; they're tissue that a bite takes first, protecting the code inside, as skin protects organs.
> - A bite is a third of the biter's size, rounded up. A bite at least as big as its prey swallows it whole; a smaller one does damage as today.
> - INJECT can't get through a NOP. **Any program may use another's wound.**
> - An injected program gets the host's resources, bypassing the body barrier: true parasitism, with room for symbiosis.
> - **Injected code is passed down with the host's code**, as a virus's is to the host's children.
> - **No backward compatibility** (beta): no legacy tier for this; older snapshots and replay logs are refused.
>
> Source of truth for today's rules: `docs/design.md` 6.1 (private code, invasion), 6.4 #3, #6-9 (contact, overwrite), 6.6 (INJECT's key and room), 6.15 (actions, threads), 6.16 (bites), 6.17 (dormancy).

## 0. What long4 shows (tick ~420,000, last 20,000 ticks, per tick)

**Bites**
- 3,580 ENGULF tries; 1,360 land: 1,080 of waste, 280 of living bodies.
- The mean bite is 18 codons, against a typical body of 28: two thirds of it.
- 71 a tick swallow their prey whole (a quarter of body bites): 11% of all deaths.
- 55% of the codons eaten are fresh corpses and dormant programs (the cheaper bite).

**Surfaces and padding**
- About two thirds of saved replicator genotypes have a NOP surface, 40% three or more leading NOPs, about the same as long2 (rays off): the padding is bite and INJECT armour today, not ray armour.
- Trailing NOPs average about 0.5 a genotype; only about 10% end in two or more. **Under B6 most of today's programs would bleed from their tail.**
- The knockout scan of the 10 commonest long replicators: a third of their codons are needed to copy; 56% did nothing measurable in an empty world, 11% had decayed to INERT.

**INJECT** is a major way to reproduce today.
- 5,309 tries, 661 land; 308 of those overwrite a victim's whole first block so that it joins the injector's lineage.
- DIVIDE births: 660. So about a third of all inheritance is a body snatched by INJECT.
- Injected code never gets a thread of its own (threads started: 0); it runs only if the victim runs into it.

## 1. Bites, wounds and bleeding (B)

**B1. No immunity** (agreed). A blind bite takes the nearest material, waste or any body, surface or not. An aimed bite (a template after ENGULF) still picks the best-matching surface in range, with no grip roll: aiming chooses *which* body, tissue decides how much a bite hurts.

**B2. Size** (agreed). A bite is CX codons, at most `engulf.bite_share` (1/3) of the biter's own size, **rounded up**, so a 1-codon program can still eat another 1-codon program. CX stays the request: nibbles against full bites is the program's choice; the world never bites for it.
- A 28-codon biter takes at most 10 codons, so it swallows whole only prey of 10 codons or fewer.
- *Effect:* whole swallows of peers (11% of deaths) mostly end. ENGULF-whole composition (endosymbiosis) is left to big programs eating small ones.

**B3. Whole or piece.** Prey no bigger than the bite is swallowed whole, as today. A bigger body loses the bite from its end nearest the biter, as today.

**B4. NOPs as tissue** (agreed). The piece is taken whole, NOPs and all (they're food: memory for the biter). What the prey loses besides is set by its *code*, not its padding:
- its millicores, GPU and NPU units go in proportion to the non-NOP codons taken over its non-NOP codons (today: all codons over all codons);
- a thread running in the bitten code halts, as today; with no thread or no core left the prey dies, as today.
- So a bite that takes only padding costs the prey nothing but the padding.

**B5. Wounds stay open until repaired** (Damian). An *open end* is an end of a body's first block (its genome) whose outermost codon isn't a NOP. A bite that leaves code exposed makes one; so does a program that never had a NOP there.
- It closes when the outermost codon there is a NOP again. The program repairs it by writing a NOP over that codon with its own write head (COPY, or a write instruction), sacrificing whatever was there: a program with a little spare code near its ends can heal cheaply.
- No world rule heals anything: self-repair has to evolve (Damian).

**B6. Bleeding** (Damian). Each open end loses one codon every `life.bleed_every` ticks: the outermost codon is let go to waste, as a remnant (food for scavengers), until a NOP is outermost.
- Bleeding stops on its own at a NOP (Damian): once the codons outside an internal NOP have bled away, that NOP is the outermost codon, so it is the new outer skin and the end is closed: no more bleeding, and no INJECT through it.
- It takes memory only, not cores. A thread running in a bled codon halts, as with a bite.
- It applies to living bodies and to resting dormant ones (they're in memory). Drifting ones are in transit, not in memory.
- *Rate:* `life.bleed_every` = 100 ticks to start: about 3 codons per open end over today's 330-tick lifespan. Calibrated in the sweep (step 6).
- *Effect:* today most programs have an open tail, so the first pressure is to cap both ends. That should turn some of the 56% filler into tissue.

## 2. INJECT through wounds (I)

**I1. Only through an open end.** INJECT lands only on a body with an open end within contact range; anything else fails as a lost contest does today. A NOP blocks it.
- **Any program may use another's wound** (decided). Blind INJECT takes the nearest open end; an aimed one the best-matching surface among bodies with one.
- The key (`contact.inject_key`) goes: the wound is the way in. The room rule (host block ≥ payload) stays.
- *Risk:* on 2026-11-05 a bare `SENSE INJECT` in random code was a virus and chance INJECT chains met the replicator gate within seconds; the surface key was the fix. Random sparks have open ends, so INJECT gets in again. Bleeding shortens uncapped code, which works against it, but the sweep has to watch for INJECT-only lineages early on.

**I2. Just inside the wound, so inherited** (Damian). The payload overwrites the CX codons just inside the open end, in reading order: the last CX codons of the block for a tail wound, the first CX for a head wound. AX no longer places it.
- Because it's in the genome, the host's own copying passes it to its daughters (decided: "passed down like a virus"). A host whose copy loop stops short of it drops it; selection decides.
- Overwrite, not insert: there's rarely free memory beside a wound, and inserting would make the world claim memory for someone.
- A payload whose outer codon is a NOP seals the wound behind it, against bleeding and against the next injector.
- What it overwrites is the host's end: padding or spare code there absorbs it, code that matters is lost.

**I3. A thread of its own** (agreed). A landed INJECT starts a thread in the host body at the payload's first codon, with the injector's registers (as the GPU verb's spawn does). It runs on the host's cores, paid from the host's compute share, from the next tick.
- If the host already holds `cpu.max_threads`, the code is written but no thread starts: a latent infection that runs only if the host runs into it.
- A host daughter inherits the code but not the thread: her one thread starts at her start, as now. The payload wakes in her only if her flow reaches it, or a later INJECT restarts it.

**I4. The host's rights.** The parasite's thread is a thread of the host body, so under design 6.1's "one body, one set of rights" it can write anywhere in the host and ALLOC, COPY and DIVIDE with the host's memory and cores. That is the bypass (decided). Two lives follow without new rules:
- *Lytic:* the parasite ALLOCs from the host, copies its own code and DIVIDEs it off with the host's cores. Free-living copies.
- *Lysogenic:* it does little, rides in the genome, and spreads as the host breeds.
- The host can fight back the same way: its threads can overwrite the intruder (design 6.1: hosts police their parts). A parasite whose thread does useful work (copying faster, sealing wounds) is a symbiont.

**I5. Lineages.** Lineage tags are per block today, and the parasite lives in the host's block. So a thread started by INJECT carries the injector's lineage tag. A DIVIDE run by that thread links the daughter to the injector's lineage (a new link kind, `via parasite`), not the host's. The host's own daughters stay in the host's lineage.
- The old INJECT link (the victim's whole first block now matches the injector's) stays: a payload as big as the host still snatches its body.

## 3. Seeing it

- `stats.csv`: `wounds` (bites that opened an end), `open_ends` (gauge), `bled` (codons lost to bleeding), `repairs` (open ends closed by the program's own write), `inject_wounds` (INJECTs that landed through one), `parasite_threads` (started), `parasite_divides` (daughters made by parasite threads), `infected` (gauge: bodies with a running parasite thread).
- A milestone, `parasite`: the first gated lineage whose members made `detect.parasite` (3) daughters from inside hosts. Another, `healer`: the first gated lineage whose members closed `detect.repairs` (3) wounds.
- Profiles: a sample counts its parasite threads' instructions apart from its own (`foreign` already exists for code it runs outside its body).
- Viewer: open ends, bleeding and infected bodies marked in the memory strip.

## 4. Formats and tests

- No compatibility (decided): snapshot v8, replay v6; older files are refused with a clear message. Old run data is incompatible with the application after this (Damian). The existing legacy tiers aren't extended.
- **Tests move to the new rules** (Damian: no fallback; INJECT works as decided everywhere). The new keys don't go into the test runner's fallback, so every suite runs bites, wounds, bleeding and INJECT as section 1-2 say. That means:
  - *Seed genomes get NOP caps* where a test needs them to live past `life.bleed_every` (the hand-written ancestors in `tests/helpers.h` end in code at both ends, so they'd bleed).
  - *Behaviour tests written for today's rules are rewritten:* "a surface shields from a blind bite", "INJECT needs a key", the body-snatching detector, the bite-size and core-share checks.
  - *Pinned hashes are re-pinned:* about 6 tests compare a small world's end-state fingerprint with a recorded value (`test_gate`, `test_replay`, `test_rules`); each is checked to have moved for the expected reason, then recorded anew.
  - Determinism checks (the same hash at 1, 4 and 20 threads, and through a snapshot) need no re-pinning: they compare runs with each other.
  - A new suite `parasites` tests the new rules directly.

## 5. Steps

1. B1-B4: bites without immunity, the rounded-up third, cores by code share.
2. B5-B6: open ends, repair by writing a NOP, bleeding.
3. I1-I2: INJECT only through an open end, just inside it; the key goes.
4. I3-I5: the parasite thread, its rights, its lineage link.
5. Stats, the milestones, profiles, the viewer.
6. Section 7's disaster defaults and the `struck_bodies` count.
7. Tests moved to the new rules (section 4), suite `parasites`, verify, docs.
8. A short sweep against long4's settings (50,000 ticks): `life.bleed_every` at 50, 100 and 200, and the disaster rates at half, as set and double. Measured: bites and deaths by cause, disasters' share of deaths, open ends, repairs, infections, parasite divides, INJECT-only lineages, surfaces and padding, genome length. The defaults are set from it, then a long run.

## 6. Risks

- **A shock at the start.** Most of today's programs have an open tail, so bleeding hits almost everything until caps evolve. The sweep finds a rate that pushes without wiping life out.
- **Less predation.** Peer swallows end and body-snatching by INJECT mostly ends with them. The world may get more crowded and turn over more slowly until bite-then-inject strategies evolve.
- **INJECT viruses in random code** (I1's risk).
- **Runaway parasites.** A lytic parasite could sweep a node. Hosts can police, and dead hosts end their parasites, but a sweep is possible.
- **GPU-form hosts.** A parasite thread is a CPU thread; in a body with no millicores it can't run (latent).
- **Speed.** Bleeding checks each body's two ends; more threads per body means more instructions a tick.
- **Two big changes at once.** Bleeding and the disasters both raise deaths. The sweep varies them separately, so each one's effect can be read.

## 7. Disasters at about a tenth of deaths (Damian)

In long4 surges, crashes and burnouts together caused about 0.01% of deaths and lightning about 1% of the damage ordinary rot and copy errors do, so nothing was selected to avoid them. Damian: more storms, and disasters at about 10% of deaths, as in the hazard sketch of the same evening.

- **Storms alone can't get there without becoming node-wipers.** A strike randomises a run of memory, and a 32,768-codon node is about a third living code. At the sketch's storm settings, storms add roughly 1% of deaths; ten times as many strikes still gives only a few percent. In the sketch most of the 10% came from crashes (about 6%) and surges (about 3%).
- So the new defaults raise all three, storms the most visible. Starting values, from long4's per-event tolls (a crash kills about 53 programs at today's 10-60% share, a surge about 1,360 over 5 nodes) and today's death rate (about 650 a tick, so 10% is about 65):

| Setting | Today | New default (to calibrate) | Why |
|---|---|---|---|
| `disaster.storm_every` | 5,000 | 300 | about 20 storms at a time instead of 1-2 |
| `disaster.strike_every_min` / `_max` | 100 / 400 | 30 / 120 | a storm strikes every ~75 ticks |
| `disaster.strike_min` / `_max` | 64 / 512 | 512 / 2,048 | a strike takes out a few to a dozen programs |
| `disaster.crash_every` | 5,000 | 3 | each node every ~3,000 ticks; a crash deletes only *running* programs, so sleeping through one is an escape |
| `disaster.crash_share_min` / `_max` | 0.1 / 0.6 | 0.3 / 0.8 | |
| `disaster.crash_lead_min` / `_max` | 200 / 1,000 | 100 / 400 | the warning (instability faults) inside a lifetime |
| `disaster.surge_every` | 50,000 | 55 | each node every ~11,000 ticks; drifting out of the region is the escape |
| `disaster.surge_lead_min` / `_max` | 1,000 / 5,000 | 300 / 1,500 | brownout heat and hum |

- These are on by default (Damian's rule). Burnouts and disk failures are unchanged: burnouts send programs dormant automatically, so they can't select for a behaviour.
- The hazard sketch's other changes (longer lives through lower wear, rays that a few NOPs can stop) aren't part of this revision.
- **Measuring it:** crash, surge and burnout deaths are counted today; strikes aren't tied to the deaths they cause. A new count, `struck_bodies`, records each living body whose code a strike changed, so the sweep can read storms' share.
- **Per-node responses:** `nodes.csv` gains voluntary sleeps and drifts, and `stats.nodes_every` drops to 1 in the run config, so whether programs learn to sleep through crashes or flee surges can be lined up with the warnings.
