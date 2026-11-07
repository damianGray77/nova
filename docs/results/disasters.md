# Results: natural disasters (disasters plan X1–X11)

> Built 2026-11-07 on branch `wt/disasters` from `docs/disasters-plan.md` revision 6 (with `docs/world-plan.md` revision 2 and `docs/config-plan.md` revision 1), steps 1–6 of the plan. X12 (the lineage tracker's fixes) is another branch's. Uncommitted, for the merge.

## What was built

Five disasters, each with symptoms before it and a sound in the signal field, all behind `disaster.*` keys (66 of them, in the Disasters group). They're on in the engine defaults and off in the test runner's legacy defaults (`disaster.enabled = false` in `Config::set_fallback`), so every existing suite keeps its hashes.

- **X1, the schedule.** Each kind has a world-wide mean interval (`disaster.<kind>_every`, 0 = never) and its own switch. At each barrier a Poisson count is drawn per kind from a world stream keyed by the kind and the tick, and each event picks a random live node from the same stream. Effects are applied at the barrier, one event at a time, in a fixed order:
  1. repairs of burnt cores;
  2. each kind in turn (surge, crash, burnout, disk failure): its due episodes strike or fade, in the order they began, then its new ones begin;
  3. the storms (strikes, ends, moves, then new storms);
  4. the sounds;
  5. a surge's collapses, through the lifecycle, which runs next.
- **X2, power surge.** The epicentre is a random live node. Its region is every live node within 1–2 hops (drawn per surge) along paths, breadth first. At the surge, every live node of the region is marked to collapse, and the lifecycle collapses it at the same barrier through the normal collapse: ordinary deaths, ruins, fallow, cargo dropped, the normal recovery. Resting dormant programs die with their node; drifting ones survive unless they're on the region's paths. Surges need the lifecycle, so a map world (procedural or landscape) with surges on has one even without wear or drifting land. A ring world gets no surges.
- **X3, OS crash.** A random 10–60% of the node's running programs are deleted (ordinary deaths, remnants left). The rest restart:
  - each thread goes back to its body's start with every register cleared;
  - launched warps end, and a GPU-born program gets a fresh warp at its start;
  - every action in progress is dropped (counted as failed);
  - async results still due to a restarted thread are ignored.

  The node then reboots for 20–100 ticks. It's idle like a fallow node with its state kept: daughters arriving are lost (`lost`), dormant drifters pass through, and its pending signals are cleared unheard. The survivors count as running from the reboot's first tick, so the watchdog doesn't take the reboot for a freeze.
- **X4, burnout.** Programs are picked at random until they hold 10–40% (drawn) of the node's cores, millicores and GPU units together. Their cores are the ones that will burn. At the burnout:
  - the holders of those cores at that moment are the victims, and every core they hold goes offline for 200–2,000 ticks (one draw per burnout);
  - the burnt cores that are still free go offline too;
  - offline cores belong to no one and aren't in any pool (`Soup::offline_`). At the barrier where their repair is done they come back to their pools with heat 0.
  - Each victim goes dormant through the existing path (`go_dormant`, burnout mode). It rests at home for `dormant.dwell` and then drifts as usual through `rest_tick`, waking if cores of its kind come free.
  - A victim with no reserve gets a grace reserve of `disaster.grace` (32) codons with chance 0.5; the rest die, and their cores burn all the same.
  - NPU units go back to their pool, as a dormant program's always do.
  - The dormant state carries `Dormant::flags` (burnt, graced; the struct's old padding, so v3 snapshots still read). *(At the integration the disk tiers kept that padding word for `archive_reserve`, and the flags moved to the byte after `kind`, which was always 0: `docs/results/integration.md`.)*
- **X5, disk failure** (today's single disk). Everything goes through one function, `Soup::fail_disk()`: every holding is released, every disk codon becomes 0 (INERT under `nova_v2`) with no stamp, and resting dormant programs lose their disk reserves. Nobody dies directly. At the merge with the disk tiers (W4), this is the one spot where a tier is picked by size, through `fail_disk_tier(archive)`; the strike already has its own keyed stream (`dis_strike`) to draw the pick from. *(Done at the integration: `fail_disk(r, &archive)` takes the archive with chance archive / (disk + archive) and calls `fail_disk_tier`; the event gains ` tier=fast|archive` where a node has both: `docs/results/integration.md`.)*
- **X6, lightning storms.** A storm forms over a random live node with a lifetime (2,000–10,000), a heading (a random direction), and a mean strike interval (100–400).
  - *Moving:* it moves every 200–800 ticks along a path to the live neighbour whose map direction best matches its heading (ties to the nearer). Its heading then turns by up to ±30°, and its next move (tick and destination) is decided at once, so the static ahead is honest. With nowhere to go it stays put, and without a map it moves to a random neighbour.
  - *Striking:* strikes are a Poisson process where it is. Each randomises 64–512 codons at a random place in memory; ownership stays, stamps are cleared, and the detector is told (`on_wipe`).
  - Each storm draws from its own stream, keyed by its serial and continued from a saved counter.
- **X7, seeing it.** Events go to the replay log and console. They follow the plan's formats, with extra fields appended:
  - `surge node= hops= nodes= deaths= bang=`
  - `crash node= deleted= restarted= reboot=`
  - `burnout node= cores= dormant= graced= died= repair=`
  - `diskfail node= codons=`
  - `storm formed node= heading= dry= band= loud=`
  - `storm moved from= to=`
  - `storm ended node= moves=`
  - `lightning node= codons= thunder=`
  - `symptom kind= node= start= until= false= [hops= nodes=] [cores=] band= loud=`
  - `symptom_end kind= node= start= false= struck=`

  `stats.csv` gains 26 columns after `ambient`:
  - sums: `surges`, `surge_nodes`, `surge_deaths`, `crashes`, `crash_deaths`, `crash_restarts`, `burnouts`, `cores_burnt`, `burnout_dormant`, `burnout_graced`, `burnout_deaths`, `disk_failures`, `disk_lost`, `storms_formed`, `strikes`, `strike_codons`, `episodes`, `false_alarms`, `spurious_faults`, `static_flips`, `sounds`, `sound_amp`;
  - gauges: `cores_offline`, `rebooting`, `storms`, `episodes_active`.

  The per-node file gains `symptom_ticks`, `struck` and `cores_offline`. The signal tap gains `sound_amp`, the world's own sound per channel, so environmental sound can be told from programs'. `nova resume` prints two summary lines (each kind's counts, the symptom episodes and false alarms, and what's offline, rebooting or brewing now).
- **X8, keys.** Each kind has a switch, an interval and its size and lead ranges. The symptoms and the sounds each have a switch and strengths. Every key is in `core/src/keys.inc` and `docs/keys.md`, marked structural (none is a live knob). Min and max pairs are cross-checked.
- **X9, snapshots.** The format is now v4. With disasters on it adds each soup's offline cores and the world's state: every node's reboot end, the symptom episodes with their regions and cores, the storms, and the serial counter. v3 and older load with nothing pending. A map world whose lifecycle now exists only for surges starts that lifecycle afresh when an old snapshot loads. Everything is hashed whenever disasters are on.
- **X10, symptoms.** With symptoms on, each disaster begins as an episode over a lead time, and its victims are fixed at the start: the region, the node, the cores, the disk. Before each tick the world sets each node's weather (`Soup::Weather`, derived, never saved) from the episodes and storms. The weather acts only through quantities programs already read or suffer, with no new SENSE:

  | disaster | symptom | lead (ticks) | what it changes |
  |---|---|---:|---|
  | surge | brownout | 1,000–5,000 | extra heat input on every unit and page of the region (`ambient` rises too); signal noise up to (1 + 4)× the floor |
  | crash | instability | 200–1,000 | spurious instruction faults (error flag, IFERR) on the node's CPU threads; 10% of them halt the thread |
  | burnout | fever | 200–1,000 | extra heat on exactly the cores that will burn |
  | disk failure | decay | 500–2,000 | extra disk rot on the node |
  | storm | static | while it lives | bit flips anywhere in memory and noise, full under the storm, building linearly on its next destination as the move approaches |

  - Strength rises as the square of the elapsed share of the lead: 1% of peak at 10% of the lead, 25% at half, 100% at the strike.
  - **False alarms** (`disaster.false_alarms`, 0.25) rise exactly as a real episode of the same lead does, to a peak at 30–100% of the lead, then fade linearly in half the time they took to rise. Early on a false alarm looks like a real one; strong symptoms are more often real.
  - Episodes begin at 1/(1 − false_alarms) times the disaster rate, so disasters still come at the interval the key says.
  - A storm's false alarm is a *dry* storm: static and rumble, no lightning.
  - Each episode is logged twice: `symptom` at its start and `symptom_end` with `struck`. Each node has `symptom_ticks` in the per-node file.
- **X11, sounds.** The world emits into the signal field as a program's EMIT does: scattered at the barrier along the signal paths, faded per hop, delayed by latency, and dropped under each target's floor. A sound reaches its source at full strength the next tick, and its neighbours fainter and later. Each kind has its own band and pattern:

  | disaster | band (first, width) | pattern | loudness |
  |---|---|---|---:|
  | surge | 13, 3 | a hum from every region node, alternating in sign each tick, rising with the brownout; a bang from the epicentre at the surge | 256 at peak; bang 4,096 |
  | crash | 8, 2 | stuttering clicks, each tick with chance = the symptom's strength | 64 |
  | burnout | 6, 2 | a crackle, random in size (0.5–1.5×) and sign, as often as the clicks | 32 |
  | disk failure | 5, 1 | a grinding: a regular pulse whose period shortens from 32 ticks to 4 as the decay grows | 16 |
  | storm | 10, 3 | a rumble, random each tick, wherever a storm is; thunder at each strike | rumble up to 48; thunder 2,048 |

  - False alarms sound too, and fade with their symptoms.
  - Without signals (`signal.channels` 0) the disasters are silent.

## Chosen values (the plan's "set at build time")

- **Symptom strengths** come from the heat model: `heat.cool` is 0.1, so an extra input of x a tick settles at about 10x more heat. A busy core sits near 60, which is hot (60) and past warm (40).
  - *Brownout* 1.0 a tick: +10 at the peak, enough to push busy units into throttling and pages up a band. At 10% of the lead it's +0.1, lost in the noise. Measured on a busy core it's +5, since throttled units do less work.
  - *Fever* 2.0 a tick: +18 at the peak, on the doomed cores only, so a program comparing SENSE 3 with its own work sees heat its work doesn't explain. Measured on a busy core it's +8 above the same program's cores under the brownout.
  - *Instability:* 5 × 10⁻⁴ spurious faults per instruction at the peak, five times the flaw rate, with 10% of them halting their thread. Neighbours die faster (free millicores, SENSE 5).
  - *Failing disk:* 2 × 10⁻⁴ rot per codon per tick at the peak, against `disk.rot` 10⁻⁸. A stored 64-codon block sees about one flip in 80 ticks at the end, and almost none early. It takes repeated DISK compares to notice.
  - *Static:* 10⁻⁴ flips per codon per tick under a storm, against cosmic rays of about 10⁻⁷. A 32-codon program under a storm sees about one flip in 300 ticks.
  - *Noise* 4: the uniform noise on every channel grows from ±floor to ±5 × floor at the peak.
- **Bands** come from EMIT's channel being BX itself. Random code's registers are mostly small, so programs' traffic sits in the low channels, and the disasters take channels 5–15 of the default 16, leaving 0–4 to programs. Bands wrap modulo `signal.channels`.
- **Loudness** comes from `signal.attenuation` 0.25 and `signal.floor` 1, where a contribution under 1 is dropped and the noise is ±1. A sound of amplitude A is A/4ʰ at h hops, so each kind's loudness sets its reach:
  - grinding (16): 4 at one hop, dropped at two;
  - crackle (about 32): 8 at one hop, faint;
  - clicks (64): 16 at one hop, 4 at two;
  - hum (256 at peak, from every region node): 4 at three hops beyond the region;
  - thunder (2,048): audible to five hops (2 at five), so nodes ahead of a storm hear it before any static reaches them;
  - bang (4,096): audible to six hops.

  These are first values, chosen from the settings. The plan's measurement of real channel traffic in the first long run should retune them (all are keys).
- **A check of the bands on a preset.** `configs/world.cfg` was run at 24 nodes on every engine default, with disasters 25–125× more frequent than the defaults, for 3,000 ticks (signal tap on). Programs' EMITs went:
  - channel 0: 43,651;
  - channels 1–4: 2,713, 609, 391 and 260;
  - channels 5–15: 31–226 each, about 0.003 per node a tick.

  Random code's amplitudes are huge (|CX| up to 2³¹; the mean |field| on channel 0 was about 3 × 10⁷), but rare on the disaster bands, where the mean |field| was 9 on a quiet one (channel 8). The disasters' own sound there, by band: storm 1.16 × 10⁶ |amplitude| per channel, surge 4.0 × 10⁶, crash about 1.3 × 10⁵, burnout about 9 × 10⁴, disk 2.4 × 10⁴. So the bands sit away from programs' traffic, and the only interference is programs' rare large bursts.

## Measured (test worlds, Debug build)

The `disasters` suite's worlds are the 24-node procedural `kSoupWorldProc` (dispersal, seeded dispersers, legacy rules otherwise) at high rates. Counts are as the suite prints them:

| test world (rates) | what happened |
|---|---|
| surges only (every 60, symptoms off), 600 ticks | 6 surges, each collapsing exactly its region (checked against the graph before it), 55 recoveries, all 24 nodes live again at the end |
| crashes only (every 30, reboot 10–30), 500 ticks | 12 crashes: 184 programs deleted, 323 restarted; 226 node-ticks down, all quiet (no instruction, birth or arrival); 4 arrivals refused |
| burnouts only (every 20, repair 100, dormancy on), 500 ticks | 27 burnouts: 5,124 cores offline (at most 2,143 at once), 116 programs dormant at home (91 graced), 79 died; every core back at its repair tick |
| disk failures only (every 25), 300 ticks | 9 failures, every disk all INERT and unheld after (1 codon was held: programs barely use the disk here) |
| storms only (every 60, moves 20–40, strikes 10–20), 1,200 ticks | 22 storms, 300 moves (each to the neighbour its heading picked a move ahead, turning at most 30°), 559 strikes all where a storm was, 14 ended at their lifetime |
| every kind with symptoms (false alarms 0.4), 700 ticks | 78 episodes ended: 44 real (38 struck; the rest found their node fallow or rebooting), 34 false alarms, none struck; every node's weather exactly what its episodes and storms say, every tick |
| every kind frequent, symptoms and sounds on, 500 ticks | 3 surges (43 nodes, 757 bodies), 6 crashes (38 deleted, 90 restarted), 12 burnouts (3,025 cores, 63 dormant, 51 graced, 46 died), 5 disk failures, 12 storms, 35 strikes (8,669 codons), 53 episodes (20 false), 19 spurious faults, 450 static flips, 10,778 sounds |

The determinism check uses the last world. Its hash is the same at 1, 4 and 20 threads and under a shuffled dispatch order, and through a snapshot taken at tick 103, during a reboot with 266 cores offline. A world on the engine's own defaults (warps, async devices, E cores, GPU and NPU) with every disaster frequent gives the same hash at 1 and 4 threads and through a snapshot. A strike's thunder reaches its source at exactly 1,000 the next tick, and its ring neighbours at exactly 500 a tick later (attenuation 0.5, floor 0).

**A preset end to end** (`configs/world.cfg`, 24 nodes, every engine default, intervals: surge 400, crash 100, burnout 80, disk 150, storm 100; 3,000 ticks in 68 s on 4 threads, Debug):
- the log shows 45 storms formed (77 moves, none old enough to end), 167 lightning strikes, 16 crashes, 18 burnouts, 9 disk failures and 2 surges. 125 symptom episodes began (37 false alarms) and 86 ended. The stats CSV's sums match the log exactly.
- The 2 surges took 8 and 10 nodes, most of a 24-node world, because 2 hops is most of it. Live nodes fell from 19 to 1 by tick 3,000, waiting out `node.fallow_min` (2,000) before they return.
- `nova resume` from the run's snapshot at tick 1,500 reproduced ticks 1,500–1,999 exactly: births 693, deaths 2,688, halted 811, frozen 608 and worn out 800, as in the run's stats row. It prints the disasters' summary: 4 crashes (246 deleted, 375 restarted), 7 burnouts (7,587 cores, 355 dormant of which 188 graced, 192 died), 3 disk failures, 7 storms, 48 strikes, 23 episodes (8 false alarms), 301 spurious faults and 33,906 static flips.

## Deviations and settled details

- **No viewer marker** (instructions: `app/viewer.cpp` belongs to the configuration page's branch). Events and stats only.
- **Surges need a map world**, as described above.
- **Burnout:**
  - cores are millicores (P and E) and GPU units; NPU units go back to their pool;
  - there's one repair time per burnout, not one per core;
  - with `life.dormancy` off, victims die (their cores still burn).
- **Crash:**
  - deletion and restart happen at the crash, then the node idles; it's the same as restarting at the end, since nothing runs in between;
  - resting dormant programs aren't running, so they're untouched;
  - async accelerator writes already in flight still land, but their scalar results are dropped;
  - the instability's spurious faults reach CPU threads only, not warps on the GPU kernel.
- **The noise floor** rises as noise in what LISTEN reads. The threshold under which other signals are dropped stays the node's own.
- **Symptoms off** means no episodes and no static: disasters strike at once (storms still move and strike, with thunder and rumble).
- **Keys behind the master switch** are read only when `disaster.enabled` is on, as other features' keys are read only behind theirs.
- **Lineage counting** isn't touched. Burnout victims rest and wake at home through the existing dormancy paths, so the lineage branch's fix covers them.
- **Not done here:** design.md and README sections for the disasters, and the plan's measured run from the late snapshot (step 7). Both wait for the merge.

## Files touched

- New:
  - `core/src/disaster.cpp`: all of the disasters, the world's side and the soup's;
  - `tests/test_disasters.cpp`: suite `disasters`;
  - `docs/results/disasters.md`.
- Shared spots, each edit in a compact commented block:
  - `core/include/nova/soup.h`: `SoupParams::disasters`, the `SoupTally` block, `Dormant::flags`, `OfflineCore`, `Weather`, `DisasterCount`, the soup's disaster API and members, `go_dormant`'s burnout mode, `UnitLedger::check`'s offline count;
  - `core/include/nova/world.h`: kinds, `DisasterParams`, `Episode`, `Storm`, accessors and members;
  - `core/include/nova/stats.h`, `core/src/stats.cpp`: the columns, the per-node and signal-tap columns;
  - `core/include/nova/snapshot.h`: v4;
  - `core/src/keys.inc`, `docs/keys.md` (regenerated).
- Hooks:
  - `core/src/soup.cpp`: key read, init, the fault countdown, `go_dormant`, brownout and fever heat, the end-of-tick hazards, collapse, hash, save, load, check;
  - `core/src/interp.cpp`: spurious faults;
  - `core/src/world.cpp`: init, rebooting nodes idle, refused arrivals and pass-through, noise, the barrier, hash, save, load, structural keys, old snapshots;
  - `core/src/run.cpp`: events into the log;
  - `core/src/view.cpp`: the new columns in charts and archives;
  - `app/main.cpp`: the `nova resume` summary.
- Build: `core/CMakeLists.txt`, `tests/CMakeLists.txt`.
- Tests updated:
  - `tests/test_main.cpp`: the fallback and the suite;
  - `tests/test_view.cpp`, `tests/test_dispersal.cpp`, `tests/test_phase4.cpp`: headers with the new columns;
  - `tests/test_snapshot.cpp`: the future format is now 5.
