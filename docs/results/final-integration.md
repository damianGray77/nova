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
