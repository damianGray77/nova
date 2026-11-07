# The lineage tracker's fixes and founders by origin: results

Built from disasters plan X12 and world plan W7, both approved by Damian. Branch `wt/lineage`, uncommitted. The tracker only observes, so no world hash changed: every pinned and gate hash holds, and the 20,000-tick landscape run below ends on the same hash with the old and new code.

## 1. What was built

**The dormancy leak (X12).**
- A program now leaves its lineage when it goes dormant, wherever it rests, through a new observer hook, `Lineages::on_dormant`. `Soup::go_dormant` calls it right after it takes the program's cargo tag into `dorm_tag_`.
- On waking it rejoins through `on_arrive`, as an arrival does.
- Since the block is no longer a member, freeing it later (at its departure, or at its death while dormant) leaves nothing again. So the lineage is never left twice.
- `on_arrive` no longer overwrites a member's tag. If the block is already a member, it stays as it is. That can happen only when an INJECT took over a resting program, which needs `contact.inject_key` off, because a resting program has no surface.

**Copies the tracker missed (X12).** `Lineages::on_divide` now asks `descends()`:
- A daughter joins her parent's lineage when at least `detect.similarity` of her own codons match the parent at some alignment. Only codons the parent's body wrote count (`detect.written`, as before).
- The share is measured against her length.
- Alignments, in order; the first that passes is taken:
  1. the straight comparison;
  2. shifts of 1 to `detect.shift` codons, forward then back (+1, −1, +2, −2 and so on);
  3. rotations: only where her first 4 codons appear in the parent, at most 8 tried.
- The new key `detect.shift` (default 8, range 0–64) is read in `detect_params_from_config` the same way as the other `detect.*` keys.

**Founders by origin (W7).**
- Each body carries an origin byte: spark, daughter, arrival, wake or unknown.
- When `Lineages::link` founds a lineage, the founding body's origin goes into `LineageInfo::founder_origin` and into a count per origin (`Lineages::founded_by`).
- `lineages.csv` gets a new last column, `founder_origin`, written as a word.
- `stats.csv` gets four new columns: `founded_spark`, `founded_daughter`, `founded_arrival` and `founded_wake`, counted per window.

## 2. Settled details

**Dormancy.**
- The hook is `on_dormant(k)`: leave if block k is a member. It is the same operation as `on_free_block`, kept under its own name.
- If a lineage's last living member goes dormant, the lineage ends on that node and gets an extinct row in `lineages.csv`. When the program wakes, the lineage reappears there as a new entry (founded = the wake tick, gate state carried in the tag), exactly as a lineage does when a member arrives on a node where it has no members. The lineage id doesn't change.
- A rejoin is counted in `members` (members ever), as an arrival is.

**Alignment.**
- Every alignment reads the parent cyclically: her codon i is compared with parent codon (i + r) mod the parent's length, the way a read head wraps at its block's end. So a shift is a rotation by a small r. A daughter longer than her parent is compared all the way along, so a copy of twice the parent (from a wrapped write head) matches in full.
- Duplicate offsets are skipped. Shifts stop at half the parent's length. Rotation candidates skip offsets already tried as shifts and are taken in the parent's order. For a parent of 17 codons or fewer, the shifts already cover every rotation.
- An alignment stops as soon as more than (1 − similarity) of her codons have missed, so a failing one costs about a quarter of her length in comparisons. A birth costs at most 25 alignments: the straight one, 16 shifts and 8 rotations.
- The GPU-link flag (most matching codons written by GPU ops) is taken from the alignment that passed.
- *Every link the old same-offset rule made is still made.* Its matches are a subset of the straight comparison's, and it needed similarity × the longer length. The diagnostic run confirms this exactly: 8,650 old-rule DIVIDE links in both runs.

**Origin byte.**
- It lives in the soup, as `Soup::origin_` per body slot, next to `dorm_tag_`. It isn't in `Body`, because `Body` is hashed and saved raw.
- It is set by:
  - `sparks()` → spark;
  - `divide()` (DIVIDE here) → daughter;
  - `Soup::arrive()` → arrival (this includes daughters dispersed from a neighbour);
  - `Soup::wake()` → wake.
- Everything else reads unknown: bodies alive at a snapshot resume (`Soup::load` resets the array), a seeded genome, and bodies made through the test API.
- A dormant program that lands on a node stays unknown until it wakes, but it can't found a lineage while it rests.
- The founder is the parent block of the founding link (DIVIDE or INJECT), and its origin is that of the body owning the block.
- The founder's origin travels in `CargoTag` (`gated` became a `u8`, followed by `u8 origin`, so the tag stays 32 bytes and `Cargo` stays 104). Every node's entry for a lineage therefore names the same founder. An entry made by an arrival isn't counted as a founding.

**CSV.**
- `lineages.csv`: `…,founder_genotype,detected,founder_origin`.
- `stats.csv`: the four columns sit after `resting` and before `compress`, at indices 90–93. So no earlier integer column moves; `compress` and `ambient` move to 94–95, and every reader finds them by `kStatsIntColumns`.
- `kStatsColumns` is now 96 and `kStatsIntColumns` 94.
- The viewer charts the four columns as sums (per-tick rates): `stats_column_is_sum` covers 90–93.
- `RunArchive::load_stats` now sizes its field buffer from `kStatsColumns`. It was a fixed 96, which the new header would have filled exactly.
- Unknown has no stats column, as specified, so seeded founders and founders alive at a resume show only in `lineages.csv`.

**Not changed.**
- `on_inject` is unchanged. Its match is counted inside the interpreter at fixed offsets, so the new rule doesn't drop in without touching `interp.cpp`.
- The presets don't list `detect.shift`. The default applies, and the key moves into the registry at merge. *(Done at the integration: a `keys.inc` entry in Detection and lineages, read with `key_u32`.)*

## 3. Deviation from the plan: at least 4 matching codons

**What the plan's rule did.** Built exactly as written, the rule linked 1-codon daughters. A written codon that matches anywhere in a long parent passes, because the rotation search finds where it appears and 1 of 1 codons is 100%.
- In the random-soup test (300 ticks, sparks only), about 300 DIVIDE links appeared, all with 1-codon daughters of 27- to 60-codon sparks. The test `detect: random_budding_never_links` failed.
- The viewer test's genebank held 1-codon genotypes, so its `0-1` length filter was no longer empty.

**What I added.**
- A link also needs at least 4 matching codons, or 3/4 of the parent's length if that is fewer (parents of 4 codons or fewer). That is `need = max(⌈s·len_d⌉, min(4, ⌈s·len_p⌉))`. The 4 is the rotation key's length.
- The old rule still passes wherever it did, so exact copies of very short parents still link.
- For random codons, the chance that a 4-codon daughter passes is a few in a million births.
- With the floor, both failing tests pass unchanged. In the landscape run, daughters of 4–7 codons make only 2.6% of the newly caught links.
- *This is my call, not the plan's.* It is the constant `kDescentFloor` in `lineage.cpp` if Damian wants it different.

## 4. Tests and changed counts

**New checks.**
- `detect` (91 → 170 checks):
  - `descent_not_likeness`: a rotation by 9 (beyond the shifts), a head two codons late (only a shift finds it; with `detect.shift = 0` it doesn't join), a shifted partial copy, and half the parent each join one generation deeper. A rotated granddaughter joins two deep, and a doubled copy joins.
  - It also checks what doesn't join: unrelated written codons, a rotation lying as remains, and a partial with 8 of 12 codons matching (9 of 12 does).
  - `detect.shift` accepts 64 and rejects 65.
  - `rotation_through_divide`: the same through ALLOC and DIVIDE in the interpreter.
  - `founder_origins`: each origin (spark, daughter, arrival, wake and unknown) is recorded on the body and as the founder of a lineage. The founder's origin travels in the cargo tag, and the next node's entry names it without counting a founding.
  - `founders_in_the_csv`: in a 24-node dispersal world with dormancy, the four stats columns summed over the run equal the detectors' counts, and with the seeded (unknown) founders they make every lineage founded.
  - `output_files`: the `founder_origin` header, and `unknown` for the seeded founder.
  - `snapshot_resume`: every body alive at a resume reads unknown.
- `ecology` (196 → 230): `test_dormant_lineage`.
  - Three rest-and-wake rounds at home leave the count at 2; it used to gain a phantom each round. The peak stays 2, and the lineage ends once when its last members die.
  - One that drifts off leaves once, and its tag goes with it.
  - One that dies resting changes nothing.
  - A lineage whose last member is dormant has ended.
- `view` (124 → 125): the header, and the founded columns as sums.
- `dispersal`: the header.

**Changed counts.** No asserted expected number had to change. The gate suite prints its detector-event counts but checks only that they agree across thread counts and dispatch orders:

| gate world | old | new | hash (unchanged) |
|---|---|---|---|
| procedural world (dispersal): colonist events | 14 | 15 | `8807336232190f56` |
| procedural world (dispersal): gate events | 30 | 31 | |
| Phase 4 world (every feature): gate events | 28 | 29 | `55ac56ad58482685` |

- *Why:* I reproduced both worlds with the old and new `nova.exe`, and the hashes are identical.
  - In the procedural world, two more daughters of the seeded 13-codon disperser on node 4 now join lineage 3.1, as copies at an offset that the straight comparison missed. One disperses to node 8, which is the extra colonist event at tick 35.
  - The Phase 4 world's extra event is the same: lineage 18.1 colonising node 14 at tick 63.

**Before the floor.** The two tests in section 3 (`detect` random soup, the `viewer` browser filter) failed with the plan's rule as written. They pass unchanged with the floor.

## 5. Measurements

`configs/landscape.cfg`, 20,000 ticks, 4 threads, Release, the same seed.
- *Old:* HEAD (`98c2a3c`) built in a scratch folder.
- *New:* this branch.
- Outputs are in `build-rel/measure/{old,new}`, which git ignores.
- Both runs end on hash `1b73a9aacf981bbc` with 1,397,261 births. So the world is identical, and only the tracker's figures differ.

| | old | new |
|---|---:|---:|
| inheritance links (DIVIDE + INJECT) | 8,790 | 18,089 |
| lineages founded | 862 | 1,756 |
| ... per 1,000 ticks, by 5,000-tick quarter | 43.6, 44.8, 43.2, 40.8 | 112.4, 87.6, 78.6, 72.6 |
| ... per 1,000 ticks, overall | 43.1 | 87.8 |
| founding lineages that never got past depth 1 | 697 (81%) | 1,529 (87%) |
| max depth (`result.txt`) | 424 | 424 |
| deepest living member, peak | 398 | 398 |
| `replicator` events (gate met) | 168 | 235 |
| `colonist` events | 256 | 380 |
| `gpu_replicator` / `gpu_native` events | 298 / 298 | 437 / 437 |
| `signaller` events | 11 | 19 |
| `top_lineage` at the end / `genotypes_in_top` | 38 / 0 | 8 / 1 |
| windows (of 20) whose top lineage has no living genotype | 15 | 0 |
| lineages living at the end | 286 | 586 |
| founder length: most common | 32 codons, 8.4% | 32 codons, 5.1% |
| founder length: share above 45 codons | 10.3% | 23.6% |
| wall time | 1,382.8 s | 1,374.7 s |

The founder-length row is for comparison with sparks, which give each length from 8 to 64 equally (1.75% each, 33% above 45).

**Founders by origin (new run):**

| origin | founders | share |
|---|---:|---:|
| spark | 1,314 | 74.8% |
| wake | 245 | 14.0% |
| daughter | 196 | 11.2% |
| arrival | 1 | 0.1% |

The per-window counts (spark, daughter, arrival, wake):
- first 1,000 ticks: 168, 20, 0, 6;
- ticks 1,000–2,000: 90, 13, 0, 7;
- from tick 2,000 on, a window held 43–76 spark founders, 3–17 daughter founders, 0–1 arrival founders and 7–20 wake founders.
- Daughter founders fall over the run, from 9–17 a window between ticks 2,000 and 12,000 to 3–8 after it. Wake founders rise a little, from 7–17 to 8–20.

**What the numbers say.**
- *The dormancy leak is gone.* The old run reproduces `runs\long1`'s symptom: from tick 6,000 the top lineage sits at 38 living members with no living genotype, for 15 of the 20 windows. The new run never shows it.
- *Founding rose rather than fell.* This doesn't match the plan's expectation. A diagnostic build, run in a scratch folder and not committed, classified every link of the same run:
  - 17,949 DIVIDE links, of which 9,299 are new. By alignment: 350 straight, 5,991 shifts and 2,958 rotations.
  - 80% of the new links (7,400) are copies of the parent's full length, read at an offset. 18% are shorter partial copies (587 under half the parent), and 2% are longer.
  - Daughter lengths: 244 of 4–7 codons, 2,064 of 8–15, 5,244 of 16–31 and 1,747 of 32 or more.
  - 1,009 foundings came through new links: 772 by sparks, 144 by woken programs and 93 by daughters.
  - So the newly caught copies are mostly spark-born programs that copy themselves at an offset (a read head that doesn't start at their first codon). Each now founds a lineage, where before it was invisible. Most of these lineages stay at depth 1.
  - Merging missed copies into their parents' lineages, which the plan expected to lower founding, also happens, but in this early-world run it is outweighed.
  - The plan's figure (spark-born at most 22% of founders) came from ticks 100,000–250,000 of the long run, when sparks are rarer. A late snapshot would test that period; this run doesn't.
- *Max depth didn't move*, because the deepest lineages here are exact copiers.
- *The founder lengths moved toward the even spread of sparks*: the peak at 32 fell from 8.4% to 5.1%, and the share above 45 codons rose from 10% to 24%.
- *Cost:* none measurable. The new run was 8 s faster over 23 minutes, which is noise on the shared machine.

## 6. Files touched

- `core/include/nova/lineage.h`: header notes; `DetectParams::shift`; `LineageInfo::founder_origin`; `origin_name`; `on_dormant`, `founded_by`, `descends`, `aligned`.
- `core/src/lineage.cpp`: `detect.shift`; `descends()` and `aligned()` (the alignments and the 4-codon floor); `on_divide`; founder origin and counts in `link`; `cargo_tag` carries the origin; `on_arrive` leaves an existing member alone and takes the founder's origin; `origin_name`.
- `core/include/nova/soup.h`: the `kOrigin…` constants; `CargoTag` (`u8 gated`, `u8 origin`, `u16 pad`); `Soup::origin()`; `origin_`; the dormancy comments.
- `core/src/soup.cpp`: `origin_` in `new_body`, `init`, `sparks`, `arrive`, `wake` and `load`; `on_dormant` in `go_dormant`.
- `core/src/interp.cpp`: a DIVIDE daughter's origin.
- `core/src/run.cpp`: the `founder_origin` column.
- `core/include/nova/stats.h`, `core/src/stats.cpp`: the four columns and their marks.
- `core/src/view.cpp`: `stats_column_is_sum`; the `RunArchive::load_stats` field buffer.
- `tests/test_detect.cpp`, `tests/test_ecology.cpp`, `tests/test_view.cpp`, `tests/test_dispersal.cpp`: the tests above.
- `docs/results/lineage.md`: this note.

## 7. For the merge

- **The registry:** `detect.shift` is read in `detect_params_from_config` (u64, 0–64, default 8), next to `detect.similarity`.
- **design.md is not updated.** It needs:
  - in 6.6 (detection), the DIVIDE link rule, the 4-codon floor, `detect.shift`, and `founder_origin` in the lineage log;
  - in 6.9, the founder's origin in the cargo tag;
  - in 6.17, that a dormant program leaves its lineage while dormant and rejoins on waking;
  - the four stats columns.
- **The X12 call says the CSV doesn't change.** W7 adds columns, so it does now.
