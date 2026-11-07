# Results: integrating the next build

Branch `next`, from the settings registry (`4daa357`), 2026-11-07, in two rounds. Round 1 (below) brought in substrates and the disk tiers; round 2 (after it, from "Round 2") brought in the lineage tracker, the disasters, the configuration page and the offline analysis.

## Round 1: substrates and the disk tiers

Two approved builds, each kept as its own commit with its own message, then one integration commit:

1. `Substrates, sparks by activity and resource speed multipliers` (from `wt/world`, `b6060d3`; `docs/results/world.md`): `docs/world-plan.md` W1, W2, W5.
2. `Disk tiers: an archival tier beside the disk` (from `wt/disk`, `41f95a0`; `docs/results/disk.md`): W4.
3. `Integrate substrates with disk tiers; smoother substrate map; old replay logs keep their defaults`: the wiring and its test, the new substrate defaults, replay format v3 with legacy defaults, and this note.

The world build applied cleanly: its commit's tree is `b6060d3`'s exactly, since both sit on `4daa357`. The disk build conflicted in six files, and five more places needed work although git merged them as text. Every conflict was resolved inside the disk build's commit, which passed `verify.ps1` on its own (42 tests, all green) before the integration commit went on top.

### Conflicts and how they were resolved

**Textual conflicts** (git stopped):

| file | what clashed | resolution |
|---|---|---|
| `core/include/nova/snapshot.h` | both bumped `kSnapshotVersion` to 4, each with its own comment | one v4, one comment listing both builds' v4 state (the substrate levels after the lifecycle section; the archive in each soup's disk section, and a cargo's archive share) and what older snapshots resume with |
| `core/src/world.cpp` | `is_structural`'s prefixes: each added one | both: `"substrate."` and `"archive."` |
| `tests/CMakeLists.txt` | each added a source and a suite | both: `test_substrates.cpp`, `test_tiers.cpp`; suites `substrates tiers` |
| `tests/test_main.cpp` | each added a suite declaration and entry | both. The legacy fallback had merged by itself; its three new lines now sit together: `substrate.enabled = false`, `spark.activity = 0`, `world.archive = 0` |
| `tests/test_snapshot.cpp` | each rewrote the future-version check to follow `kSnapshotVersion` | the world build's version (a `TextBuf`, no `snprintf`) |
| `docs/keys.md` | each regenerated it | regenerated from the merged registry with `build\bin\nova.exe keys --md docs/keys.md` (285 settings), again after the defaults change |

**Semantic conflicts** (merged as text, but broken, doubled or unguarded as code):

- **`SoupParams::disk_speed` and `archive_speed`** were declared twice (the disk build's tier block and the world build's eight speeds), which doesn't compile. The world build's block of eight speeds was kept, since the world sets them; the disk block now points to it. Both builds also set them to 1 in `soup_params_from_config`; that's done once now, with the other six.
- **`Run::init_from_snapshot`'s pre-v4 block** came in twice: the disk build's (`world.archive = 0`, before the `--set` edit) and the world build's (`spark.activity = 0` and, on a map, `substrate.enabled = false`, after it). They are now one block after the edit, so the config or a `--set` can still name any of the three, and in the integration commit that block became one call to `apply_legacy_defaults` (below). A pre-v4 snapshot resumes with `spark.activity = 0`, `substrate.enabled = false` (procedural and landscape worlds) and `world.archive = 0`, unless its config or a `--set` names them. Only soup worlds get them, since no other world reads them.
- **Noise field ids:** the disk build adds `kFieldArchive` as the `NoiseField` enum's last member, and the world build numbers its eight substrate fields from `kFieldSubstrate` = 256 outside the enum. They can't collide while the enum stays under 256 entries. `landscape.cpp` now has `static_assert(kFieldCount <= kFieldSubstrate)` to enforce that, and `graph.h` says so beside `kFieldCount`. Today `kFieldArchive` is 17 and `kFieldCount` 18. The noise streams are keyed by base + field id, so distinct ids mean distinct permutations.
- **The tiers suite's `test_old_snapshot`** wrote a v3-style snapshot from a world on the engine defaults with only `world.archive = 0` set. Once merged, those defaults also turn sparks by activity on, while a v3 snapshot resumes with the old spark rule, so its hash diverged. A v3 build's world had the old rule too, so the test's world now also sets `spark.activity = 0`. It's a ring, so it has no substrates.
- **The substrates suite's `test_old_snapshot`** turns a snapshot into v3. Its world had no disk, so the archive played no part. It now gets a 256-codon disk under the full legacy fallback, so it has no archive and its disk section is v3's shape. The resumed run's fallback drops all three switches, so the registry's defaults (substrates on, sparks by activity 2, an archive of a quarter of the disk) would apply if the pre-v4 block didn't turn them off. The test checks that all three are off (`archive == 0`, `world.archive = 0` in the effective config) and that the run resumes on the same hash.
- **Stats columns, `SoupTally`, the CSV header tests (`test_dispersal`, `test_view`) and `view.cpp`'s columns** didn't conflict. The world build adds per-node columns to `nodes.csv` only. The disk build's three `stats.csv` columns (`archive_held`, `archive_stores`, `archive_loads`), its two `SoupTally` fields, `stats_column_is_sum` and both header tests merged as they were and pass. `keys.inc` merged cleanly with both groups.

### The wiring: substrates divide the disk tiers' costs

- *The world end:* `World::derive_node` sets each node's `disk_speed` from its fast-disk level (`kSubDiskFast`) and `archive_speed` from its slow-disk level (`kSubDiskSlow`).
- *The disk end:* `Exec::disk_scale` divides each tier's DISK access and per-codon costs by those speeds, for classic costs and timed ones (`disk_time`).
- *The same fields, both ends:* after the duplicate pair was dropped, they are connected through the same two fields of the node's `SoupParams`.
- **A fix found while writing the test:** the speeds were floats, and the floor (0.1) as a float is 0.100000001. A cost divided by it comes out a hair under ten times and is truncated one short. Worked out in doubles exactly as `disk_scale` does it (not run as a test on the float build):
  - an 8-codon store at the floor: (4 + 0.25 × 8) ÷ 0.100000001 = 59.9999991, so 59 instructions, not 60;
  - on the archive: 599.999991, so 599, not 600.

  The four tier speeds (`mem_fast_speed`, `mem_slow_speed`, `disk_speed`, `archive_speed`) are now doubles, set from the level's exact multiplier. `soup.h` says why. The cores' and accelerators' speeds stay floats, as the world build measured them. Nothing else read the tier speeds, so no other result moves.
- **The test** (suite `tiers`, `test_substrate_speeds`) uses a procedural world of 40 nodes with substrates, a 256-codon disk and a 64-codon archive (classic costs). On every node it runs an 8-codon DISK store on each tier, on a soup built from that node's own params.
  - It checks that each node's speeds are its levels' multipliers.
  - The fast-disk cost is 6 ÷ the node's fast-disk multiplier and the archive cost 60 ÷ its slow-disk multiplier, exactly:

    | level | floor (0.1) | 0.5 | 1 | 1.5 | 2 |
    |---|---:|---:|---:|---:|---:|
    | fast disk | 60 | 12 | 6 | 4 | 3 |
    | archive | 600 | 120 | 60 | 40 | 30 |

  - So at 2× a node pays half the DISK cost, and at the floor ten times. Every level occurs (4, 8, 16, 8 and 4 nodes of each type).
- **Everything at once** (suite `tiers`, `test_varied_world`): the disk build's busy archive landscape (48 nodes, shuffled selectors, dispersal, dormancy, the lifecycle, salvage), now with substrates and sparks by activity on as well.
  - Over 300 ticks: 5,489 sparks, 169 DISK stores (151 touching the archive) and 13 loads (9). 42 of 48 nodes have a disk tier off 1×.
  - The same hash (`a8a88cd3038c22a1`) at 1, 4 and 20 threads, under shuffled dispatch, and through a snapshot taken at tick 150 and resumed on 20 threads.

### Smoother substrate map: new defaults

`substrate.smooth` 0.7 → **1**, `substrate.scale` 3 → **1.5**, in the registry and in `SubstrateParams`' own defaults (the neighbours test checks the two agree). On a 1,000-node landscape (seed 5):

| | neighbours: types differing (of 8) | neighbours / random pairs | distinct substrates (`landscape.cfg`) |
|---|---:|---:|---:|
| before | 5.24 | 73% | 990 |
| after | 1.75 | 19% | 542 |

`test_neighbours` now asserts that the defaults stay under 30% of random pairs and under 2.5 types differing. The old defaults stay in its table for the record. `docs/results/world.md` has the full table and the reasons. The change touches no pinned hash, since the legacy fallback keeps substrates off. The everything-on worlds, which run on engine defaults, changed and are still the same at every thread count.

### Old replay logs replay as they were recorded

- **The problem:** a replay log records the config it was given, so a setting it doesn't name takes the engine's default when replayed. With substrates, sparks by activity and the archive on by default, a log written before this build would replay a different world. Its first checkpoint already differs, because the hash covers the substrate levels and the archive's ledger.
- **The fix, once for every feature:**
  - **New logs are `nova-replay 3`** (`kReplayVersion`). No record changed; the version only marks a log written with the new defaults.
  - **The legacy defaults text** lives in `core/include/nova/legacy.h`, as `kLegacyFormatDefaults`:

    ```
    spark.activity = 0
    substrate.enabled = false
    world.archive = 0
    ```

    Its header says how to add a line: the disasters (`disaster.enabled = false`) and the memory tiers (`mem.fast_share = 0`) each add one there.
  - **Snapshots and replay logs share that text,** each gated by its own format version:
    - a snapshot before v4 (`kSnapshotLegacyBefore`) gets it in `Run::init_from_snapshot`, as before, now through `apply_legacy_defaults`;
    - a replay log before v3 (`kReplayLegacyBefore`) gets it through `ReplayLog::replay_config`.
  - **`apply_legacy_defaults`** sets each line the config doesn't name, in soup worlds only. A line whose registry entry needs a choice is set only where that choice holds: `substrate.enabled` only in procedural and landscape worlds. So no unused entry is added, and a ring or demo log replays exactly as before.
  - **`replay_verify`** replays with `replay_config`. The viewer's run archive builds its graph world from it too, and now with `world.archive = 0`, since only the graph is wanted. The archive's displayed config text stays the log's own.
  - **`nova replay`** says when it reads a version 1 or 2 log.
- **The tests** (suite `replay`):
  - `test_legacy_config`:
    - a v2 or v1 log gets the three lines, and a v3 log none;
    - a setting the log names keeps its value;
    - a ring log gets no `substrate.enabled`, and a demo log nothing.
  - `test_legacy_logs` runs on the engine's own defaults (no test fallback), where the new features are on.
    1. **A log from the old binary.** The build of `4daa357` (Debug) recorded `nova run` of a procedural world with a disk (12 nodes, `world.disk = 512`, 80 ticks, a checkpoint every 20). It is embedded verbatim and replays on this build to its own hashes, ending `57978c34d6bf41e8`. With the header changed to `nova-replay 3`, the same records diverge.
    2. **A log made on this build.** The same world with the three settings explicitly off ends on the old binary's hash exactly (`57978c34d6bf41e8`). Its log, written by the replay writer and rewritten as the old build wrote it (header 2, the three settings unnamed), replays to its hashes.
  - `test_parse_errors` now refuses version 4 and reads version 3. The detect suite's event-replay check expected a new log to be version 2. It now expects `kReplayVersion`.
- **The same from the command line:** `nova replay` on the old binary's log:
  - with this build's binary: "replay OK: 6 checkpoints match", after its note that a version 2 log takes the defaults it was written with;
  - with the header changed to 3: "diverged at tick 0: logged 7fdb46c43695b0e1, replayed 7a55d1c4cc113e3e";
  - the old binary replays its own log OK too.

  A v3 log is refused by the old binary ("unsupported replay version 3"), as any newer format is.
- **Not covered:** a v3 log replayed by a later build whose defaults change again. That needs another version bump with its own text (`legacy.h` says so). Adding a line for the disasters and the memory tiers to the same text works only if they land before any run is recorded with a v3 build. A v3 log recorded before they land doesn't name their switches, so a build with them would replay it with them on (their engine defaults) and diverge. Their lines only cover v2 and v1 logs.

### Determinism

All three in `verify.ps1`'s suites:

- `substrates` / `test_everything`: the busy landscape on the engine's defaults, now with the archive and the new substrate defaults too. The same hash at 1, 4 and 20 threads, under shuffled dispatch, through a snapshot resumed on 20 threads, and on the RTX 5080 with every round verified (160 of 160).
- `tiers` / `test_varied_world`: the same checks on a landscape that uses the archive (above).
- `tiers` / `test_world` and `substrates` / `test_drift_recovery`: each build's own worlds, unchanged.

Every pinned and gate hash from `4daa357` holds, because the test runner's legacy fallback has all three switches off.

### Verify

`powershell -NoProfile -File .claude\verify.ps1` on the integration commit: 42 tests (41 suites, including `substrates`, 151 checks, `tiers`, 281, and `replay`, 80, and the include check). It ended:

```
100% tests passed, 0 tests failed out of 42

Total Test time (real) = 106.00 sec
verify: clang-format not found in C:\Program Files\Microsoft Visual Studio\18\Community (advisory step skipped)
verify: OK - green in 107,816 ms
```

The everything-on world's hash in that build is `af6726f0481b4692` (suite `substrates`), and the varied archive world's `a8a88cd3038c22a1` (suite `tiers`).

### Files

- New:
  - `core/include/nova/legacy.h`, `core/src/legacy.cpp`: the legacy defaults and `apply_legacy_defaults`;
  - this note.
- Changed in the integration commit:
  - `core/include/nova/soup.h`, `core/src/soup.cpp`, `core/src/world.cpp`, `core/src/interp.cpp`: the tier speeds as doubles;
  - `core/include/nova/landscape.h`, `core/src/keys.inc`, `core/src/landscape.cpp`, `docs/keys.md`: the substrate defaults;
  - `core/include/nova/replay.h`, `core/src/replay.cpp`, `core/src/run.cpp`, `core/include/nova/snapshot.h`, `core/src/view.cpp`, `app/main.cpp`, `core/CMakeLists.txt`: replay v3 and the legacy text;
  - `tests/test_tiers.cpp`, `tests/test_substrates.cpp`, `tests/test_replay.cpp`, `tests/test_detect.cpp`: the tests;
  - `docs/results/world.md`, `docs/results/disk.md`: pointers to this note, and the defaults change.
- Changed in the disk build's commit, resolving its conflicts:
  - `core/include/nova/snapshot.h`, `core/include/nova/soup.h`, `core/include/nova/graph.h`;
  - `core/src/soup.cpp`, `core/src/run.cpp`, `core/src/world.cpp`, `core/src/landscape.cpp`;
  - `tests/CMakeLists.txt`, `tests/test_main.cpp`, `tests/test_snapshot.cpp`, `tests/test_substrates.cpp`, `tests/test_tiers.cpp`;
  - `docs/keys.md`.

## Round 2: the lineage tracker, the disasters, the configuration page and the analysis

Four more approved builds, cherry-picked in this order, each kept as its own commit with its own message, then one integration commit:

1. `Lineage tracker: link by descent, fix the dormancy leak; founders by origin` (from `wt/lineage`, `887bee9`; `docs/results/lineage.md`): disasters plan X12 and world plan W7.
2. `Natural disasters: surges, crashes, burnouts, disk failures and storms` (from `wt/disasters`, `ab13e81`; `docs/results/disasters.md`): X1–X11.
3. `The configuration page; runs' settings fixed from their start` (from `wt/configui`, `3161aae`; `docs/results/configui.md`): config plan C2–C5.
4. `Offline analysis: novelty meter, family trees and settings search` (from `wt/analysis`, `7f12aa4`; `docs/results/analysis.md`): analysis plan A0, A2, A4, A1.
5. `Integrate lineage, disasters, configuration page and analysis`: the fixes below, their tests, and this section.

Every conflict was resolved inside the commit that brought it, and each of the four commits passed `verify.ps1` on its own before the next went on (42, 43, 43 and 44 tests, all green).

### Conflicts and how they were resolved

**The lineage tracker** (based on `98c2a3c`, before the registry):

| where | what clashed | resolution |
|---|---|---|
| `core/src/lineage.cpp` | its `detect_params_from_config` (raw getters with their own range checks) against the registry's rewrite | the registry's readers kept. `detect.shift` is a new `keys.inc` entry: whole, default 8, range 0–64, group Detection and lineages, needs `detect.enabled`, read with `key_u32`. Its test ("accepts 64, rejects 65") passes on the registry's range check |
| `stats.h`, `stats.cpp` | both builds put their columns after `resting` | the archive's three first (merge order), then `founded_spark`, `founded_daughter`, `founded_arrival`, `founded_wake` |
| `view.cpp` | `stats_column_is_sum`, and `RunArchive::load_stats`' field buffer | both: the archive's sums (91, 92) and the founded columns (then 93–96); one `kMaxFields` buffer |
| `test_view.cpp`, `test_dispersal.cpp` | the header strings | the merged header; the column-index checks moved to 93–96, with the archive's gauge and sums checked too |
| `docs/keys.md` | regenerated (286 settings) | |

Everything else merged by itself: the origin byte and `CargoTag` in `soup.h`, the hooks in `soup.cpp` and `interp.cpp`, `founder_origin` in `run.cpp`, and the new tests.

**The disasters:**

| where | what clashed | resolution |
|---|---|---|
| `snapshot.h` | a third v4 | still one v4. The comment lists every v4 part: the substrate levels, the archive in the disk section and in cargo, and the disasters' offline cores and world state; and the legacy defaults for older snapshots |
| `soup.h`: `SoupParams` | `disasters` beside the varied world's block | both |
| `soup.h`: `SoupTally` | both appended after `dormant_hops` | the archive's two, then the disasters' 22 (the stats code indexes them in that order) |
| `soup.h`: `Dormant` | both claimed the padding word | see "The `Dormant` decision" below |
| `stats.h`, `stats.cpp`, `view.cpp` | the disasters put their 26 columns after `ambient`, in a separate `StatsRow::x` | moved before `compress` and `ambient`, which are last again. `StatsRow::x` is gone; the 26 are ordinary integer columns (`kStatsIntColumns` 123, `kStatsColumns` 125, `kStatsDisasterFirst` 97). `stats_column_is_sum` covers their 22 sums (97–118); the four gauges (119–122) aren't. `RunArchive::load_stats` reads them like any column, by name. The viewer's chart list comes from `kStatsColumns` and the names, so it follows |
| `stats.cpp`: `nodes.csv` | both added columns after `born` | the substrates' eight and `occupancy` first, then `symptom_ticks`, `struck`, `cores_offline` (41 columns; the substrates suite's file test now reads 41) |
| `soup.cpp`: `Soup::init` | the origin array and the disasters' resets | both |
| `soup.cpp`: `go_dormant` | the archive's reserve split against the disasters' grace and flags | both: a burnout victim's memory reserve is its grace when bare, the disk reserve splits into fast and archive, and the flags are set |
| `soup.cpp`: end of tick | the archive's rot and decay against the disasters' weather hazards | the archive's draws, then the weather's, both after every earlier draw |
| `world.cpp` | `is_structural` | `substrate.`, `archive.` and `disaster.` |
| tests | `CMakeLists.txt`, `test_main.cpp` (the suites and the fallback), `test_snapshot.cpp` (the future version) | all suites; the fallback gains `disaster.enabled = false`; the computed future-version check kept |

Also in that commit, to keep it green:

- `disaster.enabled = false` joined `legacy.h`'s `kLegacyFormatDefaults`, so v3 snapshots and v2 replay logs keep the disasters off. Without it the old binary's embedded log diverged.
- The tiers suite's `test_old_snapshot` world now sets `disaster.enabled = false` too: a v3 build had no disasters.
- The substrates suite's `test_old_snapshot` drops all four switches from its resumed run's fallback, and checks the disasters come back off.
- The replay suite's explicitly-off world also turns the disasters off, and its legacy-config checks count lines from the text rather than hard-coding three.

**The configuration page:**

| where | what clashed | resolution |
|---|---|---|
| `core/CMakeLists.txt` | `disaster.cpp` against `check.cpp` | both |
| `world.cpp` | the disk folder: the archive's condition against `!check_only_` | both: `disk > 0 \|\| archive > 0`, and never in check-only mode |
| `test_replay.cpp` | the legacy-log tests against its old knob-log test | all of them |
| `docs/keys.md` | regenerated (348 settings) | its help-string edits for the locked knobs kept |

`keys.inc`'s help strings merged by themselves: the seven locked soup knobs no longer say "a live knob". The demo world's four still do, as the configuration build left them. Its own v2 knob log still replays to every hash: it is a v2 log, so it gets the legacy defaults, which match the build that recorded it.

**The analysis:**

| where | what clashed | resolution |
|---|---|---|
| `app/main.cpp` | the header comment | both: the analysis commands and the accepted-keys rule |
| `core/CMakeLists.txt` | the source lists | all sources |
| `tests/CMakeLists.txt`, `test_main.cpp` | the suites | all suites. One `add_dependencies(nova_tests nova)` serves both the viewer suite and the analysis suite |

Its CSV readers find columns by name, so the moved columns need nothing.

### The `Dormant` decision

`Dormant` is hashed and saved as its raw 48 bytes, so its layout is part of the snapshot format. The disk tiers turned its padding word into `archive_reserve`; the disasters put their burnout flags in the same word.

- **Kept:** `archive_reserve` in the old padding word.
- **Moved:** the flags into the byte after `kind`:
  - `kind`, a `u32`, became `u8 kind; u8 flags; u16 spare;`;
  - `kind` only ever holds 0 or 1 (`kKindP`, `kKindE`), so in what was a little-endian `u32` those three bytes were always 0;
  - the flags became `u8` (`kDormantBurnt`, `kDormantGraced`).
- **Nothing else moved.** `static_assert`s pin the size (48) and the offsets: `kind` 36, `flags` 37, `hops` 40, `archive_reserve` 44.
- **Why this is safe:**
  - With no archive and no burnout, every byte of every `Dormant` is what it was before either build. So the dormant hash is unchanged: every pinned and gate hash holds, dormancy's included.
  - A v3 snapshot's `Dormant` arrays load unchanged: its `kind` word reads as `kind`, with `flags` and `spare` 0.
  - The tiers suite's v3 snapshot test (a world with dormancy) resumes exactly.
- **What it costs:** the disasters build's own v4 layout, with the flags in the padding, was never released, so no file depends on it.

### The integration commit

- **X5, one tier chosen by size.**
  - `Soup::fail_disk(Rng& r, bool* archive)` now takes the archive with chance archive / (disk + archive), as `r.below(disk + archive) < archive`.
  - `r` is the disaster's own stream, `dis_strike`'s, keyed by the episode's serial.
  - It then calls `fail_disk_tier`.
  - It draws only when the node has both tiers. So a world without an archive draws nothing new, and the disasters suite's numbers and hashes are unchanged; `fail_disk_tier` on a lone fast disk does exactly what the old `fail_disk` did.
  - A node with an archive alone fails it. `dis_strike` no longer skips a node with no fast disk but an archive.
  - The event gains ` tier=fast` or ` tier=archive` where a node has both.
  - The failing-disk symptom's extra rot covers both tiers, since the tier is drawn at the strike.
  - Tests (suite `disasters`, `test_diskfail_tiers`):
    - 4,000 failures of one soup: 1,008 on the archive at 768 fast to 256 archive (1,000 expected), and 2,026 at 256 to 256 (2,000 expected). Each is within 4 standard deviations.
    - One failure of each tier leaves the other's codons and holdings exactly as they were.
    - In a world with a 1,024-codon disk and a 256-codon archive: 40 fast and 11 archive failures, each one's tier INERT and unheld after.
- **The fit check's sums, shared.**
  - `world_footprint` (`budget.h`) is now the one place the world's RAM and disk-file sizes are summed: soups at `kRamPerCodon`, plus the fast disk and the archive at `disk_bytes_per_codon`, in RAM or in `disk.path`'s files.
  - `resolve_budgets`' fit check and the setup page's check (`check.cpp`) both call it, so the page counts the archive, as the fit check has since the disk tiers, and the two can't drift again.
  - The page shows the archive beside the fast disk.
  - Test (suite `viewer`): with a 1,000-codon disk and a 500-codon archive, the page's RAM is the soups plus 1,500 codons a node, equal to `world_footprint`'s; on the engine's defaults the archive is a quarter of the disk (250).
- **`same_land`:** none of the merged builds added a `LandParams` field. The landscape cache's key now also holds the substrates' switch and settings, because the summary counts substrates.
- **The check sees every new setting.**
  - Every read the merged builds added happens in `World::init` before `if (check_only_) return true;`:
    - `spark.activity`, `world.archive`, `archive.*` in `soup_params_from_config`;
    - `detect.shift` in `detect_params_from_config`;
    - `res.archive.*` in `read_spreads`;
    - `substrate.*` in `read_substrates`;
    - `disaster.*` in `read_disasters`.
  - Nothing new opens a device or a folder in check-only mode. The disk folder honours `check_only_` with the archive's condition; the disasters and substrates open nothing.
  - Test (suite `viewer`): twelve of the new settings come back with effective values, and `detect.shift = 65` and `disaster.reboot_min = 200` come back as problems by setting.
- **The landscape summary** gives the distinct substrates, as `nova land` does, or "off". Test: on a 150-node landscape the page's count equals a built world's (141), and follows `substrate.smooth` (150 at 0.3) through the cache.
- **Unread keys.** `key_accept_known` covers the disasters' keys behind `disaster.enabled`. Test (suite `disasters`, `test_unread_keys`):
  - a world with the disasters off that names every other `disaster.*` setting leaves them unread;
  - `key_accept_known` then accepts them;
  - a misspelt `disaster.surge_evry` still fails.

  End to end, the viewer suite already runs `nova.exe run` on a page's `run.cfg`, which names every setting, the disasters' included, with the disasters off.
- **Everything at once** (suite `disasters`, `test_everything_merged`): a 48-node landscape with every new feature on:
  - substrates and sparks by activity;
  - the archive (spread with barren sites);
  - every disaster frequent, with symptoms and sounds;
  - dormancy, dispersal and the lifecycle;
  - the detector linking by descent.

  Over 300 ticks:
  - 48 distinct substrates, and up to 538 archive codons held;
  - 1 surge, 6 crashes, 8 burnouts, 8 disk failures (2 of the archive), 23 strikes and 43 episodes;
  - 46 lineages founded and 183 inheritance links.

  It passes these checks:
  - the same hash at 1, 4 and 20 threads and under shuffled dispatch;
  - the same 210 events, the detector's and the disasters', in the same order;
  - a snapshot taken during a reboot (tick 95, 1 node rebooting) resumes on 20 threads to the same hash.

  The substrates suite's everything-on world runs on the engine's defaults, which now include the disasters and the archive. It still matches at 1, 4 and 20 threads, shuffled, through a snapshot, and on the RTX 5080.
- **Old replay logs.** The embedded v2 log from the `4daa357` binary still replays to its hashes (suite `replay`). `nova replay` on that log with this build: "replay OK: 6 checkpoints match".
- `docs/keys.md` was regenerated and is unchanged by the integration commit (348 settings).

### Verify

`powershell -NoProfile -File .claude\verify.ps1` on the integration commit: 44 tests (43 suites and the include check), all green. It ended:

```
100% tests passed, 0 tests failed out of 44

Total Test time (real) = 113.22 sec
verify: clang-format not found in C:\Program Files\Microsoft Visual Studio\18\Community (advisory step skipped)
verify: OK - green in 114,628 ms
```

The merged everything-on world's hash in that build is `295095e639e94809` (suite `disasters`). The substrates suite's engine-default world is `ef48a94c61e13fc5`, the same on the RTX 5080 (160 of 160 rounds verified).

### Doubtful or open

- **Disk-failure symptoms don't say which tier.** The failing-disk rot covers both tiers, and the tier is drawn at the strike. If the symptom should point at the tier that will fail, the draw belongs at the episode's start: a keyed draw either way, but a change to the plan's wording.
- **The `Dormant` flags moved.** They no longer sit where the disasters build put them. Anything outside this tree reading raw `Dormant` bytes, in that build's own snapshots, wouldn't match; there is none that I know of.
- **The stats columns moved for the disasters.** Their 26 columns now sit before `compress` and `ambient`, at 97–122. Readers by name (the viewer, `RunArchive`, the analysis) are unaffected. A reader by position written against the disasters build's layout would be.
- **The merged everything-on test uses nearest selectors,** so the seeded replicator links and lineages form. Random code then rarely runs DISK, so the archive shows up there as holdings and as failures, not as DISK ops. The tiers suite's shuffled world covers the archive's DISK ops.
- **Not updated:** `docs/design.md` (none of the builds updated it) and the README's sections for the disasters and the analysis.