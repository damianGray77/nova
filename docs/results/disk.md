# Results: disk tiers (the archive)

`docs/world-plan.md` revision 2, call W4 (with W9's keys, snapshot and determinism rules), approved by Damian. Built 2026-11-07 on branch `wt/disk`, from the commit that added the settings registry. Nothing is committed.

## What was built

- **The fast disk** is today's disk, unchanged: `world.disk`, `disk.rot`, `disk.decay`, `disk.cost_base`, `disk.cost_per_codon`.
- **The archive** is a second tier on every node, in the same ledger as the fast disk:
  - Codons `[world.disk, world.disk + world.archive)` of the node's disk arrays are the archive (`DiskLedger::fast` marks where it starts). An extent's tier is where it lies, and no extent straddles the boundary. Each tier has its own free count (`DiskLedger::afree`; `free` counts both).
  - Its size is `world.archive`, by default a quarter of `world.disk` (1,024 at the default 4,096). 0 means no archive, which is the legacy world.
  - In a landscape it is spread by its own family, `res.archive.spread|smooth|barren|hotspots|hotspot_boost|against`, with its own noise field (`kFieldArchive`, added last), like `res.disk.*`. A site's share is fixed for every life of the site. `against` gains the option `archive`. In procedural worlds the archive doesn't vary (`vary.disk` varies the fast disk only).
  - It rots at `archive.rot`, by default a hundredth of `disk.rot`, and its fossils decay at `archive.decay`, by default a hundredth of `disk.decay`. Both are drawn after every earlier draw of the tick, so worlds without an archive are unchanged.
  - It is slow: every DISK operation on it costs `archive.slow` (10) times the fast disk's, for the access and for each codon.
- **Holding it:**
  - ALLOC's new resource selector 6 claims archive only (one run of CX codons; AX gets its offset in the body's whole disk). Selector 3 claims fast disk only. `res_n` is 7 when the world has an archive; without one it and the shuffled control's draws are exactly as before.
  - RELEASE 3 gives up the newest CX codons of the body's fast disk, RELEASE 6 those of its archive. Later extents then address lower, since a body's disk is its extents in claim order.
  - DIVIDE hands over the newest share of each tier (share/64 of each), contents and all. The daughter's extents keep the parent's claim order, in one detach pass.
  - ENGULF takes all of the prey's disk, each extent keeping its tier.
  - Death frees both tiers. Their contents stay as fossils, stamped with the dead program's genome length (6.10 step 4), and loading archive fossils is salvage, as on the fast disk.
  - Dispersal releases each tier's share at home (fossils, stamped) and the cargo carries both counts (`Departure::archive`, `Cargo::archive`). On arrival she claims each in its own tier, empty, capped by what's free there: the fast disk first, then the archive.
  - Dormancy (6.17): see below.
- **Addressing:** a body's disk is all its extents in claim order, addressed from 0 whatever their tier. Compare, store and load work across mixed holdings unchanged (`DiskLedger::addresses` never cared about tiers).
- **DISK costs** (`Exec::disk_scale`):
  - Each tier has a factor. The fast disk's is `1 / disk_speed`, the archive's `archive.slow / archive_speed`.
  - An operation costs its access times the factor of the slowest tier it touches, plus each codon times its own tier's factor.
  - classic: `disk.cost_base` is the access, `disk.cost_per_codon` the codon.
  - home_v1: the kernel call (not scaled), then `disk_read`/`disk_write` as the access and `disk_read_codon`/`disk_write_codon` per codon. An operation longer than a tick is an action as before.
  - A range outside the body's disk (which then fails) is priced at the fast disk's, as before tiers.
  - Without an archive and at `disk_speed` 1 the old expressions are used verbatim.
- **Substrate speeds:** `SoupParams::disk_speed` and `archive_speed` (floats, set to 1 in `soup_params_from_config`; nothing else sets them yet). They divide each tier's access and per-codon costs. The world agent adds the same two fields for substrates. (At the integration one pair was kept, as doubles, and the world sets them from each node's fast-disk and slow-disk levels: `docs/results/integration.md`.)
- **Dormancy (6.17):**
  - The disk reserve counts both tiers, kept apart. `Dormant::archive_reserve` takes the struct's former padding, so `Dormant` stays 48 bytes and hashes as before without an archive.
  - The clock spends the memory reserve, then the fast disk's reserve, then the archive's, each at `dormant.disk_loss`. The world's transit clock (`World::deliver_dormant`) spends them in the same order.
  - Waking claims each tier it held, empty, up to its remaining reserve, capped by what's free in that tier.
  - A bite of a resting program takes the same share of both disk reserves.
- **`Soup::fail_disk_tier(bool archive)`,** for disasters X5 as amended by W4. One tier's holdings are all released, in schedule order. Resting dormant programs lose that tier's reserve. Every codon of the tier becomes INERT (codon 0), fossils included, with stamps cleared. It draws nothing and returns the codons lost. Nobody dies directly; a resting program left with no reserve at all expires at its next clock tick. The tier sizes for "chosen by size" are `disk().fast` and `disk().size() - disk().fast` (the node's `params().disk` and `params().archive`).
- **Disk in files:** with `disk.path` set, the archive lives in the same four files per node as the fast disk, after it, so it is memory-mapped the same way. The budget's fit check counts the archive.
- **Stats:**
  - New columns `archive_held` (a gauge), `archive_stores` and `archive_loads` (sums: DISK stores and loads that touched the archive, also counted in `disk_stores` and `disk_loads`).
  - They sit after `resting` and before `compress` and `ambient`. The two floating-point columns stay last, as every phase since 8 has done.
  - The charts treat the two sums as per-tick rates.
- **SENSE:** no new reading. SENSE 15 (my disk) reads the whole disk, both tiers: the range DISK addresses. SENSE 16 (free disk) reads the free fast disk.
- **Snapshots:** format v4.
  - The archive's contents, owners, extents and stamps are in the existing disk section (its arrays now hold both tiers); `afree` is rebuilt on load. A cargo carries its archive count; a resting program's archive reserve is in `Dormant`.
  - v3 and earlier load with the archive off: `Run::init_from_snapshot` sets `world.archive = 0` when the embedded config doesn't name it. They ran without one, and on the new defaults the disk section would be the wrong shape.
- **Keys** (registry, `Disk` group): `world.archive`, `archive.rot`, `archive.decay`, `archive.slow`, and the `res.archive.*` family (6 keys). `archive.*` is structural (not a live knob). `docs/keys.md` is regenerated (275 settings).
- **Test runner:** the legacy fallback sets `world.archive = 0`, so every existing suite runs as before.

## Tests (suite `tiers`, `tests/test_tiers.cpp`)

- **Keys:** with no fallback, an archive of 1,024 for the default 4,096 of disk, following `world.disk`; rot and decay a hundredth of the disk's; slow 10; speeds 1; `res_n` 7. With the legacy defaults the archive is off and `res_n` 4. The shuffled control's maps are byte-identical with the archive off and a permutation of all seven resources with it on. `archive.slow` below 1 is refused.
- **Selectors:**
  - ALLOC 6 claims archive only, ALLOC 3 fast disk only, over many mixed claims; offsets are in the whole disk.
  - A claim bigger than the free archive fails.
  - SENSE 15 reads the whole disk, SENSE 16 the free fast disk.
  - RELEASE 6 and 3 give up only their own tier's newest codons; with none left RELEASE fails.
  - Without an archive BX 6 fails.
  - An archive alone (no fast disk) works: BX 3 fails, BX 6 and DISK work.
- **Mixed holdings:** a store over fast, archive and fast again writes exactly what it should; a compare finds two damaged archive codons; a load across the tiers reads them back. Archive stores and loads are counted only when the archive is touched.
- **Costs (classic):** 8 codons cost 6 on the fast disk, 60 on the archive, and 51 for 4 of each (the archive's access, each codon at its own price). A fast disk at speed 2 costs 3; an archive at speed 2 costs 30.
- **Costs (home_v1, tick 4.75 µs):**

  | op, 8 codons | ticks |
  |---|---:|
  | fast store | 3 |
  | archive store | 26 |
  | archive load | 97 (about 100 ticks, as the plan says) |
  | mixed load (4 + 4) | 97 |

  Each lands at its end.
- **Hazards** (256 codons of each tier):

  | | fast disk | archive |
  |---|---:|---:|
  | rot over 100 ticks at `disk.rot` 0.01: codons changed | 149 | 1 |
  | the same with `archive.rot` 0.01 set outright | 149 | 159 |
  | fossil decay over 20 ticks at `disk.decay` 0.05: bits lost | 214 | 1 |
- **DIVIDE** (share 16/64 of 32 fast and 32 archive): the daughter gets the parent's newest 8 fast and newest 8 archive codons, in that order, by address. **ENGULF** (a whole swallow) takes archive then fast in the prey's claim order.
- **Death:** 72 codons over both tiers are freed and keep their contents, with stamp 8 (the genome length). A two-codon program that claims those archive fossils and loads 32 of them gets 8 to 32 garbled (a quarter come through intact at K = 1).
- **Dispersal:** a share of 32/64 of 32 fast and 16 archive releases 16 and 8 at home, as fossils. On a node with 5 archive codons free, she arrives with 16 fast then 5 archive.
- **Dormancy:**
  - The reserves are 8 fast and 8 archive, with nothing held.
  - With every core taken, the clock at 4 a tick goes 4/8, 0/8, 0/4, then it expires: fast first.
  - Waking on its own freed cores, it claims 16 fast and only 4 archive, where 4 were free.
  - A bite of half a resting program halves both reserves.
- **`fail_disk_tier`:**
  - The fast disk's failure makes all 256 codons INERT with stamps cleared and frees its holdings. The archive keeps every codon, every stamp and its holdings. A resting program loses its fast reserve only.
  - Then the archive's failure wipes the archive.
  - Without an archive, an archive failure does nothing and the fast disk's takes everything.
- **Files:** a soup with `disk.path` holds both tiers in its files (320 codons, the archive from 256) and hashes the same as one in RAM after stores and 20 ticks.
- **A busy world:**
  - The world: a 48-node landscape, classic costs, the archive spread with a fifth of sites barren of it, dispersal, dormancy, the lifecycle (collapse and ruins), salvage and the shuffled control.
  - Site archives sum exactly to 256 × nodes, with barren sites.
  - Over 300 ticks: 99 archive stores and 2 archive loads (of 199 disk stores), archive held up to 6,786 codons, 414 bodies dormant and 365 woken, 1 collapse and 1 recovery.
  - The same hash at 1, 4 and 20 threads and under shuffled dispatch, and through a snapshot taken at tick 150.
- **Off is unchanged:**
  - `world.archive = 0` set outright hashes as the legacy world, and 256 changes it.
  - A v3 snapshot (version patched to 3, config without `world.archive`) of a world on every engine default loads with the archive off and resumes exactly.

## Measurements

Release builds in scratch folders, 4 threads, single runs on the shared machine, so the timings are rough. The base build is the commit this branch started from, built from `git archive HEAD`. No real GPU or NPU was used (`accel.gpu = cpu`, `accel.npu = cpu`, `warp.device = cpu`, or instant mode).

**With the archive off, every world is the base's world exactly.** The same final hash from the base build and from W4 with `world.archive = 0`:

| world | ticks | hash (both builds) | what the disk did |
|---|---:|---|---|
| procedural, classic, shuffled control, 40 nodes | 400 | `3a1bcbd02eb69f50` | dormancy, salvage |
| landscape on every engine default (home_v1, warps, 60 nodes) | 300 | `8085128fe33a45d8` | 665 codons held, 118 sent, 146 dormant |
| procedural, classic, disk 1,024 varied, salvage, lifecycle, nearest | 400 | `1f0f39022bdfad38` | 4,888 codons held, 646 sent, 1,102 dormant, 2 collapses and recoveries (ruins) |
| the same, shuffled control | 400 | `d0b49c248a65d5a0` | 861 DISK stores, 28 loads, 124 compares, 1,917 sent, 397 dormant, 1 collapse and recovery |

That covers what the refactored ledger touches without an archive: DIVIDE's share, dispersal's release and claim, dormancy's reserve and wake, death fossils, rot, decay and ruins.

**Speed** (whole run, ms):

| world | base | W4, archive off | W4, archive on (default size) |
|---|---:|---:|---:|
| landscape on every default, 300 ticks | 3,911 | 3,383 / 3,431 | 3,515 |
| procedural disk world, nearest, 400 ticks | 245 | 214 | 225 |
| the same, shuffled | 253 | 265 | 277 |

The differences are within run-to-run noise. The archive adds two range scans of rot and decay a tick, a tier count per DISK op, and an extent walk per DIVIDE, dispersal and dormancy.

**The archive in use** (W4, archive on):
- On the default landscape, 594 archive codons were held at tick 300. There were no DISK ops at all in 300 ticks: on home_v1 a DISK op is an action of 3–100 ticks, and random code rarely makes one.
- In the shuffled procedural disk world: 786 archive codons held, 125 DISK stores, 1 archive load.
- So the archive is reached by random code, mostly as holdings (and dormancy reserves). Using it takes selection.

## Settled details

1. **One ledger, two ranges** rather than a second ledger. A body's claim order across tiers comes for free (one extent chain), and so do the files, the snapshot section, stamps, fossils, ENGULF and death. Each tier's claims stay inside its range: `alloc_run` and `claim_any` take the tier. Rot and decay run per range.
2. **RELEASE is per tier** (3 the fast disk, 6 the archive), like ALLOC, so each holding keeps its tier. Releasing fast disk claimed before archive shifts the archive's offsets down, as releasing any middle extent would.
3. **DIVIDE and dispersal share each tier separately,** share/64 of each, as millicores share each kind. DIVIDE keeps the parent's interleaving. Dispersal's arrival claims the fast disk first, then the archive (the cargo carries counts, and the claims are empty).
4. **Waking keeps the tiers it held,** each capped by what's free of that tier. An archive reserve is never turned into fast disk, nor the other way round: the plan's "claims disk up to its remaining reserve" was read per tier.
5. **The dormant clock spends the archive at `dormant.disk_loss`,** the same rate as the fast disk, last. No separate key.
6. **`archive.decay` is a key the brief didn't list** (inside W9's `archive.*`): near-permanent should hold for the archive's fossils too, and without it they would fade at the fast disk's rate. Default a hundredth of `disk.decay`.
7. **The kernel call isn't scaled** by `archive.slow` or the speeds; only the disk's own access and codons are. So an archive read is about 96 ticks of disk access plus the call, matching the plan's "about 100 ticks".
8. **`disk_rot` and `decayed` count both tiers.** `disk_held` keeps its meaning (fast disk held); `archive_held` is the archive.
9. **INERT on failure is codon 0** (INERT under nova_v2, the default table), as decay uses.
10. **An archive alone is a disk:** a node (or world) with `world.archive` > 0 and no fast disk has a ledger, SENSE 15–16 and DISK. ALLOC and RELEASE 3 fail there, as 6 fails on a node barren of archive.

## Deviations and open points

- **`budget.disk` still sizes only `world.disk`.** The archive (a quarter more by default) comes on top in the same files. The fit check now counts it and refuses a world that won't fit, naming `world.archive`. The registry can't peek a computed key, so the budget can't tell a defaulted archive from a set one, which leaving room for it would need. The help text says so.
- **Replay logs written before W4** replay with the archive on, because their config doesn't name it and the replay format has no version to key on (snapshots have one, and are handled). A pre-W4 replay of a world with a disk won't verify. The world and disasters agents' new defaults have the same problem; one shared fix at merge would cover all of them (for example a replay-format bump that turns the new features off for older logs). **Fixed at the integration:** new logs are `nova-replay 3`, and a v2 log takes `world.archive = 0` (with the other legacy defaults) for what it doesn't name: `docs/results/integration.md`.
- **No viewer or inspector changes:** W8 is the configuration page's and the viewer's step. The inspector's disk figure is the whole disk.
- `docs/design.md` isn't updated (a results note only, as asked).

## Merge notes (shared spots)

All shared-spot edits are compact blocks marked "disk tiers" or "W4":
- `soup.h`: the `SoupParams` block after `disk_rot` (`archive`, `archive_rot`, `archive_decay`, `archive_slow`, `disk_speed`, `archive_speed`); `SelectorMap::res[7]`; `DiskLedger` (`fast`, `afree`, the tier methods); `Ruins::disk_fast`; `SoupTally::archive_stores|archive_loads` at the end; `Departure::archive`; `Dormant::archive_reserve` (was `pad`); the `Soup` disk-tier block (`disk_on`, `free_archive`, `archive_held`, `fail_disk_tier`); `arrive(..., archive)`.
- `soup.cpp`: `go_dormant` (three lines), `wake` (two), `rest_tick` (two), `depart_dormant` (one); hash, save, load and check switch `p_->disk` to `disk_on()`.
- `world.h` and `world.cpp`: `Cargo::archive` (+ `pad`, 112 bytes), its hash/save/load (v4), `deliver_dormant`, `spreads_[8]` and `share_archive_`, `derive_node`, `read_spreads`, `spread_resources`.
- `graph.h`: `kFieldArchive` before `kFieldCount` (the world agent's substrate fields go there too).
- `stats.h`, `stats.cpp`, `view.cpp`: three columns after `resting` (the lineage agent's `founded_*` go there too); `kStatsColumns` 95, `kStatsIntColumns` 93.
- `snapshot.h`: `kSnapshotVersion = 4` (shared with the others); `run.cpp`: the v<4 block.
- `keys.inc`, `docs/keys.md`, `tests/test_main.cpp` (`world.archive = 0` in the legacy defaults).
- Header tests updated: `test_dispersal.cpp`, `test_view.cpp`; `test_keys.cpp` (48 spread keys); `test_snapshot.cpp` (the future version is now computed).

## Files touched

- `core/include/nova/soup.h`, `core/src/soup.cpp`: the ledger's tiers, params, dormancy, arrival, death, hazards, hash/save/load/check, `fail_disk_tier`.
- `core/src/interp.cpp`: ALLOC/RELEASE 6, DISK across tiers and its costs (classic and timed), DIVIDE, dispersal, ENGULF, bites of resting programs, SENSE 15–16.
- `core/include/nova/world.h`, `core/src/world.cpp`: cargo, transit clock, the `res.archive` spread, structural keys, disk files.
- `core/include/nova/graph.h`: `kFieldArchive`.
- `core/include/nova/stats.h`, `core/src/stats.cpp`, `core/src/view.cpp`: the three columns.
- `core/include/nova/snapshot.h`, `core/src/run.cpp`: v4, older snapshots without an archive.
- `core/src/budget.cpp`: the fit check counts the archive.
- `core/src/keys.inc`, `docs/keys.md`: the keys.
- `tests/test_tiers.cpp` (new), `tests/CMakeLists.txt`, `tests/test_main.cpp`, `tests/test_dispersal.cpp`, `tests/test_view.cpp`, `tests/test_keys.cpp`, `tests/test_snapshot.cpp`.
- `docs/results/disk.md` (this note).
