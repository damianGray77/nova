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

## Status: Phase 1a (design.md 9.1)

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
| `core/` | `nova_core` static library, namespace `nova`. Containers (`Array`, `HashMap`, `Str`, `TextBuf`), counter-based RNG, hashing, `key = value` config, Win32 thread pool, world + tick loop, run driver, snapshots, CSV stats, replay log, genebank and compressibility stubs. Phase 1a: `isa` (codon tables), `soup` (a node's organisms, ledger, heat, hazards, sparks, death sweep), `interp.cpp` (the instructions). |
| `app/` | The `nova` command line, including `nova bench`. |
| `tests/` | One `nova_tests` executable, one CTest entry per suite, hand-rolled `tests/check.h` harness. |
| `tools/` | `check_includes.cmake`, the no-STL guard; `bench.ps1`, the Release benchmark. |
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
.\build\bin\nova.exe run configs\soup.cfg --threads 20      # the Phase 1a soup world
.\tools\bench.ps1                                          # Release build + interpreter benchmark
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
