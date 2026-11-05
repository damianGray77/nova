# Nova

A Tierra-inspired digital ecosystem: programs are virtual code living in
per-node soups on a graph of nodes, owning resources governed by heat, and
(the main mode) arising from a random soup with no replicator seeded. The full
design is in [`docs/design.md`](docs/design.md), which is the source of truth.

Like ScratchML and itracker, Nova is written **without the C++ standard
library (STL) and without third-party libraries** -- we own the whole stack.
Only thin C-runtime headers (`cstdint`, `cstddef`, `cstdio`, `cstdlib`,
`cstring`, `cmath`) and platform headers (`windows.h`, `intrin.h`) are used. A
CTest entry (`includes`) enforces this; adding a header to its allowlist is a
dependency decision to discuss first.

## Status: Phase 0 (design.md 9.1)

The skeleton and the determinism guarantee. **Gate: the same seed gives an
identical world-state hash at 1, 4 and 20 threads** -- proven by the `gate`
test, which also permutes the thread pool's dispatch order (reversed and
shuffled) so it shows order independence, not just lucky timing.

No interpreter yet; that is Phase 1a. The world in `core/` is a deliberately
trivial "demo" node model that exercises the machinery later phases inherit:
counter-based RNG, parallel per-node updates, the tick barrier, and per-edge
outboxes delivered in a fixed node-id order.

## Layout

| Path | Role |
|------|------|
| `core/` | `nova_core` static library, namespace `nova`. Containers (`Array`, `HashMap`, `Str`, `TextBuf`), counter-based RNG, hashing, `key = value` config, Win32 thread pool, demo world + tick loop, run driver, snapshots, CSV stats, replay log, genebank and compressibility stubs. |
| `app/` | The `nova` command line. |
| `tests/` | One `nova_tests` executable, one CTest entry per suite, hand-rolled `tests/check.h` harness. |
| `tools/` | `check_includes.cmake`, the no-STL guard. |
| `configs/` | Example run configs. |
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

```powershell
.\build\bin\nova.exe run configs\demo.cfg --threads 20
.\build\bin\nova.exe run configs\demo.cfg --threads 1      # same hash
.\build\bin\nova.exe replay runs\demo.replay --threads 4    # re-run and check every logged hash
```

A run reads `run.*`, `stats.window`, `replay.checkpoint_every` and `output.*`
from the config, writes CSV stats (opens in Excel), a replay log and a final
snapshot, and prints the world hash. Any config key nothing reads is an error,
so typos fail loudly.

## Determinism rules (design.md 7.1 D)

- Every random draw is a pure function of (seed, node, tick, event counter);
  there is no shared generator.
- During a tick, a node touches only its own state and its own outboxes.
- Cross-node effects are applied at the barrier in a fixed order: destination
  node id, then source node id, then send order.
- Knob changes are timed events: applied before a given tick, written to the
  replay log, and carried in snapshots.
