# Nova

A Tierra-inspired digital ecosystem: programs are virtual code living in
per-node soups on a graph of nodes, owning resources governed by heat, and
(the main mode) arising from a random soup with no replicator seeded. The full
design is in [`docs/design.md`](docs/design.md), which is the source of truth.

Like ScratchML and itracker, Nova is written **without the C++ standard
library (STL) and without third-party libraries** -- we own the whole stack.
Only thin C-runtime headers (`cstdint`, `cstddef`, `cstdio`, `cstdlib`,
`cstring`, `cmath`) and platform headers (`windows.h`, `intrin.h`, and since
Phase 5 Windows' own Direct3D 12, DXCore and DirectML headers, used only by
`core/src/accel_dx.cpp`) are used. A CTest entry (`includes`) enforces this;
adding a header to its allowlist is a dependency decision to discuss first.
The build also runs the Windows SDK's own `dxc.exe`, when it's there, to
compile the GPU warp kernel; only its output is embedded, and nothing of DXC is
included, linked or shipped (`docs/dxc-plan.md`).

## Start

Build with `powershell -ExecutionPolicy Bypass -File tools\build.ps1`, then
open `build-release\bin\nova.exe view` for the configuration page. Every
setting can be checked before Start; settings stay fixed throughout a run.
`nova view --setup configs\landscape.cfg` opens the larger 1,000-node preset
with substrates, memory and disk tiers, disasters and observers enabled.

`nova keys` lists settings; `nova keys --md docs/keys.md` regenerates the
reference. `nova analyse novelty|roles|groups|tree|scores <run folder>`
produces offline reports (`nova analyse` lists options). `nova search
<search file> --dry-run` checks a tuning search before running its trials.
`nova view --run <folder> --show life --render life.png` renders the map of
life (`--life-colour role` colours it by behaviour-profile role);
`--map-of-life frames/life --frames 24` writes animation frames.
The viewer also offers new kinds of life, substrate maps and disaster marks.

## Status: varied world, disasters and analysis built (design.md 6.18–6.23)

**INJECT moves the program; swallowed prey are digested** (design.md 6.27,
`docs/inject-plan.md`, 2026-11-09):
- A landed INJECT moves the whole program into its host through a wound,
  with its threads, cores and devices. The memory it leaves is cleared to
  zeros. It lives on as a parasite inside the host: it can read, write and
  run the host's code, and the host can overwrite it.
- It multiplies by carving copies out of its host's code; its daughters take
  the host's cores. When the host dies the parasite gets what is left of its
  own code back as a free body.
- INJECT needs a host that owns more memory than the injector
  (`contact.inject_ratio`, 1) and is priced like a bite, not like COPY. A
  host only a little bigger is all but overwritten.
- A bite that would swallow its prey whole now digests it, `engulf.digest`
  (2) codons a tick. The prey keeps running and can INJECT itself into its
  eater to escape. Its cores go to its eater with its codons, the same share
  each tick (design.md 6.29), so each runs on what it holds.

**Warp widths by node** (design.md 6.28): each node's GPU has its own warp
width, a power of two drawn from the world seed between `warp.lanes_min`
(16) and `warp.lanes_max` (1,024, the GPU's ceiling). The GPU runs every
width on its own kernel, compiled at build time, and gives the CPU
reference's results word for word.

**Each warp its own lanes** (design.md 6.29, `docs/lanes-plan.md`): the
node's width is the most a warp there may have. A launch asks for CX lanes,
a warp's daughter gets its lanes, and any other body born on the GPU the
fewest that cover its code. A step costs the warp's own lanes, so a small
program no longer pays for a wide node's lanes. `stats.csv` gains `lanes`.

**Dormancy and drift** (design.md 6.17, `docs/dormancy-plan.md`): a program
the watchdog would kill as frozen (200 ticks without running, typically buried
in upkeep debt) now goes dormant, like a bacterial spore, if it has reserves.
- It keeps its code, leaves its cores, extra memory and disk to its node, and
  drifts at random from node to node until it finds a free core of its kind,
  where it wakes.
- Its reserves (the memory beyond its code, then its disk) run down as it
  drifts. Spent out, it dies.
- Resting on a node it's easy food: no shield, no response.
- Node collapse now comes only from wear; the starvation rule, which collapsed
  full nodes, is gone.
- In the late landscape over 5,000 ticks: 20% more births, 11% fewer deaths,
  and 94% of dormant programs woke somewhere, most within a hop or two, at no
  cost in speed (`life.dormancy`, on by default).

**Phase 10** changes where life's material comes from and how things end,
and turns every feature of the world on by default (set a key to change it):

- **Vents:** points in each node's memory spray random codons into nearby
  waste, fast and continuously (`vent.spacing`, `vent.rate`, `vent.radius`).
  Sparks still only animate a run of waste with a CPU and millicores, now a
  quarter as often in the presets.
- **Decay:** dead matter loses bits until it reaches INERT, codon 0 of the new
  default table `nova_v2` (it ends templates and fails when run), so animated
  dead matter halts at once. Living code doesn't decay; random flips (cosmic
  rays) are a hundredth of the old rot.
- **ENGULF bites:** up to CX codons, never more than the eater's own size, of
  the nearest material: raw waste (foraging) or a body. Smaller bodies are
  swallowed whole; bigger ones lose a piece at the point of contact and the
  same share of their millicores, and limp on if they can. A surface still
  shields a body from blind bites.
- **Death:** a halted thread leaves its body, and a body that runs nothing
  for 200 ticks (other than waiting on the disk or a device) dies.
- **On by default:** `home_v1`, threads and E cores, the GPU and NPU with
  async real devices, warps, the Phase 4 features and all of the above. The
  test suites keep the old defaults (`Config::set_fallback`) so they still
  test one feature at a time.

**Profiling after the hour-long test** (design.md 6.16) found GPU life (about
42,000 warps) to be the cost (28–29 ms a tick with the GPU), and fixed three
bugs: `warp.device = auto` getting stuck without the GPU for 15,000 ticks at
four times the cost; the RTX 5080's integer division being wrong for some
operands, which made runs differ (warp code now divides exactly, so the GPU
and the reference agree again); and snapshots losing a tick's pending page
heat (snapshot format v2). `nova resume <snapshot>` continues a snapshot
headlessly for timing and bisecting. `warp.device = auto` now shares each
round per body (long bodies to the CPU threads, the rest to the GPU) with
the split set by a measured model of both sides.

The warp kernel is now compiled at build time by the Windows SDK's DXC, a
build tool only, with no new header or DLL at run time. It keeps
`core/src/warp_kernel.hlsl` as its one source and gives records groupshared
tiers sized to them, the long bodies first. The late world's GPU round takes
16.1 ms instead of 29.6, word for word the same, and a late tick takes
25.7 ms instead of 28.3 in `auto` (31.9 instead of 38.7 with every body on the
GPU). A build without `dxc.exe`, another lane count, or `warp.kernel = fxc`
runs the old FXC kernel, compiled at run time. `nova resume --dump-round`
and `nova bench warp` replay one GPU round to compare kernels.

**Phase 9** charges every instruction in world time measured on the real
machine (`cost.table = home_v1`, the default since Phase 10; `classic` is
every instruction costing 1), and (9c) gives bodies threads and two kinds of
core:

- **One fixed table, measured once:** `nova bench costs` times this
  machine's own operations outside the simulation (an add, a divide, a load,
  a mispredicted jump, a kernel call, a cache line between cores, the disk),
  and their numbers live in the source as `home_v1`. Runs never measure, so
  they replay exactly. `nova costs` prints a table.
- **Instructions as machine code:** each costs what it would be: a divide
  14 adds, a jump the hardware can't predict 24, a template search a scan
  per place it looks, and every world change (ALLOC, DIVIDE, ENGULF ...) a
  kernel call of about 2,000 adds. A tick is `cpu.instr_per_core` adds.
- **Actions that take time:** ENGULF and INJECT, and anything longer than a
  tick (a DISK read), start, wait and end. ENGULF takes 16 of the prey's
  codons a tick (`engulf.codons_per_tick`), so bigger prey take longer. The
  target can sense it (SENSE 22, 23) and respond; at the end the attack lands
  only against the target as it is then, so rewriting the aimed-at surface,
  leaving or eating the attacker first defends. Milestone `defended`.
- **The GPU and NPU, measured:** a GPU lane is about 7 times slower than a
  P-core at serial work and far from its memory, but the GPU runs 10,752 of
  them. Each warp has its own clock (a step takes its instruction's time on
  a lane) and its body's GPU units bound its lanes' throughput; a warp's
  host instruction waits a GPU round trip (67 µs, about 15 ticks). A body's
  device ops in a tick are one batch: the first pays the start-up, each
  further op only its share. With warps doing that much more, the real GPU
  now runs `configs/gpu_world.cfg` twice as fast as the CPU (60 against 124
  ms a tick, the same world).
- **Threads (`cpu.threads = true`, 9c):** a body's CPUs are threads, each on
  one core (1,000 millicores) at a time, so a thread gets at most a core's
  tick. Programs start threads with the GPU verb's BX 21 (a fork at a
  template) and choose P or E cores with BX 22. Threads take turns in
  `cpu.slices` (8) a tick, so they can hand data to one another within it.
- **P and E cores (`cpu.e_share`, 9c):** that share of each node's millicores
  are E: an instruction on one costs `home_v1`'s E-core column (about 15%
  slower for adds, 60–70% for divides, searches and kernel calls) and makes
  0.4 of the heat. Each thread gets a core of its preferred kind first, then
  whatever is left of either kind, with no penalty for the wrong one. ALLOC
  and RELEASE BX 5 take E millicores; DIVIDE, ENGULF and dispersal carry both
  kinds; in a landscape the E share varies by site (`res.ecpu.*`).

**Phase 8** runs programs on the GPU (`gpu.warps = true`; off, everything is
as before):

- **Warps:** a body's code in lockstep across `warp.lanes` lanes (32), each
  lane a full CPU state. Lanes that agree run together cheaply; lanes that
  branch apart take turns. Heads are offset by lane, so one COPY on 32 lanes
  copies 32 codons. A warp works only on its own body's memory, and stops in
  front of world-changing instructions (ALLOC, DIVIDE ...), which the host does
  after the round.
- **Two forms of life:** a body is born on a CPU or on a GPU (a warp). A
  daughter is born on what divided her; sparks draw GPU units too
  (`spark.gpu_grant`), so CPU-barren, GPU-rich nodes get life. The GPU verb's
  BX 20 launches a warp from either form.
- **The real GPU:** the warp interpreter is one source, `core/src/warp_isa.inc`,
  compiled as C++ for the CPU reference and as HLSL for a Direct3D 12 kernel,
  so both give the same world bit for bit. Lanes stay on the GPU between
  rounds. `warp.device = auto` (the default) shares each round between the
  GPU and the CPU at once, and runs whole ticks without the GPU when that is
  quicker; `gpu` or `cpu` forces one; `warp.verify = N` checks every Nth GPU
  round against the reference. In today's worlds the CPU is quicker
  (design.md 6.14, findings).
- **Milestone `gpu_native`:** the first replication by a body born on a GPU.
- **Seeing it:** the inspector shows a body's form, warps and lanes; the map
  colours by GPU lanes running; `stats.csv` gains warp columns; `--profile`
  shows the GPU round's time and bytes.
- **Preset** `configs/gpu_world.cfg`: the landscape with GPU units placed
  against CPU (`res.gpu.against = cpu`) and a third of its nodes CPU-barren.
  Harness arms `warps` and `gpu_world`. `budget.gpu` also sizes `warp.capacity`.

**Phase 7b** maps budgets to the machine. Each budget is turned into an
ordinary setting at start-up and recorded, so replays rebuild the same world:

- `budget.memory = 50%` (or `24GB`) sizes the soups from the RAM available.
- `budget.disk = 20GB` together with `disk.path = E:\somewhere` puts the
  in-world disk tier in memory-mapped files there (deleted when the run ends).
  The results are the same as with the disk in RAM.
- `budget.gpu = 50%` and `budget.npu = 25%` (async mode) size the
  accelerators from a short calibration.

A world too big for the RAM or the disk is refused with what it would need.
Runs leave one logical processor free (`budget.threads`) and run at
below-normal priority (`budget.priority`).

**Phase 7a** adds one big world generated like a game level,
`world.graph = landscape` (the old `ring` and `procedural` modes are
unchanged):

- **Land:** noise land and water on a raster, separate landmasses, and
  `world.nodes` nodes scattered over the land.
- **Paths, generated once:** a triangulation gives each node its natural
  neighbours, then paths over water are dropped and each node keeps as many as
  it wants. Most have a few, hubs up to 12, hermits none.
- **Bridges between landmasses** open for `bridge.open_ticks` of every
  `bridge.open_every` ticks.
- **Distributions:** sparks and every resource are spread unevenly, so nodes
  are rich, poor or barren (`spark.*`, `res.memory.*`, `res.cpu.*`, ...).
- **The map** shows the land, bridges open or shut and landmass labels, with
  an overview map; double-clicking fits a landmass.
- **Map seeds:** `world.seed` (a number or any text) picks the map apart from
  the run's `seed`: places, paths, bridges, land, substrates and the resource
  and spark spreads (`--set world.seed=atlantis`). Empty, the map is `seed`'s.
- **Tools:** `nova land <config>` prints what settings generate;
  `nova run --profile` shows where each tick goes; `stats.nodes_every` thins
  the per-node CSV.
- **Presets** (1,000 nodes each): `configs/landscape.cfg` (continents),
  `archipelago.cfg` and `maze.cfg`.

### Phase 6 (design.md 6.12)

**Phase 6** is the analysis UI, `nova view`: a native Win32 window drawn
with GDI (Windows' own `user32` and `gdi32`, nothing else).

- **Live:** `nova view <config>` runs the world and shows it, writing the
  outputs the config names (`--no-output` turns them off). A sim thread steps
  the run; the window reads view frames between ticks, built on the run's
  idle workers and kept to about 5% of the sim's time, so watching a big
  world costs about 2% of its speed. Pause, step, a speed
  limit, snapshot now and stop: watching changes nothing. (Phase 6's live
  knobs in the toolbar are gone: since the setup page, a run's settings are
  fixed from its start. Replay logs with knob records still replay.)
- **Panels:** a map (colour by bodies, lineage territories, health, any node
  property...; cargo in flight, failed links, fallow sites, collapse and
  recovery rings), four charts of any `stats.csv` column, an inspector (a
  node's memory one pixel per codon, its bodies, a body's genome and CPUs), a
  genome browser and a phylogeny.
- **Scrolling and zoom:** every list and panel the wheel scrolls has a
  scrollbar too (drag the thumb, or click the track to page). The wheel over
  the charts zooms their shared timeline about the tick under the mouse; drag
  to pan, and double-click, Home or **whole run** to see the whole run again.
- **Snapshots and finished runs:** `nova view <snapshot>` continues one live;
  `output.snapshot_every = N` writes numbered snapshots during a run;
  `nova view --run <dir>` browses a finished run's files with a slider over
  time.
- **Headless:** `--render out.png --ticks N` draws one frame into a PNG (a
  hand-rolled writer, no zlib).

### Phase 5 (design.md 6.11)

**Phase 5** runs the in-world GPU and NPU on the machine's real devices,
through Windows' own Direct3D 12 and DirectML (no CUDA). With
`accel.mode = async`, accelerator ops are issued during a tick, gathered for
every node at the barrier, sent to the devices as one batch, and written back
`accel.latency` ticks later (default 2), with results in a per-CPU slot
(SENSE 18) and a pending count (SENSE 19). Async mode defaults to a
high-volume cost profile: expensive per op, nearly free per element.

- **The GPU** (`accel.gpu = auto | cpu | <name>`): one compute shader does
  all 20 op kinds exactly, with integer atomics for the reductions.
- **The NPU** (`accel.npu = auto | cpu | gpu`): dot, matvec and matmul as
  float16 matrix multiplies of base-8 digits (the only exact form the Intel
  NPU takes), put back together exactly on the CPU. A self-test at start-up
  decides whether it is used; otherwise NPU ops go to the GPU.
- **Determinism:** every device result equals the CPU reference bit for bit,
  so the world hash doesn't depend on where the ops ran.
  `accel.verify = N` checks every Nth batch against the reference and stops
  the run on any difference.
- `nova devices` reports what each adapter accepts; `nova bench devices`
  times the devices against the CPU reference. Harness arms: `gpu_hw`,
  `npu_hw`, `accel_hw`.

In the first checks the devices are correct and busy (1.6 million device ops
in 3,000 ticks of random soup, all verified) but not yet faster than the CPU
reference: random code's ops are small, and the NPU's round trip is longer
than a tick. Instant mode (the default) is exactly Phase 4.

### Phase 4 (design.md 6.10)

**Phase 4** adds the individual features, each its own switch (off by
default, so earlier worlds are unchanged) and its own harness arm:

- **the ambient heat leak** (`heat.ambient`): a busy body warms everyone on
  its node a little;
- **edge corruption and link failures** (`edge.corruption`, `edge.failure`):
  bit flips in transit, and links that go down for a while, losing what they
  carry;
- **the disk and volatility** (`world.disk`, `mem.volatility`): a second,
  slowly rotting store with DISK compare/store/load, and memory wipes that a
  program can repair from its own disk copy;
- **remnants and salvage** (`salvage.ratio`, added by Damian): dead code
  remembers whose it was, and a much smaller program taking it (or an engulfed
  big prey's code) gets only pieces;
- **signals** (`signal.channels`): EMIT and LISTEN on shared channels,
  attenuated per hop, delayed by edge latency, under a noise floor;
- **the node lifecycle** (`node.endurance`, `world.land`): nodes wear out
  (starving too, until 2026-11-07) and collapse, leaving ruins, and sites come back as
  nodes where a drifting land field allows, with the graph rebuilt.

New milestones `disk_user` and `signaller`; world events `collapse` and
`recovered`; output `output.signals` (the researcher's signal tap).

### Phase 3 (design.md 6.9)

**Phase 3** builds the world. `world.graph = procedural` scatters nodes in a
hidden 2D space, links each to its nearest neighbours, cuts links where a
ruggedness noise field runs high and adds a few long links. Each node gets its
own properties from noise fields (`vary.*`: memory, millicores, GPU and NPU
units, climate, spark rate, radiation). DIVIDE with BX = neighbour k (nearest
first) sends the daughter away: her share goes back into the origin's pools,
she crosses the edge as cargo (bandwidth, latency, capacity), and on arrival
she takes waste and draws her share from the destination's pools, in a fixed
delivery order at the barrier. Lineages span nodes, and the first gated
arrival on a node the lineage wasn't founded on is a `colonist` event. Many
origins by default, or `world.origins = one`; `seed.node` seeds one founder.
New outputs: CSV columns `sent`, `arrived`, `lost`, `in_flight`, `colonised`,
and a per-node file (`output.nodes`). Ring worlds without dispersal are exactly
as before. In short runs of `configs/world.cfg` the first replicator from
random code was already a disperser, and colonists reached every connected
node within 750 ticks.

### Phase 2b (design.md 6.8)

**Phase 2b** widens the in-world GPU to 16 ops (reductions compare, count,
find and sum; copy, fill, not, reversed copy; add, sub, xor, and, or, min, max,
mul) and adds the in-world NPU (`world.npus`, default 0): dot product, matrix
times vector, matrix times matrix and argmax on signed fixed-point codons. Both
run through the one accelerator verb (BX 0-15 GPU, 16-19 NPU), are held and
heated like millicores, and are virtual: the machine's real NPU is not used.
The detector marks the first replicator lineage to use the NPU (`npu_user`);
the harness has `npu` and `accel` arms.

### Phase 2 (design.md 6.7)

**Phase 2** adds the in-world GPU: `world.gpus` units per node (default 0, which
leaves everything exactly as Phase 1b), held, heated, throttled, worn and
passed on like millicores, and the GPU instruction (compare, copy, add, xor
across CX codons from the read head to the write head, as far as the body's GPU
capacity allows each tick). The detector marks GPU-made copies and the first
gated lineage to copy with the GPU (`gpu_replicator`); the harness has a `gpu`
arm. In a short check the GPU sped up emergence about 4×, and a GPU-copying
lineage arose from random code within two minutes.

The 12-hour paired experiments wait until everything is built; each phase is
checked with runs of minutes (design.md 9.1).

### Phase 1b (design.md 6.6)

**Phase 1b** (the abiogenesis test) adds, per the approved plan
(`docs/phase1b-plan.md`, revision 3):

- **Rules:** square-cube memory upkeep (`mem.upkeep`, default 2, exponent 4/3),
  CPU wear (`cpu.wear`) and spark timing (`spark.timing = first|last`), live
  spark knobs (`spark.grant`, `spark.min_len`, `spark.max_len`), and INJECT's
  criteria (a key matching the host's surface, a host the payload fits in).
  `mem.upkeep = 0`, `contact.inject_key = false` and `contact.inject_room = 0`
  together were exactly Phase 1a, until the bites plan (2026-11-09) replaced the
  key with wounds (below) and Phase 1a could no longer be reproduced.
- **Controls:** the `nova_v1_plain` and `nova_v1_inv2` codon tables and the
  shuffled-selector numbering (`isa.selectors = shuffled`).
- **Detection** (a pure observer; the world hash is identical with it off):
  inheritance links at DIVIDE, lineages, the replicator gate, a Tierra-style
  genebank, a hand-rolled LZ77 compressibility estimate and the new CSV
  columns. Gate events are `event` records in replay log v2 and `nova replay`
  re-checks them.
- **Running:** `--set key=value`, `run.seconds`, Esc and `--stop-file` stops
  that keep replay exact, `run.after_replicator`, and `result.txt`;
  `tools\abio.ps1` runs paired arms x seeds as separate processes (`-Percent`,
  default 75% of the logical processors, or `-Parallel N`; `-Priority`,
  default below normal) and writes `summary.csv` with the detection rate per arm.

The first smoke runs found replicators within seconds that copied themselves
with INJECT. INJECT now needs a key matching the host's surface and a host the
payload fits in (`contact.inject_key`, `contact.inject_room`); since then
replication goes through COPY, and in two-minute checks the plain and shuffled
controls produced no replicators while baseline and inv2 did (design.md 6.6).
Detection counts DIVIDE copies only for codons the parent wrote, and INJECT
copies too. (Since 2026-11-09 INJECT lands only through a wound instead of a
key: design.md 6.26, `docs/bites-plan.md`.)

### Phase 1a (design.md 9.1)

**Phase 0** built the skeleton and the determinism guarantee: the same seed
gives an identical world-state hash at 1, 4 and 20 threads, proven by the
`gate` test, which also permutes the thread pool's dispatch order (reversed and
shuffled) so it shows order independence, not just lucky timing.

**Phase 1a** adds the interpreter on one node (`world.model = soup`; exact rules
in design.md 6.5): codon table v1 and Ray's instruction set 0, private code,
the millicore ledger and free pool, per-unit heat with throttle-then-release,
copy errors, execution flaws and bit rot, ALLOC/DIVIDE, ENGULF/INJECT with the
graded surface contest, the death sweep and sparks. Its gates:

1. Light Tierra mode: Ray's ancestor 0080aaa replicates in **827 / 809**
   instructions and breeds true (Ray's documented figures; why not 839/813 is
   in design.md 6.5) -- the `tierra` suite.
2. The 6.2 sketch replicator makes exact copies under Nova's rules -- `soup`.
3. Throughput: about 3.5e7 instructions/s on one core and 2e8 across the
   machine for a random soup (design.md 7.1 I) -- `tools\bench.ps1`.
4. The determinism gate holds with the interpreter, heat, hazards and contact
   all running -- `gate`.

Phase 0's trivial parcel model stays as `world.model = demo` (the default), so
its gate hash and replay logs are unchanged.

## Layout

| Path | Role |
|------|------|
| `core/` | `nova_core` static library, namespace `nova`. Containers (`Array`, `HashMap`, `Str`, `TextBuf`), counter-based RNG, hashing, `key = value` config, Win32 thread pool, world + tick loop, run driver, snapshots, CSV stats, replay log. Phase 1a: `isa` (codon tables), `soup` (a node's organisms, ledger, heat, hazards, sparks, death sweep), `interp.cpp` (the instructions). Phase 1b: `lineage` (the replicator detector), `genebank`, `compress`. Phase 2 and 2b: the GPU and NPU (`UnitLedger` in `soup`, the accelerator verb in `interp.cpp`). Phase 3: `graph` (procedural generation and noise), per-node parameters, transit and delivery in `world`. Phase 4: the disk (`DiskLedger`), volatility, stamps and salvage, wear and ruins in `soup`; corruption, link failures, signal propagation, collapse, recovery and graph rebuilds in `world`. Phase 5: `accelop.h` and `accel_ref.cpp` (async ops and the CPU reference), `accel.h` and `accel_dx.cpp` (device discovery, probes, the Direct3D 12 GPU backend and the DirectML NPU backend), the ops in flight and the devices in `world`. Phase 6: `view` (view frames, the live session, a finished run's files, the phylogeny layout), `png` (the PNG writer), the shared stats row in `stats`, the new live knobs in `world`, the genebank log and numbered snapshots in `run`. Phase 7: `landscape` (land, nodes, the triangulation, paths, bridges, distributions), the landscape mode, bridge schedules and fixed-path lifecycle in `world`, the step profile; `budget` (budgets, calibration, the fit checks) and `mapped` (memory-mapped files for the disk tier). Phase 8: `warp_isa.inc` (the warp interpreter, one source for C++ and HLSL) and `warp_layout.inc` (its image), `warp.cpp` (packing, unpacking, the warps' bookkeeping), host instructions in `interp.cpp`, `WarpGpu` in `accel_dx.cpp` (the kernel), the tick split and device choice in `world`. The settings registry: `keys` (`keys.h` explains adding and reading a setting; `keys.inc` is the table of every setting's kind, default, range, group and help). The setup page's check: `check` (a config read with the engine's own code, nothing built). |
| `app/` | The `nova` command line, including `nova bench`, `nova bench devices`, `nova devices`, `nova keys` (every setting; `--md` writes `docs/keys.md`), `nova resume` and `nova view` (`view_cmd.cpp`: the window, the sim thread, starting runs from the setup page). `canvas`, `viewer`, `setup` and `widgets` (the GDI canvas, the panels, the setup page, and the scrollbar and charts' zoom they share; `theme.h`, their colours) form the `nova_viewer` library, which the tests drive headless. |
| `tests/` | One `nova_tests` executable, one CTest entry per suite, hand-rolled `tests/check.h` harness. |
| `core/src/learn/`, `core/src/analysis/` | Hand-written learning algorithms; novelty, roles, groups, family trees, run scores and settings search. |
| `core/src/profile.cpp`, `core/src/lifemap.cpp` | Observe-only behavior sampling and the fixed-reference PCA map of life. |
| `docs/results/` | Feature findings, integration decisions and measured checks; `design.md` contains the consolidated behavior. |
| `tools/` | `check_includes.cmake`, the no-STL guard; `bench.ps1`, the Release benchmark; `abio.ps1`, the trial harness (single-node abiogenesis trials, or world trials with `-Threads`). |
| `configs/` | Example run configs; `abio.cfg` is the Phase 1b baseline trial, `world.cfg` the Phase 3 100-node world. |
| `pilots/phase1b/` | The throwaway pilot code behind the Phase 1b plan's findings (not built by CMake). |
| `.claude/` | `verify.ps1`, the fast build + test loop. |

## Build & test

Everything ships with Visual Studio's C++ toolchain (MSVC + CMake + Ninja) --
any recent edition, or the standalone Build Tools, with the *Desktop development
with C++* workload; nothing extra to install. Run the verify loop:

```powershell
.\.claude\verify.ps1
```

It sets up the MSVC environment, configures on the first run only, does an
incremental Ninja build, and runs the tests via CTest. It exits non-zero if the
build or any test fails.

## Running

`nova.exe` is a command-line program: run it from a terminal (PowerShell, or
the Code tab's terminal) with a command and a config, not by double-clicking
it. Started with nothing to do, it prints its usage and exits; a window opened
just for it (a double click) now waits for Enter so that can be read.

The quickest start, from the repository folder:

```powershell
.\tools\build.ps1                                        # build nova.exe (Release) into build-release\bin
.\build-release\bin\nova.exe view                        # the setup page: every setting (from configs\landscape.cfg), then Start
.\build-release\bin\nova.exe view --setup configs\world.cfg   # the setup page on another file
.\build-release\bin\nova.exe view configs\landscape.cfg  # watch the 1,000-node world live (Space pauses; close the window to stop)
.\build-release\bin\nova.exe run configs\landscape.cfg --ticks 2000 --profile   # headless: prints events, then the hash
.\build-release\bin\nova.exe                             # the usage: every command and option
```

**The setup page** (`nova view` with no config, or `--setup <config>`;
`docs/config-plan.md`) shows every setting before a run begins, from the
settings registry: the groups down the left, a row per setting with its
control (a checkbox, a slider and a number, a drop-down or a text field), its
default, a mark when it differs from the loaded file, and a reset. Hover a row
for the key's name and help; search names, labels and help; "changed only"
lists what you changed. Settings whose switch is off are greyed out but keep
their values. The right-hand panel checks the settings with the engine's own
code as you go: problems by setting (Start stays off until they are fixed),
the values the run will work out for itself, the memory and disk the world
needs, and a landscape's map (land, paths, bridges and nodes) with its
landmasses, paths and hermits. **Map seed** (`world.seed`) is the map's own
seed, like a game's world seed: a number or any text. **new map** (beside it,
and above the map) rolls a fresh one, so you can look through maps before
starting; the run's `seed` then varies what happens on that same map. Left
empty, the map follows `seed` as before. **Load** opens any
`.cfg` (the presets one click away); **Save as** writes what differs from the
defaults, grouped and commented. **Start** writes every setting to the run's
folder (`runs\<name>` unless you pick or type another) as `run.cfg` and starts
it in the window, watched, or headless (`nova run <folder>\run.cfg` in its own
console; Esc there, the page's Stop, or a file named `stop` in the folder ends
it). A run's settings are fixed from its start: the toolbar's Settings button
shows them read-only, as `nova view --run <folder>` does for a finished run
(with **Copy to a new run**).

A `run` with no tick or time limit (`run.ticks = 0`, `run.seconds = 0`, as in
`landscape.cfg`) goes on until Esc. Outputs go where the config's `output.*`
keys say (`runs\landscape\manual\` for `landscape.cfg`).

More:

```powershell
.\build\bin\nova.exe run configs\demo.cfg --threads 20
.\build\bin\nova.exe run configs\demo.cfg --threads 1      # same hash
.\build\bin\nova.exe replay runs\demo.replay --threads 4    # re-run and check every logged hash
.\build\bin\nova.exe run configs\soup.cfg --threads 20      # the Phase 1a soup world
.\tools\bench.ps1                                          # Release build + interpreter benchmark

# Phase 1b: one abiogenesis trial by hand (Esc stops it; outputs in runs\abio\manual)
.\build-release\bin\nova.exe run configs\abio.cfg --set seed=3 --set run.seconds=600
# the paired experiment: 4 arms x seeds, 12 h each (Esc stops, resumable);
# 75% of the machine by default, all of it overnight with -Percent 100 -Priority Normal
.\tools\abio.ps1 -Name exp1 -Hours 12 -Seeds 1,2,3 -ArmSeeds @{ baseline = 1..6 }

# Phase 3: the 100-node world on all threads (outputs in runs\world\manual)
.\build-release\bin\nova.exe run configs\world.cfg --set seed=3 --set run.seconds=300
# world trials through the harness: 8 threads each, so two at once at 75% of 24
.\tools\abio.ps1 -Name w1 -Config configs\world.cfg -Arms baseline -Seeds 1,2 -Threads 8 -AfterReplicator -1 -Hours 1
# Phase 4: each feature against the baseline (arms leak, transit, disk, salvage, signals, lifecycle, phase4)
.\tools\abio.ps1 -Name p4 -Config configs\world.cfg -Arms baseline,disk,lifecycle -Seeds 1,2 -Threads 8 -AfterReplicator -1 -Hours 1

# Phase 5: the machine's accelerators, their speed, and a world using them
.\build-release\bin\nova.exe devices
.\build-release\bin\nova.exe bench devices
.\tools\abio.ps1 -Name p5 -Config configs\world.cfg -Arms accel,accel_hw -Seeds 1,2 -Threads 8 -AfterReplicator -1 -Hours 1

# Phase 6: watch a world live (outputs in runs\world\manual; Space pauses)
.\build-release\bin\nova.exe view configs\world.cfg
# a long run with numbered snapshots, then continue one live, or browse the run's files
.\build-release\bin\nova.exe run configs\world.cfg --set output.snapshot=runs/world/manual/snap --set output.snapshot_every=100000
.\build-release\bin\nova.exe view runs\world\manual\snap.100000
.\build-release\bin\nova.exe view --run runs\world\manual
# a picture without a window: 20,000 ticks, nodes coloured by lineage
.\build-release\bin\nova.exe view configs\world.cfg --no-output --ticks 20000 --color lineage --render world.png

# Phase 7a: a landscape -- see what the settings generate, watch it, profile it
.\build-release\bin\nova.exe land configs\archipelago.cfg --set land.scale=8
.\build-release\bin\nova.exe view configs\archipelago.cfg
.\build-release\bin\nova.exe run configs\landscape.cfg --ticks 1000 --profile --set world.nodes=10000
# Phase 7b: size the world from the machine (memory from RAM, the disk tier in files on another drive)
.\build-release\bin\nova.exe run configs\landscape.cfg --set budget.memory=25% --set budget.disk=20GB --set disk.path=E:\nova_disk

# Phase 9: measured costs -- the machine's operations, the table, a world on it
.\build-release\bin\nova.exe bench costs
.\build-release\bin\nova.exe costs home_v1
.\build-release\bin\nova.exe run configs\landscape.cfg --ticks 2000 --profile --set cost.table=home_v1

# Phase 8: programs on the GPU -- the GPU world, its profile, and warps on vs off
.\build-release\bin\nova.exe run configs\gpu_world.cfg --ticks 1000 --profile
.\build-release\bin\nova.exe run configs\gpu_world.cfg --ticks 1000 --set warp.device=gpu --set warp.verify=10
.\build-release\bin\nova.exe view configs\gpu_world.cfg
.\tools\abio.ps1 -Name p8 -Config configs\landscape.cfg -Arms baseline,gpu_world -Seeds 1,2 -Threads 12 -AfterReplicator -1 -Hours 1
```

A run reads `run.*`, `stats.window`, `replay.checkpoint_every` and `output.*`
from the config, writes CSV stats (opens in Excel), a replay log, a final
snapshot (and numbered ones every `output.snapshot_every` ticks) and, for soup
worlds, the genebank, lineage log and `result.txt`, and prints the world hash. Any config key that isn't a setting (one with no
entry in the registry, `nova keys`) is an error, so typos fail loudly; a setting whose feature is off is accepted (a setup
page's `run.cfg` names them all). `--set key=value` overrides a key before anything reads it, so the
override is logged with the config.

## Determinism rules (design.md 7.1 D)

- Every random draw is a pure function of (seed, node, tick, event counter);
  there is no shared generator.
- During a tick, a node touches only its own state and its own outboxes.
- Cross-node effects are applied at the barrier in a fixed order: destination
  node id, then source node id, then send order.
- A run's settings are fixed from its start (`docs/config-plan.md` C3): a run
  started from the setup page is exactly `nova run` on its `run.cfg`. Knob
  changes in older replay logs are timed events, re-applied before their tick.
