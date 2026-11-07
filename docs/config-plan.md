# Plan: the configuration page

> **Revision 1, for Damian's approval (2026-11-07).** A setup page for every setting, built with `docs/world-plan.md` (revision 2) and `docs/disasters-plan.md` (revision 6). Damian's decisions are marked (Damian). The open details carry recommendations.
>
> - **Why (Damian):** every feature should be a toggle and every number a knob, with one dedicated page to adjust all of them. Then any run can switch any part off or retune it, instead of staging builds and runs.
> - **Today:**
>   - Settings live in `key = value` files, read by scattered getters, each with its own default and range written at the call site.
>   - The engine reads about 220 keys by name, plus families such as `res.<kind>.*`. The two plans add about 60 more.
>   - Only 7 can change during a run, from the viewer's toolbar.
>   - Nothing lists them all in one place.
> - **Source of truth:** `docs/design.md` 6.12 (the viewer), 7.1 B (hand-parsed configs).

## 0. Decided with Damian

- **One dedicated page** shows every setting:
  - a checkbox for each feature;
  - a knob for each number;
  - a choice for each setting that picks among options.
- **Settings change only before a run begins (Damian).** Once a run starts, its settings are fixed for the whole run.
- **One run with everything on,** where any part can be switched off for a later run, instead of staged runs.

## 1. Design calls

**C1. One registry of every setting.**
- *The table:* `core/src/keys.inc`, with one entry per setting:
  - its name;
  - its kind: switch, whole number, number, choice or text;
  - its default and range;
  - for a choice, its options;
  - its group and a short label;
  - a line of help;
  - whether its knob is linear or logarithmic (for rates spanning decades, like `disk.rot`);
  - the switch it depends on, if any.
- *Families:* families such as `res.<kind>.spread` are written once as templates and expanded for each kind.
- *One source of truth:* the engine reads every setting through its registry entry. So the default and range live in one place, and the page can never fall behind the engine.
  - A key read without an entry fails the tests.
  - Defaults that depend on other settings (for example, `disk.rot` follows the instruction table) are marked as such. The page shows the value the run will actually use (C4).
- *Seeding it:*
  - A one-off pass records every key that each preset and test config reads, with its type, default and range, and writes the first draft of the table.
  - Groups, labels and help are then written by hand.
  - Test-only keys (the `demo.*` worlds) are marked hidden.
- *Recommendation:* as above, with the getters changed to read by entry, which touches every call site (about 300).

**C2. The page.**
- *Where:* in `nova view`, which then sets up, starts and watches a run in one window.
  - `nova view` with no config opens the page on the default preset.
  - `nova view --setup <config>` opens it on that file.
- *Layout:*
  - The groups run down the left. The selected group's settings are on the right, one row each: label, control, default, a mark if changed from the loaded file, and a reset button.
  - Hovering shows the key's name and its help.
  - A search box finds any setting by name, label or help, and a "changed only" filter lists what differs from the loaded file.
- *Controls:*
  - a checkbox for each switch;
  - a knob (a slider with a number field beside it) for each number, held to its range, logarithmic where the registry says so;
  - a drop-down for each choice (`world.graph`, `isa.table`, `cost.table`, `bridge.phase` and the like);
  - a text field for paths and names.
- *Feature switches:* each feature's switch heads its group. With it off, the rest of the group is greyed out but keeps its values.
- *Groups (a draft):*
  - Run and output
  - World and land
  - Paths and bridges
  - Resources
  - Substrates
  - Sparks
  - CPU and cores
  - Memory
  - Disk
  - GPU
  - NPU
  - Heat
  - Hazards
  - Costs
  - Contact and eating
  - Signals
  - Life and death
  - Dormancy
  - Disasters
  - Detection and lineages
  - Budgets
- *Recommendation:* as above.

**C3. Before a run only (Damian).**
- *Before a run:* the page edits a configuration.
  - **Start** writes the full effective configuration (every setting, defaults included) to the run's folder as `run.cfg`, so the run can always be reproduced, and starts the run.
  - It can start in the window, watched, or headless as `nova run` in a console of its own with a stop file, for long runs like `long1`.
- *During a run:* the page is read-only and shows what the run was started with.
- *A finished run* (`nova view --run`): the page shows its settings from its replay log, read-only, with **Copy to a new run** to start from them.
- *A snapshot:* continuing one keeps the snapshot's settings, shown read-only.
- *The toolbar's live knobs (open, for Damian):* today 7 knobs can change during a run: spark rate, grant and lengths, CPU wear, ambient heat and transit corruption. They're logged and replayed, so determinism holds.
  - *Recommendation:* lock them, so every run's settings are fixed from its start, as Damian wants for the page. The 7 settings move to the page like every other.
  - Keeping them is the alternative, if live nudging is still wanted for experiments.

**C4. Checking before Start.**
- *The check:* the page reads the configuration with the engine's own code, the same parameter builders a run uses, without building the world.
- *What it shows:*
  - every range or cross-check problem, next to the setting (for example, `spark.min_len` above `spark.max_len`, or E cores without a timed cost table);
  - every value the run will actually use, including defaults computed from other settings;
  - the memory and disk the world will need, from the existing budget checks, which refuse worlds too big for the machine;
  - what the landscape will be, from `nova land`'s report: landmasses, paths, hermits and the number of distinct substrates.
- *Start* stays unavailable while any problem is open.
- *Recommendation:* as above.

**C5. Files.**
- *Load and Save:*
  - **Load** opens any `.cfg`, with the presets in `configs\` listed for one click.
  - **Save as** writes the settings that differ from the defaults, grouped and commented, so the file reads like today's presets.
- *`nova keys`:* prints the registry (name, kind, default, range, group, help), and writes `docs/keys.md` as a reference to read outside the app.
- *Unchanged:* `nova run <config>` and `--set key=value` work as today.
- *Recommendation:* as above.

**C6. Determinism.**
- The page only produces a configuration file. The world is built from that file exactly as `nova run` builds it.
- So a run started from the page gives the same hash as `nova run` on its `run.cfg`, and replays the same.

## 2. Steps
1. **The registry** (C1): the seeding pass, the table, the getters reading by entry, and `nova keys`. This is built early (world plan step 2), so every new key from the two plans goes straight in.
2. **The check** (C4): reading a configuration without building the world, the effective values, and the problems by setting.
3. **The page** (C2): groups, rows, the controls, search and the changed-only filter.
4. **Start, Load and Save** (C3, C5), with the run's folder and `run.cfg`, and read-only views of running and finished runs.
5. **Tests,** the README's start section, and `docs/keys.md`.

## 3. Tests
- *The registry:*
  - every key the engine reads (every preset and test config) has an entry with the same kind, default and range;
  - no entry is unreachable;
  - every suite keeps its hashes, since the defaults are unchanged.
- *The page, driven by made-up input:*
  - a checkbox toggles;
  - a knob drags and clamps to its range;
  - a drop-down picks;
  - a typed value applies;
  - search and the changed-only filter find the right rows;
  - a feature switch greys its group.
- *Save and Load:* Save then Load gives the same settings back.
- *Start:*
  - the run's `run.cfg` holds every effective setting;
  - a run started from the page hashes exactly as `nova run` on that file;
  - `nova run` on a saved file runs.
- *The check:* each cross-check problem shows next to its setting and blocks Start.

## 4. Risks
- **Changing about 300 reads is wide.** The change is mechanical, though, and every suite's hashes guard it: a changed default would break a hash.
- **About 300 settings is a lot of page.** Groups, search, the changed-only filter and greyed-out features keep it navigable.
- **New widgets:** the viewer is hand-rolled Win32. It has buttons, pop-up lists and text fields today. Checkboxes, sliders, drop-downs and a scrolling settings list are new, built the same way with no new headers or libraries.
- **Help text is hand-written for about 300 settings.** It's mostly drawn from `docs/design.md`, which already explains each one.

## Result (2026-11-07)

Implemented the registry, generated key reference, configuration page, check and memory estimate, Load/Save, and settings fixed from a run's start. Old replay knob records retain their behavior. See design section 6.18.
