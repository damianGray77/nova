# Final integration — 2026-11-07

Memory tiers, profiles and the viewer were committed on their feature
branches and cherry-picked onto `next`, in that order. The interpreter
keeps memory-tier prices before execution and the profile probe around it.
Stats now have 139 integer columns followed by `compress` and `ambient`:
disasters start at 97, memory tiers at 123, profiles at 126, novelty at 136.
The viewer distinguishes sums from gauges and converts novelty fixed point.
The generated reference contains 359 settings.

The memory-strip accessor now returns the soup's actual fast boundary,
including page rounding. The legacy format defaults also disable profiles
and novelty, alongside memory tiers, substrates, activity sparks, archive
and disasters. The configuration check reads all new switches before its
early return. The historical run `long1` was stopped at Damian's request at
tick 672142, with final snapshot and hash `22cd4f16db64853f`; it was not
resumed or used as an output directory for these checks.

## Verification

The feature verifiers passed 45 suites for memory tiers, 45 for profiles
and 44 for the viewer. The viewer needed one correction: the archived
substrate-floor read now goes through `key_f64`, rather than a raw getter.
The first complete integration passed all 46 CTest entries, including the
no-STL include guard. The final run adds the combined memory/observer gate
and the live fast-strip assertion; its outcome is recorded below.

The memory suite compares DXC and FXC kernel images and lanes against the
CPU reference. Its full world ran 160 hardware rounds, all verified, with
zero differences and hash `8e8b6767650ed416`. The embedded pre-build v2
replay still checks all six checkpoints. The combined disaster test compares
world hashes and ordered events at 1, 4 and 20 threads and shuffled
dispatch, through a snapshot during reboot, and with both observers off.

## Release measurements

Built in `build-rel` with tests disabled. The paired runs use landscape
seed 1, 100 nodes, the new per-node resource sizes, 1,000 ticks, four
threads, and CPU accelerator/warp references. All unspecified settings use
the new engine defaults. Each run writes its own stats, genebank, profiles,
arrivals, replay and result files. These are single short timing samples,
with other verification work on the machine, not precise overhead estimates.

| Variant | CPU instructions | Instructions/tick | Seconds | Final hash |
|---|---:|---:|---:|---|
| Memory tiers off | 51,517,499 | 51,517 | 16.5 | `7980dbafbe9caa16` |
| Profiles off, tiers on | 48,778,624 | 48,779 | 16.3 | `5879c85b436a1b40` |
| Everything on | 48,778,624 | 48,779 | 16.8 | `5879c85b436a1b40` |

Tier pricing reduces executed CPU instructions by 5.3% in this sample.
Profiles leave both the instruction count and world hash unchanged. Their
on/off timing difference is about 3%, within a noisy single-pair check.

The full `configs/landscape.cfg` smoke run used 1,000 nodes, 20 threads and
3,000 ticks with the actual default accelerator selection. It completed in
89.4 simulation seconds (33.5 ticks/s), executing 1,919,341,687 CPU
instructions, with 136,098 bodies on 1,000 live nodes. Hash:
`b87cf62d1c9ef091`. This early, partially filled world is not a prediction
of mature-world throughput or a controlled comparison with `long1`.

Its 512 founders were 392 sparks, 82 daughters, one arrival and 37 wakes.
Maximum lineage depth was 63. There was one crash, one burnout and three
symptom episodes; the rarer surges, failures and storms did not strike in
this short run (the deterministic suites exercise them). Fast ALLOCs made
6,201 claims and failed 10,277 times. Final fast memory held was 990,139
codons; archive held was 87,282. The last census had 3,864 samples and mean
working length seven codons. The last window's novelty was 4.342462 bits per
codon, novel share 0.571429, with one successful novel genotype. No runtime
error was written.

`nova land` confirms 542 distinct substrates, with exact 10/20/40/20/10%
shares for each type. The rendered setup page reports zero problems,
32,768 average codons per node, 10,240 disk codons (8,192 fast and 2,048
archive), and an estimated 675 MB footprint. That is the existing estimate,
not measured peak RAM; the budget/salvage issue remains open.

Offline roles processed 11,298 samples and chose four clusters. Groups
processed those samples and 5,755 arrivals; no moving-together excess was
found, while kin adjacency and pooled listen/branch association exceeded
their baselines. These observations do not establish cooperation or causal
communication. The map rendered 124 genotypes and three animation frames;
the setup page and map PNGs were visually inspected.

## Documentation and retained decisions

Design sections 6.18–6.23 consolidate the feature notes; sections 9 and 10,
README startup/layout and all four plan Result sections are updated.
Only landscape.cfg doubles its resource means. The build script accepts
`-BuildDirectory build-rel` for a separate Release tree. The budget estimate,
disk-warning tier choice, descent floor and NPU tuning remain Damian's
open decisions. New runs keep fixed settings; old knob records replay.

Measurement files and rendered evidence are retained under
`out/verification-20261107` in the main checkout before worktree cleanup.
The stray `nova_test_gpu.replay` is excluded from every commit.

## Review and fixes (after the push, 2026-11-07)

Damian asked for the budget bug fixed and the last three features
reviewed. Three read-only reviews (memory tiers, profiles, the viewer and
the integration) found nothing that changes a world; these were fixed:

- **The disk budget** (open question 1): `budget.disk` and the footprint
  estimate assumed salvage stamps off unless `salvage.ratio` was set, though
  its default is 4, and the budget sized only `world.disk` with the archive
  on top. Both now count the stamps, and the budget covers the whole tier:
  a given `world.archive` comes out of it first, else world.disk is the
  largest d with d + d/4 inside it. 8 MB now makes 8,388,357 bytes of tier
  (the old rule: about 2.2 times that). `key_given` tells a given key from
  a computed default without reading one.
- **The novelty model's counts** could wrap after 2^32 codons learned (about
  a million ticks of a big landscape), making a probability exceed 1, a
  negative surprise and an out-of-range histogram index. Totals are now 64
  bits, counts saturate, and the bin index is NaN-safe.
- **Profiles under `warp.device = auto`:** lanes were marked in the
  executed-code bitmap only when their warp hadn't run on the GPU, which is
  decided by timing, so `profiles.csv` and the `work_len` gauge could vary
  between runs. Lanes are no longer marked (their work is `lane_instr`);
  `on_lanes` checks the body's serial.
- **`nova analyse roles`** rescanned every row for every window; the rows
  are now bucketed by one sort.
- **Kin clustering** counted an adjacency twice when both neighbours were
  sampled; it takes one side per sample now. Planted z stays about 38.
- **The live map of life** grew without bound once opened; it now drops the
  genotypes not saved since the oldest save the viewer keeps. The open
  window's "new kinds of life" are rescored at most every tenth of a window
  (or on a paused frame), not every frame on the sim thread.
- **DISK's memory-side price** checked only the length against the body's
  disk, so a DISK failing on its range was charged its moves; it now checks
  `[ax, ax + n)`, as its comment says.
- **Readers:** a profiles.csv header whose needed column is past the 256th,
  and an arrivals node id over `world.nodes`' range, are refused instead of
  read out of bounds. `analyse groups` notes when its pair cap is hit.
- **A5's colour by role** was missing: the map can now colour points by
  their latest profile's role (live from the frames, a finished run from
  profiles.csv), with a legend and the role in the tooltip.

Left for Damian (design section 10): the behaviour profile in A5's genome
vector, and three memory-tier prices that differ from W3's worked numbers.
All 46 suites pass.

### Release checks of the new features

`configs/landscape.cfg` (1,000 nodes, every resource doubled, everything
on, the real RTX 5080 and NPU) ran 2,000 ticks in 53 s on 20 threads, with
the disasters made frequent: 2 surges, 10 crashes, 6 burnouts, 10 disk
failures, 26 storms and 27 lightning strikes, 20 collapses. Memory tiers
held 897,657 fast codons (590 fast ALLOCs, 453 failed); 3,821 profile
samples; the live novelty columns filled. Outputs, snapshots and the real
devices against the CPU reference all leave the world's hash unchanged, and
a snapshot resumed at 4 and at 23 threads gives one hash. Every `nova
analyse` command, every render (`--show life`, `--life-colour role`,
`--show kinds`, `--map-of-life`, substrates, live, the setup page) and `nova
land` ran. `budget.disk = 1GB` made disk-tier files peaking at 1,073,741,500
bytes. The long run's `snap.600000`, resumed 200 ticks by its own build and
by this one, gives the same hash (`ac6a0d4441722c93`), and `nova view --run
runs\long1` renders all 68 windows.

**Found, then fixed: a resumed snapshot was not exact on a landscape.**
Resumed from its snapshot, `landscape.cfg` reached a different (repeatable)
hash than the uninterrupted run -- also with the CPU reference, and also in
the build from before this one (98c2a3c), so it was older than these
features. The loaded state hashed the same. (The warps' running-lane counts
that differed first were free slots', which nothing reads.) The cause was
the allocator's "no room of this length" cache (`known_full`), which a load
neither saved nor cleared: a resumed world is built first, and on a small
node the initial sparks fill memory and note "no room", so after the load
the resumed world refused allocations the straight run made, until a codon
was freed. Loading now clears it, and so does a node's new life (a reborn
node kept its earlier life's note: deterministic, but a fresh soup refused
its first allocations). `test_resume_after_full_start` (snapshot suite)
fails without the fix at all three snapshot points. Resumed against the
uninterrupted run, tick by tick: a 100-node landscape over 700 ticks, the
1,000-node `landscape.cfg` with frequent disasters on the CPU reference with
and without warps (200 ticks), and on the real GPU and NPU (300 ticks) --
every tick the same. `nova run --dump-node N:PREFIX` writes that working
state too.

**Where the time goes (measured, nothing changed).** long1's last snapshot
(1,000 nodes, tick 672,142), resumed for 300 ticks: 12.0 ticks/s with the
detector alone, 11.9 with behaviour profiles and the live novelty meter on
too, 12.0 with nothing -- the always-on observers cost about 1%. Watching
costs more: the viewer builds its frame on the sim thread between ticks
(about 30 a second, so one after nearly every tick of a big world), and a
frame took 14.7 ms on long1 (a tick: 83 ms) and 9.9 ms on a new landscape (a
tick: 22 ms). A watched big world therefore ran some 15-30% slower than
headless. GDI's drawing (4-7 ms a frame at 1,000 nodes, about 11 ms at
10,000, on the window's own thread) never held the simulation up, so a GPU
renderer was not the fix. Two per-node passes were most of the build: the
lineage survey (7.5 ms on long1) and the dead-matter scan (3.6 ms on long1,
8.4 on the new landscape). Now the node pass runs on the run's pool between
ticks, the scan only when the map is coloured by material, and a frame
budget keeps frames to about 5% of the sim thread: 2.0 ms a frame on long1
(2.8 coloured by material), 0.33 ms on the new landscape and 1.6 ms at
10,000 nodes -- watching now costs about 2% of a tick.

**The map's seed** (`world.seed`, design 6.13): the map apart from the run,
as a number or any text, drawn on the setup page with a **new map** button.
Empty, every world is exactly as before (the landscape suite checks the same
hash with `world.seed` = `seed`, the same map with another `seed`, and other
maps for other map seeds).
