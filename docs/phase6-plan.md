# Phase 6 plan: the analysis UI

> **Revision 1, APPROVED by Damian as recommended (2026-11-06), parked, then BUILT (2026-11-06).** Written as Phase 5. Damian then chose real GPU and NPU integration first ("I want the resource landscape to use everything I have got"), through Windows' own Direct3D 12, DXCore and DirectML rather than CUDA, so that became Phase 5 (`docs/phase5-plan.md`) and this plan became Phase 6. Damian unparked it on 2026-11-06 ("go ahead with 6 + 7"). What was built is in design.md 6.12. Details settled while building:
> - `--no-output` turns every output off at once; `--color NAME` and `--sample N` set the map's colouring and the chart sampling; `--window K` picks a finished run's window for a render.
> - `run.ticks`, `run.seconds` and `run.after_replicator` pause a watched run instead of ending it; Stop finishes the outputs (`stop = view_stop`), and so does closing the window (`view_close`).
> - `edge.corruption` is live only in a dispersal world (elsewhere nothing carries cargo, and the key was never read).
> - The map scales colours to the 95th percentile, so one crowded node doesn't flatten the rest.
> - The genome browser filters by a length, a range `a-b` or a lineage `n.k`, and the inspector lists a node's neighbours.
> - At `detect.genebank_threshold = 2` a 100-node world saves about 10 genotypes a tick, so the viewer keeps the newest 200,000 saves and events, and the run hands its saves over rather than keeping them (design.md 6.12, finding 2).
> - The stats row builder is shared by `stats.csv` and the charts, so they show the same numbers.
>
> - **Source of truth:** `docs/design.md`. Phase 5's scope is in 9.1; the UI's direction is 7.1 K and Section 8.
> - **Long runs:** the 12-hour run waits for Damian's go-ahead.

## 0. Scope (design.md 9.1)
Native Win32 + GDI: a biome map, population graphs, a genome browser, a phylogeny, and knobs whose changes are logged as replayable events.

**Not here** (Section 8 extras, later if wanted): the "who eats whose waste" view, species and partnership detectors, comparing several runs side by side, and attaching to a harness trial running in another process. Phase 7 (1,000+ nodes, parallel worlds; `docs/phase7-plan.md`) comes after.

## 1. What the design already fixes
- **Tech (7.1 K):** a native Win32 window drawn with GDI. No STL, no third-party libraries (7.1 B); `user32` and `gdi32` are platform libraries, linked by default.
- **Knobs (7.1 K, Claude's suggestion, accepted):** a knob changed mid-run is a timed event in the run record, so a replay reapplies it at the same tick. This already exists: `Run::schedule_knob` applies a change before a tick, logs it as a replay `knob` record and updates the config a snapshot carries.
- **Determinism (7.1 D):** watching must never change the world. Nodes touch only their own state during a tick; the UI may read only between ticks.
- **The map (4.1):** nodes sit at hidden coordinates, used for generation and visualisation only; organisms never see them.
- **Views (Section 8):** a biome map with population, migration, territories and collapse/creation overlays; population graphs; a genome browser; a phylogeny.

## 2. Design calls

**C1. One window, `nova view`.** A new command in the same executable: `nova view <config | snapshot> [--threads N] [--set key=value]...`. It runs the world live and shows it.
- One main window, split into panels: the map (largest), charts, an inspector (node, body, genome), a phylogeny, and a strip of run controls and knobs. Panels can be resized by dragging their borders.
- *Widgets are hand-rolled* (an immediate-mode style: each frame draws everything and handles the mouse in one pass), drawn with GDI into an off-screen bitmap and copied to the window, so nothing flickers. Native controls (and `comctl32`'s trackbars and list views) are avoided: one drawing path, no focus or flicker quirks, nothing to theme.
- Per-monitor DPI aware; Segoe UI for labels, Consolas for genomes.
- *Recommendation:* as above.

**C2. Threads: the world runs on its own thread; the UI reads at the barrier.**
- A sim thread owns the `Run` and steps it (its thread pool does the nodes, as now). The UI thread owns the window.
- About 30 times a second the UI asks for a frame. After its next step the sim thread copies a *view frame* (C10): a compact summary of what the panels show, plus the detail for whatever is selected. Then it carries on. The UI draws only from the latest frame.
- Knob changes and run controls go from the UI into a queue; the sim thread applies them between ticks (`schedule_knob` at the current tick), so they are logged and replay exactly.
- So a run watched in `nova view` hashes the same as the same run under `nova run`, knob changes included (a test checks this).
- *Recommendation:* as above.

**C3. Run controls.** Pause, resume, single step, a speed limit (ticks per second, or flat out), "snapshot now" (writes a numbered snapshot), and stop.
- `nova view` writes the outputs the config names (`output.*`), the same as `nova run`, so a watched run is a real run with a replay log. `--set output.replay=` turns any of them off.
- The title bar shows tick, ticks per second, instructions per second and the world hash at the last pause.
- *Recommendation:* as above.

**C4. The map.** Nodes at their hidden coordinates, edges between them (a ring world gets a circle layout).
- *Colour by* one of: bodies, births, colonists, live/fallow and health, the largest lineage (a stable colour per lineage id: territories), free CPU, waste, any node property (memory, millicores, climate, spark rate, radiation, disk, volatility), or signal activity.
- *Size by* bodies.
- *Overlays:* cargo in flight as dots moving along their edges (interpolated by send and due tick), links that are down dashed, fallow sites hollow, and a fading ring where a node just collapsed or came back.
- Hover shows a node's numbers; a click selects it for the inspector; zoom and pan with the wheel and drag.
- *Recommendation:* as above.

**C5. Charts.** World time series sampled every N ticks into ring buffers held by the UI: bodies, births, deaths, lineages, colonised, live nodes, in flight, compressibility, instructions per tick, and any CSV column on request.
- Up to four stacked charts, each with selectable series, auto-scaled, hover read-out, and the event ticks (replicator, colonist, collapse...) as markers on the time axis.
- Opening a finished run's `stats.csv` (C9) fills the charts with its whole history.
- *Recommendation:* as above.

**C6. The node inspector.** The selected node's properties, health, degree and neighbours, and its lineages (largest first).
- A *memory strip*: the soup as a block of pixels, one per codon, coloured by owner body, by lineage, by page heat, by salvage stamp, or by codon. A click on a body selects it.
- The *body inspector*: its genome (first block) as op names, its CPUs (registers, heads, errors), holdings (millicores, GPU, NPU, disk, memory) and lineage tag.
- *Recommendation:* as above.

**C7. The genome browser.** Every genotype the genebank has saved, with name, length, lineage, first tick and parent, sortable and filterable by lineage or length. Selecting one shows its genome, and highlights where living copies sit on the map (the nodes holding it).
- Needs one engine change: the run keeps its genebank saves in memory (name, lineage, parent, genome) when a viewer asks for them, besides writing `genebank.txt`.
- *Recommendation:* as above.

**C8. The phylogeny.** For the selected lineage: its saved genotypes as a tree by parent link, time across and generations down, each labelled with its name and length; a click selects that genotype in the browser.
- Built from the genebank's saves, so it shows the genotypes that reached the threshold, not every individual.
- *Recommendation:* as above.

**C9. Finished runs (the 12-hour runs).** Watching only a live run would leave the long runs to Excel. Two small additions make them browsable:
- `output.snapshot_every` (new; 0 = off): a long run also writes numbered snapshots every N ticks (`<snapshot>.<tick>`). A busy 100-node world is likely tens of megabytes a snapshot (to be measured), so the interval should be generous: every 100,000 ticks is a few dozen files in 12 hours.
- `nova view` opens a snapshot as well as a config, continuing live from that moment with its outputs off, so any point in a long run can be inspected in full and poked with knobs without touching the original run's files.
- `nova view --run <dir>` loads a run directory's `stats.csv`, `nodes.csv`, replay events and `genebank.txt` into the charts, the map (a time slider over the per-node windows) and the browser, without simulating.
- *Recommendation:* as above.

**C10. The view frame (the testable core).** Everything the panels draw comes from a frame built by plain functions with no Win32 in them: node summaries, edges, cargo positions, the selected node's memory strip and bodies, chart samples, genebank entries and the phylogeny layout. The Win32 shell only draws frames and forwards input.
- *Recommendation:* as above.

**C11. Headless rendering.** `nova view <config> --render out.png --ticks N [--size WxH]` runs N ticks without opening a window, draws one frame into the off-screen bitmap and writes a PNG.
- A hand-rolled PNG writer (stored, uncompressed deflate blocks with CRC-32 and Adler-32: a few dozen lines, no zlib).
- It verifies the drawing in tests and makes screenshots easy to share.
- *Recommendation:* as above.

**C12. More live knobs.** Only `spark.rate`, `spark.grant`, `spark.min_len` and `spark.max_len` are live today. The knobs panel shows them, plus three that apply as one number to every node and have no on/off state to manage: `cpu.wear`, `heat.ambient` and `edge.corruption`.
- Everything else stays structural: per-node varied settings (hazards, volatility) would need per-node factors, and switches like `salvage.ratio` or `edge.failure` allocate or drop state when they turn on or off.
- *Recommendation:* as above; more can follow one by one.

## 3. What gets built
- **App:** `nova view`: the Win32 shell, the sim thread and queue, the panels (map, charts, inspector, genome browser, phylogeny, controls and knobs), PNG output, the `--run` loader.
- **Core:** the view-frame builders; the in-memory genebank log; `output.snapshot_every`; opening a snapshot with outputs off; the three new live knobs; an accessor for cargo in flight.

## 4. Evidence
- `verify.ps1` green, covering:
  - the view frame (node summaries, cargo positions, the memory strip, chart sampling, the phylogeny layout, lineage colours stable across frames);
  - watching changes nothing: a run stepped with a frame built every tick, and with knobs queued from a pretend UI, hashes the same as plain `nova run` with the same knobs, and its replay log replays;
  - the new live knobs (applied, logged, replayed, carried in snapshots);
  - `output.snapshot_every` and opening a snapshot;
  - the PNG writer (a known image, checked byte for byte) and a headless render of a world.
- Manual: headless renders of a 100-node world at a few points (shared as PNGs), and Damian trying the live window.

## 5. Docs
- design.md: a new 6.12 (the UI; 6.11 is Phase 5), 7.1 K and 9.1 (built), the status line.
- README: how to open the viewer.
