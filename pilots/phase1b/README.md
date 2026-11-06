# Phase 1b pilots (THROWAWAY)

Exploratory code behind the pilot findings in [`docs/phase1b-plan.md`](../../docs/phase1b-plan.md), section 1.

It exists only so those numbers can be reproduced, for example on another machine. **It is not part of the engine:**
- CMake and `verify.ps1` never build it;
- the candidate rules it switches on are quick hacks behind environment variables, not config keys;
- nothing here is logged to a replay.

**Delete this folder once Phase 1b's real rules land.** Whatever the plan settles on gets re-implemented properly in `core/`, with config keys, tests and the determinism gate.

## What's here
| file | role |
|---|---|
| `proto.patch` | Adds the candidate rules to a copy of `core/` as environment toggles. Made against `main` at `2ef4571` (Phase 1a). |
| `diag.cpp` | The driver. Runs one soup node and prints its health at ticks 100 / 1k / 5k / 10k / 20k / 50k: free memory, average free CPU pool, bodies, sparks and births, plus the size of a seeded genome's lineage. |
| `build.ps1` | Copies `core/` to `build/core`, applies the patch and compiles `build/diag.exe` (MSVC, /O2). |
| `run.ps1` | Re-runs the experiment sets: `clog`, `alloc`, `share`, `upkeep`, `surface`, `predation`, `cpuwear`, or `all`. |

`build/` is git-ignored: the patched copy, objects, `diag.exe` and run outputs (`build/out/<set>/<run>.txt`).

## Build and run
```powershell
.\pilots\phase1b\build.ps1
.\pilots\phase1b\run.ps1 -Set upkeep               # one set, 8 runs at a time
.\pilots\phase1b\run.ps1 -Set all -Parallel 15     # everything (takes a while)
.\pilots\phase1b\run.ps1 -Set clog -MaxTick 1000   # quick check: stop at tick 1,000

# one run by hand: overrides (';'-separated), seed, optional max tick
$env:PROTO_UPKEEP = '2'; $env:PROTO_EXP = '1.3333'
.\pilots\phase1b\build\diag.exe "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE;seed.at = 30000;seed.millicores = 10" 9 1000
.\pilots\phase1b\build\diag.exe - 7 1000   # "-" = no overrides (PowerShell 5.1 drops an empty "" argument)
```
Toggles stay set in the PowerShell session until removed (`Remove-Item Env:PROTO_*`).
- **Default node.** It's the bench node from design.md 7.1 I: 65,536 codons, 4,000 millicores, 25,000 instructions per core per tick, and sparks of 400 initial, 4 per tick, grant 10, length 8–64. Any key in the overrides replaces the matching default.
- **Determinism.** A run is deterministic for a given seed, overrides and toggles.
- **Noise.** Most findings come from one or two seeds, so treat them as directional.

## Toggles (environment variables; unset = Phase 1a behaviour)
| variable | effect |
|---|---|
| `PROTO_PENDING=1` | ALLOC (memory) fails while the body (or, with `PROTO_PERCPU`, this CPU) still holds an undivided block. |
| `PROTO_PERCPU=1` | The pending block, DIVIDE's daughter and RELEASE's block become per CPU instead of per body. |
| `PROTO_MULT=k` | ALLOC (memory) fails above k × the length of the block the CPU runs in. |
| `PROTO_UPKEEP=u` | Memory upkeep: each tick, before it runs, a body pays u·m·(m/64)^(e−1) instructions, where m = codons owned. Unpaid upkeep becomes debt and counts as work, so it heats the body. |
| `PROTO_EXP=e` | Upkeep exponent: 1 = linear, 1.3333 = square-cube (the revision-3 candidate), 1.5. |
| `PROTO_SPARK_LATE=1` | Sparks fire after the bodies have run, so they take only CPU that life left unclaimed. |
| `PROTO_CPU_FAULT=f` | CPU wear: each held millicore fails back into the free pool with probability f per tick. |
