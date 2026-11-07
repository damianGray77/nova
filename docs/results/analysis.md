# Results: learning from the runs (the offline parts)

Steps A0, A2 (offline), A4 and A1 of `docs/analysis-plan.md` (revision 1), approved by Damian as recommended. Built 2026-11-07 on branch `wt/analysis`. Left for later: A2's live `stats.csv` columns and the viewer's "new kinds of life" list, A3 (behaviour profiles) and A5 (the map of life).

Nothing here changes a world. The tools only read a run's files, and the search runs ordinary `nova run` processes. No engine or viewer file was touched, so every pinned and gate hash is unchanged. No setting was added to the registry: the offline tools have none, and the search file's keys are its own (see A1).

## What was built

### A0: the shared library and `nova analyse`
- **`core/src/learn/`** (declared in `core/include/nova/learn.h`), hand-rolled with no STL:
  - `linalg.cpp`: a row-major `Matrix`, `dot`, an in-place `cholesky`, forward and back solves, `cholesky_solve` and `cholesky_logdet`;
  - `pca.cpp`: principal components by power iteration with deflation, never forming the d × d covariance. Start vectors are fixed and each component's sign is set, so the same input gives the same map. Also `pca_project`;
  - `kmeans.cpp`: k-means++ seeding from a caller's seed, then Lloyd's iterations. An emptied cluster restarts at the farthest point;
  - `ngram.cpp`: `NgramModel`, n-gram counts (up to 4 symbols, alphabets up to 64) with Witten-Bell interpolated smoothing, in dense arrays (24 MB for 49 ops at order 4);
  - `align.cpp`: `align_cyclic`, an edit distance that forgives a rotation and tolerates shifts, plus a plain `edit_distance` and `same_offset_similarity` (the tracker's count, for comparison);
  - `nj.cpp`: `neighbour_join`, `tree_newick` and `tree_distances`;
  - `gp.cpp`: a Gaussian process with expected improvement, plus `latin_hypercube`, `normal_pdf` and `normal_cdf`.
- **`core/src/analysis/`** (declared in `core/include/nova/analysis.h`):
  - `runfiles.cpp`:
    - `LineReader` opens a file with `FILE_SHARE_READ | FILE_SHARE_WRITE` and streams it a line at a time. It leaves out a last line with no newline yet, which a live run is still writing.
    - Also `CsvHeader` (columns by name), `GenebankReader`, `OpAlphabet`, `load_run_config` (`run.cfg`, else `replay.log`'s config lines), `run_codon_table`, `load_gated` and path helpers.
  - `novelty.cpp` (A2), `tree.cpp` (A4), `scores.cpp` and `search.cpp` (A1).
- **`app/analyse_cmd.cpp`**, the commands:
  ```
  nova analyse novelty <run> [--window N] [--threshold BITS] [--top K] [--out DIR]
  nova analyse tree <run> (--lineage N.K | --sample N) [--max N] [--neighbours N] [--pool N]
                    [--seed S] [--threads N] [--out DIR]
  nova analyse scores <run> [--body-floor N] [--threshold BITS]
  nova analyse lineages <run> [--top K]      (the lineages with the most saved genotypes)
  nova search <search file> [--dry-run]
  ```
  Output goes to `--out`, by default `analysis\<run folder's name>`. The tools refuse a folder inside the run, so `nova analyse` never writes to a run.

### A2: the novelty meter, offline (`nova analyse novelty`)
- **The model:** every `genebank.txt` entry, in save order, becomes op symbols and goes through a 4-codon n-gram model. A genotype's surprise is the bits the model needs per codon to code it, measured before the model learns it.
- **The outputs:**
  - `novelty.csv`, per window (default 10,000 ticks): saves, new genotypes, innovation (mean surprise), novel share and count, successful count, successful innovation, and novel-and-successful;
  - `novelty_top.csv`: each window's most surprising successful genotypes, with their genomes;
  - `novelty.txt`: the headline, with totals, a histogram, each measure by quarter with its second-half slope, and a verdict.
- **Reuse:** `NoveltyMeter` touches no files, so the live columns can use it.

### A4: family trees (`nova analyse tree`)
- **What goes in the tree:**
  - with `--lineage N.K`: the lineage's saved genotypes (distinct by hash), plus the founder's `--neighbours` (20) nearest genotypes saved before it, as context;
  - with `--sample N`: N/2 genotypes drawn uniformly (reservoir sampling from `--seed`), plus their saved parents.
- **The cap:** `--max` 2,000 by default, at most 4,000, because neighbour joining grows as the cube. Bigger lineages are thinned as families.
- **The steps:** alignment distances for every pair (`--threads`), then neighbour joining.
- **The outputs:** `tree_<id>.nwk` and `tree_<id>_links.csv`.
  - Each genotype has a relation: `link` (the tracker's parent is in the tree, with its distance and ranks), `outside`, `founder` or `arrival`.
  - A parentless member's likely ancestor is the nearest earlier genotype, from the tree and the `--pool` (20,000) saved before. For an arrival, its own lineage counts too.
  - Confidence = (the median distance to the candidates − the best) / that median: 0 means typical, 1 means identical.

### A1: settings search (`nova search`)
- **The search file:** a `key = value` file parsed with `Config`. Its keys are the search's own, read only from its file, so they aren't in the registry. `configs/search/example.cfg` is annotated.

  | key | default | meaning |
  |---|---|---|
  | `search.base` | (required) | the base config |
  | `search.out` | (required) | the search folder |
  | `search.seed` | 1 | the search's own choices follow from it |
  | `search.trials`, `search.init` | 40, 20 | trials in all; how many spread first (a Latin hypercube) |
  | `search.seeds` | 2 | runs a trial; world seeds 1000 × search.seed + 1, + 2, ..., the same for every trial |
  | `search.parallel`, `search.threads` | 2, 2 | runs at once; threads a run |
  | `search.ticks` | 50000 | ticks a run |
  | `search.best` | 5 | how many `best_K.cfg` |
  | `search.body_floor` | 1 | bodies per node a window needs to count as persisting |
  | `search.floor_penalty` | 2 | the objective lost per whole floor missed |
  | `search.threshold` | 3.0 | novelty's threshold, bits per codon |
  | `range.<setting>` | | `lo hi [log\|linear]`, or `registry [log\|linear]`, checked against the registry; switches and choices search all values |
  | `set.<setting>` | | a fixed change to the base |
  | `score.<score>` | lifespan 1, novelty 1 | weights |
  | `floor.<score>` | genotypes 10, persistence 0.9 | floors |

- **Each run's `run.cfg`** is complete (base, outputs, `set.*`, the trial's values, seed, ticks and threads), in `trial_NNN\seed_K`. `nova run run.cfg` there repeats it.
- **Trials run as `nova run` child processes,** not in-process:
  1. a fatal error in a world costs that trial, not the search;
  2. nothing process-wide is shared between trials;
  3. the command that ran a trial is the command that repeats it;
  4. memory goes back between trials.

  Children join a kill-on-close job object. Running a search again reuses every finished run whose `run.cfg` is unchanged.
- **Scores,** from each run's files (also `nova analyse scores`), averaged over seeds:

  | score | from |
  |---|---|
  | lifespan | the median lifespan of gated lineages (`detected` rows, plus replay.log replicator events) |
  | depth | the 90th-percentile depth of the same lineages |
  | genotypes | distinct genotypes saved |
  | evenness | Pielou's J of those genotypes over lineages |
  | novelty | A2's successful novelty in the run's second half, per 1,000 ticks |
  | persistence | the share of stats windows with bodies at or above `body_floor` × nodes |
  | speed | ticks a second (`result.txt`) |

- **The objective:** Σ weight × ln(1 + score) − floor_penalty × Σ shortfall, where a shortfall is (floor − score) / floor.
  - The default, ln((1 + lifespan)(1 + novelty)), ranks like lineage length × novelty.
  - A failed trial counts as 1 below the worst finished one.
- **The model:**
  - a Gaussian process with a constant mean and a Matérn 5/2 kernel, a length scale per setting, and signal and noise variances;
  - fitted by maximum marginal likelihood, by pattern search from three fixed starts;
  - a Latin hypercube first, then batches of max(1, parallel / seeds). Each batch member maximises expected improvement over random candidates plus candidates near the best five, all from the search's own stream;
  - a batch is chosen only after the last finished, so timing never changes a choice;
  - settings snap to what a config can hold, and a repeated trial is skipped.
- **The outputs:**
  - `summary.csv`, rewritten after every batch;
  - `pareto.csv`, the best trade-offs (speed only when weighted);
  - `best_K.cfg`;
  - `influence.csv`, the settings ranked by 1 / their fitted length scale;
  - `search.cfg`, a copy of the search file.

## Settled details
1. **Alphabet:** the codon table's distinct ops in codon order (49 for `nova_v2`). The genebank stores op names, so doubled codons are one symbol.
2. **Smoothing:** Witten-Bell interpolation over orders 0–3, with a uniform base. It can be worked by hand and needs no tuning. A random genome scores above log2 49 = 5.6 bits (8.7 in the test).
3. **Repeats:** a genotype saved again on another node counts in saves, but isn't scored or learned again. In long1, 46% of entries are repeats.
4. **Successful** means the lineage met the gate at any time: `lineages.csv` `detected` rows, plus replay.log replicator events for living lineages.
5. **The novel threshold** is 3.0 bits per codon. Mutants of known code score about 1.5–3, new code 3.5 and up.
6. **Alignment:**
   - semi-global: the shorter sequence is aligned against the longer written twice, with unit costs;
   - it's an upper bound on the cyclic edit distance, exact for a pure rotation or insertion;
   - it's symmetric, and distance = edits / the longer length (unrelated genomes are about 0.75–0.97 apart).
7. **Agreement:** a tracker link is confirmed when no other leaf lies strictly nearer the child along the tree than the parent does.
8. **Common random numbers:** seeds are shared across trials, so trials differ only by their settings.
9. **Determinism:** a search file repeats exactly, except when `score.speed` is weighted. The search warns about that.
10. **A search where nothing runs stops** after a first batch that fails entirely.

## The novelty figures from long1
`nova analyse novelty runs\long1`, on the live run read with shared access: 12 s on the Debug build, at about tick 520,000.

- **Totals:** 2,478,727 entries; 1,343,532 new genotypes (1,135,195 repeats on other nodes); 49 ops; 306,389 gated lineages.
- **Overall:** innovation 2.75 bits per codon; novel share 0.325; 748,887 successful (2.56 bits per codon); 174,439 novel and successful.
- **Surprise histogram:** 1.5–2 14%, 2–2.5 25%, 2.5–3 25%, 3–3.5 16%, 3.5–4 9%, 4 and up 8%.

| by quarter of the run | Q1 | Q2 | Q3 | Q4 | slope per 100k ticks, 2nd half |
|---|---:|---:|---:|---:|---:|
| innovation (bits/codon) | 3.30 | 2.75 | 2.81 | 2.84 | −0.004 |
| novel share | 0.456 | 0.330 | 0.347 | 0.349 | −0.007 |
| successful innovation (bits/codon) | 2.82 | 2.54 | 2.61 | 2.71 | +0.050 |
| novel and successful, per window | 1,064 | 3,433 | 3,469 | 5,063 | +1,002 |
| new genotypes, per window | 13,085 | 24,511 | 26,897 | 36,080 | +6,319 |

**Headline: innovation is not falling.** Surprise has held at 2.8 ± 0.1 bits per codon since about tick 190,000, with a novel share near 0.35. Successful innovation is creeping up, and novel, successful genotypes per window keep rising.

- **Ticks 0–60k:** high by construction, because the model starts empty.
- **Ticks 60–80k:** a dip to 1.67 bits and a novel share of 0.08. That's a sweep: lineage 500.20 (31,570 saves, ticks 60,601–148,576), then 510.32 (108,143 saves, ticks 76,774–331,629).
- **From there:** recovery to about 2.8 by tick 190,000, flat since.

**Caveats:** the last window is partial. And surprise is measured against everything before, so new 4-codon runs keep arriving partly from fresh random code that sparks bring to life.

## Family trees from long1
`nova analyse lineages runs\long1`: 854,906 lineages with saved genotypes; the biggest is 510.32. `nova analyse tree runs\long1 --lineage 510.32 --max 2000 --threads 4`: 16 s on the Debug build.

- **The sample:** 49,146 distinct genotypes, thinned to 2,000 as families; 1,642 tracker links have both ends in the tree.
- **Agreement is low:**
  - the tree puts the recorded parent nearest for 12.6% of links, and among the three nearest for 18.3%;
  - alignment alone puts it nearest for 19.2%;
  - the child–parent distance has a median of 0.34 and a 90th percentile of 0.69; only 21% of links are within 0.1, and 36% are 0.5 or more;
  - where the parent is close (≤ 0.1), alignment puts it nearest for 65%.

  So the tracker's parent links mostly don't join near-copies. That's consistent with its 75% same-offset rule and with genotype tags taken when a block joined.
- **Founders and arrivals:** the lineage's 9 parentless members are all arrivals on other nodes. All were linked, with a mean confidence of 0.78, most to their own lineage elsewhere at confidence above 0.9. The first, `0032abt` (node 571, tick 76,774), is identical to lineage 500.20's `0032acn` (node 552). So the run's biggest lineage is code carried over from the earlier sweep's lineage under a new id.

## The tiny real search
`nova search` on `configs/world.cfg`:
- **The world:** 20 nodes, `world.soup_size = 8192`, devices on the CPU, world.cfg's seeded founder.
- **The runs:** 3,000 ticks; 6 trials (4 spread first, 2 by expected improvement); 1 seed; 2 runs at once of 2 threads; 25 s on Debug.
- **The settings varied:** `cpu.wear`, `spark.rate` and `mem.upkeep`.

| trial | phase | cpu.wear | spark.rate | mem.upkeep | lifespan | genotypes | novelty | objective |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | lhs | 0.004368 | 0.253 | 3.719 | 940 | 51 | 0 | 6.85 |
| 2 | lhs | 0.0004989 | 0.7919 | 0.9188 | 1521 | 101 | 4.74 | 9.08 |
| 3 | lhs | 0.001338 | 0.05687 | 1.942 | 968 | 56 | 10.2 | 9.29 |
| 4 | lhs | 0.000431 | 0.1177 | 2.818 | 1063 | 212 | 37.9 | **10.63** |
| 5 | ei | 0.000342 | 0.05119 | 0.8827 | 897 | 142 | 27.4 | 10.15 |
| 6 | ei | 0.0002401 | 0.1548 | 2.762 | 1424 | 169 | 23.6 | 10.46 |

- **Repeatability:** trial 4's `run.cfg`, run by hand at 4 threads, gave its hash (203394587ed279a6).
- **Without the seeded founder,** 20 nodes for 3,000 ticks gave no replicators. A real search needs the plan's 100 nodes for 50,000 ticks (`configs/search/example.cfg`).

## Tests
Suite `analysis` (138 checks, 0.6 s); `nova_tests` now depends on the `nova` target.
- **Maths:**
  - Cholesky (including refusing a matrix that isn't positive definite);
  - PCA recovers planted axes and repeats bit for bit;
  - k-means separates planted blobs.
- **n-grams:** hand-worked probabilities, and an empty model is 2 bits a symbol.
- **The novelty meter:** a planted random genome scores 8.7 bits against about 2.2 for mutants; repeats return −1; windows are exact.
- **Alignment:** finds a rotation by 7 and a 3-codon shift, which defeat the tracker's count; it's symmetric.
- **Neighbour joining** recovers a known six-leaf tree.
- **The Gaussian process:**
  - the Latin hypercube is stratified and seeded;
  - fits are close in 1D and 2D;
  - an unused setting gets a long length scale;
  - expected improvement finds known optima.
- **Run files:**
  - a partial last line is skipped, and a 3 MB line is read;
  - CSV columns are found by name, with an extra `founder_origin` column;
  - scores come out exact from hand-written files;
  - a synthetic tree links its founder to its planted relative (confidence 0.93).
- **The search:**
  - a tiny search run twice gives identical summaries, apart from speed;
  - a trial's `run.cfg` reproduces its hash;
  - eight kinds of bad search file are refused.

## Deviations and open items
- **Docs:** `design.md` and the README aren't updated yet; that's left for the merge.
- **Live parts:** none yet, as agreed. `NoveltyMeter` is ready for them.
- **Persistence counts every body,** sparks included, because there's no members-only column.
- **Evenness** is of saved genotypes over lineages, not of living abundances.
- **Influence** comes from one final fit, and means little with few trials.
- **`--sample` trees** link founders only within the sample.

## Files touched
- **New:**
  - `core/include/nova/learn.h`, `core/include/nova/analysis.h`
  - `core/src/learn/{linalg,pca,kmeans,ngram,align,nj,gp}.cpp`
  - `core/src/analysis/{runfiles,novelty,tree,scores,search}.cpp`
  - `app/analyse_cmd.cpp`
  - `tests/test_analysis.cpp`
  - `configs/search/example.cfg`
  - this note
- **Edited, in compact commented blocks:** `core/CMakeLists.txt`, `app/CMakeLists.txt`, `app/main.cpp`, `tests/CMakeLists.txt`, `tests/test_main.cpp`.
