> Historical handoff. The run was stopped cleanly at tick 672142 on 2026-11-07; completion details are in results/final-integration.md.

# Handoff: finishing the Nova build (for Codex)

Written 2026-11-07 by Claude for Damian. **Nova** is Damian's Tierra-inspired artificial-life engine: C++20, MSVC, CMake + Ninja, Windows. `docs/design.md` is the source of truth. Most of a large, approved build is done and merged on a local branch. What's left is three part-finished features, one merge, presets, docs, and the commit and push.

## 1. Damian's rules (non-negotiable)
- **No C++ STL and no third-party libraries.** Only the headers `tools/check_includes.cmake` allows. Use the project's own containers (`core/include/nova/array.h`, `str.h`, `hashmap.h`, `io.h`, `rng.h`). Any new header or dependency is discussed with Damian first.
- **No Python.**
- **Determinism:**
  - the same world hash at 1, 4 and 20 threads and under shuffled dispatch;
  - every random draw comes from keyed streams in a fixed order;
  - snapshots carry all state and resume exactly;
  - the CPU reference and the GPU warp kernel agree word for word.
- **Defaults:** new features are on in the engine defaults and off in the test runner's legacy fallback defaults (`tests/test_main.cpp`, `Config::set_fallback`), so every existing pinned and gate hash stays the same.
- **No new SENSE readings,** unless Damian asks.
- **Settings registry:** every setting goes in `core/src/keys.inc` (how to add and read one is explained at the top of `core/include/nova/keys.h`).
  - Read settings with `key_*`, in `World::init` / `soup_params_from_config` before the `check_only_` return, so the configuration page can check them.
  - Regenerate `docs/keys.md` with `build\bin\nova.exe keys --md docs/keys.md` (the `keys` suite checks it's current).
- **Old logs and snapshots:** they must keep their old behaviour. Add each new feature's off value to `core/include/nova/legacy.h` (`kLegacyFormatDefaults`).
- **Git:**
  - Commit as Damian: `git -c user.name=Damian -c user.email=damiangray@gmail.com commit ...`.
  - Damian has approved committing and pushing this build once it's finished and green.
  - Never commit `nova_test_gpu.replay`, a stray file in the main tree.
  - Git isn't on PATH. Use `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd\git.exe`.

## 2. The running simulation: don't touch it
- `nova.exe` (pid 30352, `runs\long1`) is running from `E:\Projects\nova\build-release`.
  - Never kill it, never write under `E:\Projects\nova\runs`, and never build into `E:\Projects\nova\build-release` (its exe is locked).
- **To stop it** (only when Damian says): create the file `E:\Projects\nova\runs\long1\STOP`. That ends the run cleanly and writes a final snapshot.
- **The 20-minute update job** has been cancelled.

## 3. Building and testing
- Run `powershell -NoProfile -File .claude\verify.ps1` from a worktree root. It configures and builds Debug into `<worktree>\build`, then runs every CTest suite plus the include check (about 2 minutes of tests). Green means: `verify: OK`.
- **For a Release build,** use a separate folder in the worktree (for example `build-rel`), configured as `tools/build.ps1` does: `-DCMAKE_BUILD_TYPE=Release -DNOVA_BUILD_TESTS=OFF`. Don't use `E:\Projects\nova\build-release` while long1 runs.

## 4. Where things are
- **Plans,** all approved by Damian. They're untracked, in the main tree's `E:\Projects\nova\docs\`:
  - `world-plan.md` (revision 2);
  - `disasters-plan.md` (revision 6);
  - `config-plan.md` (revision 1);
  - `analysis-plan.md` (revision 1);
  - this file.

  They must be copied into the repo and committed with the build (step 5.6).
- **Branches** (the main tree is on `main` at `98c2a3c`; nothing pushed since):
  - **`next`**: the integration branch, checked out in `E:\Projects\nova-wt\next`. All 44 suites green at `9f91bd2`:
    ```
    9f91bd2 Integrate lineage, disasters, configuration page and analysis
    29f7b8a Offline analysis: novelty meter, family trees and settings search
    0859436 The configuration page; runs' settings fixed from their start
    ff1b05a Natural disasters: surges, crashes, burnouts, disk failures and storms
    53708ed Lineage tracker: link by descent, fix the dormancy leak; founders by origin
    1f1994a Integrate substrates with disk tiers; smoother substrate map; old replay logs keep their defaults
    fb9480f Disk tiers: an archival tier beside the disk
    6ad8e38 Substrates, sparks by activity and resource speed multipliers
    4daa357 Settings registry: one entry per setting, read through by the engine
    98c2a3c (main, origin/main) Dormancy and drift; ...
    ```
  - **`wt/*`**: one worktree per feature under `E:\Projects\nova-wt\`. The finished ones (`registry`, `lineage`, `world`, `disk`, `disasters`, `configui`, `analysis`) are already on `next` and can be removed after the push.
- **What was built:** each feature's results note is in `docs/results/<name>.md` on `next`, plus `docs/results/integration.md`, which covers every merge decision.

## 5. What's left, in order

### 5.1 Finish the three part-built features
Each was stopped mid-task. The work is uncommitted in its worktree, based on `next` at `9f91bd2`. Read the worktree's `git diff` and finish the feature to its plan. Each needs:
- `verify.ps1` green;
- a determinism test (1, 4 and 20 threads, shuffled dispatch, through a snapshot);
- a results note at `docs/results/<name>.md`.

1. **`E:\Projects\nova-wt\memtiers`: world-plan W3, memory tiers.**
   - **Where:** the first `mem.fast_share` (default 1/8; 0 = off, legacy) of each node's soup is fast; the rest is slow.
   - **Prices** come from `home_v1`, by the tier of each codon touched: loads, stores, template scans and bulk moves. Fetch stays unpriced.
     - CPU: fast = nearest-cache load (755 ps); slow = the 1 MB-level load (3.6 ns).
     - GPU lanes: fast = `gpu_l1` (18.8 ns); slow = `gpu_mem` (145.7 ns).
     - Each tier's cost is divided by `SoupParams::mem_fast_speed` / `mem_slow_speed` (already set from the substrates).
   - **The warp kernel** (`warp_kernel.hlsl` / `warp_isa.inc`, DXC and FXC) must price by tier too, and with `mem.fast_share = 0` behave exactly as today.
   - **A new ALLOC selector** claims fast memory only. It must change nothing when the tiers are off: selector maps and shuffled draws stay as before.
   - **The trade-off:** fast memory's wipe chance is `mem.fast_volatility` (4) times the slow tier's.
   - **Stats:** integer columns before `compress` and `ambient`, which stay the last two.
   - **Legacy:** add `mem.fast_share = 0` to `legacy.h`.
   - **State when stopped:** the agent had written the new test suite and was waiting on `verify.ps1`, with a Release measurement still to do (instructions per tick, tiers on against off).
2. **`E:\Projects\nova-wt\profiles`: analysis-plan A3, behaviour profiles.**
   - **Sampling (observe-only):** every `profile.every` (1,000) ticks, follow `profile.per_node` (4) bodies per node for `profile.ticks` (50) ticks.
     - Record ops run, a bitmap of which genome codons ran, the actions taken (ENGULF and INJECT tried and landed, DIVIDE, dispersal, going dormant, DISK by kind and tier, EMIT, LISTEN with the values heard, GPU, NPU), holdings, lineage and founder origin.
     - Write `profiles.csv` (`output.profiles`).
     - Samples come from the observer's own stream. It's never hashed, and it must never change a world: the same hash with profiles on and off.
   - **A live role census** in the stats columns.
   - **`nova analyse roles`:** k-means from `core/src/learn/` gives named roles, a census per window, and working-length and complexity measures.
   - **The group measures** (`roles` or `groups`), each against a chance baseline:
     - moving together;
     - kin clustering in memory;
     - listening then acting.
   - **State when stopped:** the agent was adding the two `nova analyse` command functions and their dispatch in `app/analyse_cmd.cpp`.
3. **`E:\Projects\nova-wt\viewer`: the viewer, and A2's live part.**
   - **A live novelty meter:** reuse `NoveltyMeter` (`core/src/analysis/novelty.cpp`) on genebank saves, and add stats columns `novelty`, `novel_share` and `novel_success`. Setting `novelty.enabled`. Observe-only, not hashed.
   - **The viewer's "new kinds of life" list,** linked to the genome browser.
   - **The A5 map of life:** each genome's 3-codon runs, hashed into 1,024 counts, then PCA (`core/src/learn/pca.cpp`) fitted on a reference window. It needs a time slider and `--render` frames.
   - **A substrate map mode,** cycling the 8 types.
   - **`RunArchive`** loads `nodes.csv`'s `sub_*`, `occupancy` and the disasters' per-node columns.
   - **The memory strip** marks the fast region, through an accessor; wire it to memtiers' boundary at the merge.
   - **A disaster map marker,** coloured by kind.
   - **State when stopped:** the live part, the snapshot note and a render ("reference to 32 for both maps").
   - The viewer must stay drawable headless from view frames, for the `viewer` suite.

### 5.2 Merge them onto `next`
- In each worktree, commit as Damian with a clear message.
- In `E:\Projects\nova-wt\next`, cherry-pick them in this order: memtiers, profiles, viewer.
- Watch for conflicts in:
  - the stats column layout: every new integer column goes before `compress` and `ambient`, so update the header tests (`test_view`, `test_dispersal`, `test_phase4`), `view.cpp`'s sums and `RunArchive`;
  - `keys.inc`, then regenerate `docs/keys.md`;
  - `legacy.h`;
  - `tests/CMakeLists.txt` and `test_main.cpp`;
  - `interp.cpp`'s run loop (memtiers' prices against profiles' sampling).
- **Then one integration commit,** which:
  - wires the memory strip's fast region;
  - extends the everything-on determinism test (`test_everything_merged` in the disasters suite) with memory tiers, profiles and novelty;
  - confirms the embedded v2 replay log still replays.

### 5.3 Presets (world-plan W6)
In `configs/landscape.cfg`, and the other 1,000-node presets if Damian agrees:
- `world.soup_size = 32768`;
- `world.millicores = 4000`;
- disk 8192, archive 2048, and doubled GPU and NPU units, so each resource per codon stays the same.

Expect roughly half today's speed. Check `nova land` and the configuration page's memory estimate.

### 5.4 Docs
- Fold the `docs/results/*.md` notes into `docs/design.md`, as new sections after 6.17:
  - 6.18 settings registry and configuration page;
  - 6.19 lineage tracker by descent;
  - 6.20 substrates, sparks by activity and multipliers;
  - 6.21 disk and memory tiers;
  - 6.22 natural disasters;
  - 6.23 analysis.

  Follow the file's existing style: plain sentences, decisions marked "(Damian)", and findings with numbers.
- Update the build plan (section 9) and section 10's open questions.
- **README:** the start section (`nova view` setup page, `nova keys`, `nova analyse`, `nova search`) and the layout table.
- Add a short "Result" section to each plan doc.

### 5.5 Final checks
- `verify.ps1` green on `next`.
- A Release build in a separate folder that runs `configs/landscape.cfg` for a few thousand ticks with everything on, to check speed and look at `founded_*` and the disasters' columns.

### 5.6 Commit and push
- Copy the four plan docs, and this file if wanted, into `next`'s `docs/` and commit them.
- Fast-forward `main` to `next` (`next` descends from `main`), then `git push origin main`.
- Remove the `E:\Projects\nova-wt\*` worktrees (`git worktree remove`) and the `wt/*` branches once pushed.

## 6. Open questions for Damian (don't decide them)
1. **Budget estimate bug:** `budget.cpp` assumes `salvage.ratio = 0`, but the default has been 4 since Phase 10, so `budget.disk = 20GB` gives about 36 GB. Fixing it changes worlds sized by budget.
2. **Disk-failure warning symptoms** hit both tiers, because the tier is drawn at the failure. Should the draw move to the episode start, so the warning points at the doomed tier?
3. **`kDescentFloor` = 4:** a lineage link needs at least 4 matching codons. It was added during the build so 1-codon "daughters" don't link.
4. **The NPU** is priced out of use (an op costs more CPU than a typical life, and returns after 710 ticks). Damian parked this for tuning.
5. **Old replay logs from before the knob lock** still replay their knob records. New runs can't change settings mid-run.

## 7. Findings worth knowing (from long1, about 650,000 ticks)
- **No complexity gain.**
  - Genomes have held at about 32 codons since tick 50,000.
  - An 11-codon ring replicator spread to over 600 nodes from tick 254,000, and the population doubled.
  - The most capable lineage is 369.2333, a 32-codon predator-parasite, 200 generations deep on 687 nodes, with every milestone except the NPU.
- **No plateau in novelty:** the offline novelty meter finds new genotypes' surprise steady at about 2.8 bits per codon since tick 190,000.
- **No group behaviour beyond a metapopulation:** strongholds seed short-lived outposts. Signalling is essentially absent.
