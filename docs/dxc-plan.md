# Plan: the warp kernel compiled by DXC, with groupshared tiers

> **Revision 1, APPROVED by Damian (2026-11-07) as recommended.** No world rule changes: every world must come out bit for bit as it does now, whichever kernel runs.
>
> - **Why:** the kernel spike (`docs/kernel-spike-plan.md`, steps 1–2; design.md 6.16 item 9). Today's kernel source, compiled by the Windows SDK's DXC instead of FXC, with groupshared tiers sized to the records, runs the GPU's round 24–39% faster: late round 26.8 → 16.4 ms, young round 12.8 → 9.3 ms. An `auto`-sized share runs up to 47% faster (14.3 → 7.6 ms). All of that is word for word the reference's.
> - **What it builds on:** the spike plan's K5, approved: DXC as a build-time tool only. No new header in the code, no DLL beside `nova.exe`, nothing new at run time.
> - **Not in this plan:** the wave-intrinsic kernel (the spike found barriers aren't the cost) and the overlap plan, which stays paused at its step 1 and is revisited once this lands, since the balance it was planned around will have moved.
> - **Source of truth:** `docs/design.md` 6.14 (warps on the GPU), 6.16 (profiling).

## 0. What the spike measured

On an idle GPU, the kernel on the device's clock, medians of 9 interleaved repeats, every variant equal to the reference:

| | FXC (today) | DXC, same kernel | DXC + tiers from 256 words | + long bodies first |
|---|---:|---:|---:|---:|
| late round (7.06 M warp steps) | 26.8 ms | 23.0 | **16.4** | 16.7 |
| young round (3.59 M) | 12.8 ms | 11.5 | 9.7 | **9.3** |
| late, bodies up to 1/4 of the longest (about `auto`'s share) | 14.3 ms | 10.1 | 8.6 | **7.6** |
| young, bodies up to 1/4 of the longest | 3.8 ms | 3.0 | 4.1 | 3.2 |

- **Tiers:** 75–79% of the steps are in records of 256 words or less. Today each reserves a 2,048-word groupshared image, which limits how many fit on an SM.
- **Compile time:** DXC compiles the kernel in about 2 s; FXC takes about 100 s.

## 1. Design calls

**D1. The kernel's source becomes a file.**
- `core/src/warp_kernel.hlsl` holds what are now the C++ string literals `kWarpHead` and `kWarpMain` in `accel_dx.cpp`, with `#include "warp_layout.inc"` and `#include "warp_isa.inc"` between them.
- DXC compiles that file at build time. FXC still compiles it at run time, from embedded bytes (as `warp_isa.inc` is embedded today), through a small include handler that serves the two `.inc` files.
- So there's one kernel source for both compilers, and still one interpreter source for the GPU and the CPU reference.
- *Recommendation:* as above.

**D2. DXC at build time.**
- CMake looks for `dxc.exe` beside the SDK's `rc.exe`, so it's the same SDK the build already uses (10.0.26100 here).
- For 32 lanes it compiles the plain kernel and four groupshared tiers (256, 512, 1,024, 2,048 words): `-T cs_6_0 -O3`, about 2 s each, in parallel. The SDK's `dxil.dll` signs each one.
- Each is embedded in `nova.exe` as bytes (`tools/embed_bytes.cmake` already handles binary files), about 0.5 MB in all.
- Without `dxc.exe`, CMake prints a message and the build carries FXC only. `-DNOVA_DXC=OFF` does the same on purpose, to test that build.
- *Recommendation:* as above.

**D3. Which kernel runs.**
- DXC's when the world's `warp.lanes` is 32 (the default and every preset), the device offers Shader Model 6.0 (`CheckFeatureSupport`), and the pipelines build.
- Otherwise FXC's: today's kernel exactly, with its one 2,048-word tier and the plain kernel, and a warp note saying why.
- A key, `warp.kernel = dxc | fxc`, for tests and comparisons; it's speed-only, handled like `warp.device`. Default `dxc`, which falls back as above.
- The results are the same either way. The warp notes and `--profile` say which kernel ran.
- *Recommendation:* as above. The 2- and 4-lane test worlds keep exercising FXC, and one 32-lane test runs with `warp.kernel = fxc`.

**D4. Groupshared tiers, DXC only.**
- A record runs on the smallest tier that holds it; larger records run on the plain kernel. Each kind is its own dispatch, with no barrier between them (records are independent).
- FXC keeps today's single tier: five FXC kernels would take about 8 minutes to compile the first time, for a fallback.
- *Recommendation:* as above (`WarpKernelOptions::lds` from the spike becomes the world's).

**D5. Long bodies first.**
- The records with the longest expected chains go first, in dispatches of their own, longest first. The expected chain is the split's guess, which the world already keeps per body (`Soup::warp_record_chain`); a long record is one over a quarter of the longest.
- Measured: it helps an `auto`-sized share (late 8.6 → 7.6 ms, young 4.1 → 3.2) and the young whole round (9.7 → 9.3). It costs 2% on the late whole round (16.4 → 16.7).
- It changes only the dispatch order, never the result.
- *Recommendation:* on, with no key. Step 4's world measurements confirm it, and the bench keeps the comparison.

**D6. Measuring it in the world.**
- Interleaved runs of `warp.kernel` dxc against fxc:
  - from the 35,000 snapshot, 200–300 ticks, `auto` and `gpu`;
  - from the 5,000 snapshot;
  - a young world from the start (1,500 ticks).
- The split's model measures the GPU's time per step live, so it should send the GPU more bodies on its own. Check where it settles (the limit, the GPU's share of body-rounds, the CPU threads' share) and the tick time.
- *Expected, roughly:* the `auto` tick from about 35 ms to about 27–30 ms (the kernel's 25–45% cut, then the split rebalancing). This is an estimate; step 4 measures it.
- *Recommendation:* as above, with the game off the GPU where possible, noting it when not.

**D7. Cleaning up after the spike and the gate.**
- *Remove:*
  - `W_FEW_SYNC` in `warp_isa.inc` and the groupshared-stack option (both lost);
  - the `dxil_dir` option and `NOVA_WARP_SRC_OUT` (replaced by D1–D2);
  - the bench's losing FXC presets;
  - the overlap gate's temporary code: `NOVA_TWO_BATCHES`, the `g_*` profile fields, the TEMP print and the `<cstdlib>` include in `world.cpp`.
- *Keep:*
  - `nova resume --dump-round` and `nova bench warp`, with `--ops`, and variants for FXC or DXC, tiers or not, long first or not;
  - `WarpKernelOptions` (tiers, long first, which compiler);
  - `WarpGpu`'s two batches with the queue-wait fix, for when the overlap plan resumes.
- *Recommendation:* as above.

## 2. Steps
1. **The source file** (D1): `warp_kernel.hlsl`, with FXC compiling it through the include handler. The world is unchanged; every suite passes. FXC's cached bytecode is recompiled once, since its key is the source.
2. **DXC** (D2–D4): the CMake step, the embedded kernels, the choice and its fallback, the tiers, `warp.kernel`.
3. **Long bodies first** (D5), from the world's chain guesses, and the cleanup (D7).
4. **Measure** (D6). Then design.md (6.14's kernel section, a 6.16 item) and the README.

### Steps 1–3 result (built, 2026-11-07)
Built as recommended:
- `core/src/warp_kernel.hlsl`, compiled by FXC through an include handler;
- the DXC step in `core/CMakeLists.txt`: five 32-lane kernels, about 107 KB each, embedded;
- `WarpGpu::open` choosing DXC or FXC, with the fallback, and `kernel()` saying which and why;
- the tiers;
- the long bodies first, from `warp_job_chain_`;
- `warp.kernel`, and `--profile` naming the kernel;
- the cleanup.

Test results:
- Suite `warp` checks both kernels: the busy world with every round verified, and the division loop. The Release and Debug suites are green.
- A `-DNOVA_DXC=OFF` build passes every suite, with the FXC note.
- From the 35,000 snapshot, `gpu`, `auto` and the reference all reach ec7042dbf711bc1f at 36,000.

### Step 4 result (measured, 2026-11-07)
On an idle GPU, with the game closed.

- **The kernel** (`nova bench warp`, medians of 9, every kernel equal to the reference word for word):

  | | FXC (before) | DXC (now) |
  |---|---:|---:|
  | late round, whole | 29.6 ms | **16.1 ms** (−45%) |
  | late, bodies up to 1/4 of the longest (about `auto`'s share) | 14.4 ms | **7.2 ms** (−50%) |
  | young round, whole | 13.6 ms | **9.2 ms** (−32%) |
  | young, bodies up to 1/4 of the longest | 3.8 ms | **2.8 ms** (−27%) |
  | a lone step (the longest body alone), late / young | 1.29 / 1.17 µs | 1.25 / 1.06 µs |

- **The world** (interleaved runs of `warp.kernel`, the same hashes with both kernels):

  | | FXC | DXC |
  |---|---:|---:|
  | late, `auto`, ticks 35,000–35,300 (hash 379499639c6179d9 at 35,300) | 28.3 / 28.2 ms a tick | **25.6 / 25.8** (−9%) |
  | ... its kernel; the CPU threads' share of the bodies | 13.4; 13.6 ms | 10.4–10.5; 10.9–11.0 ms |
  | ... the split's limit; body-rounds on the GPU | 2,300; 96.8% | about 2,870; 97.3% |
  | late, `gpu`, ticks 35,000–35,200 (3b6c54d7eeedd854) | 38.7 ms a tick (kernel 24.2) | **31.9** (kernel 17.5; −17%) |
  | young, `auto`, ticks 5,000–5,300 (21cb195afdc9da29) | 19.8 / 19.8 ms a tick (kernel 7.5) | 19.7 / 19.5 (kernel 6.8–6.9; −1%) |
  | from the start, `auto`, 1,500 ticks (9180bf4f019b3edf) | 19.25 s | 18.87 s (−2%) |

- **Why a tick gains less than the kernel:**
  - `auto` rebalances. The GPU takes more bodies (the limit rose from 2,300 to about 2,870), and the round settles with both sides at about 10.5 ms.
  - A round also carries about 4 ms beyond the larger of the two sides: the copies in and out, and the upload and download.
  - The young world is bound by its CPU phases (about 7 ms of first halves, packing and the rest a tick), not by the kernel.
  - `gpu`, which has no CPU share to rebalance, keeps more of the kernel's gain (−17%).
- **Startup:** with 32 lanes nothing compiles at run time. The kernel is ready in about 30 ms, where FXC's first compile took about 3 minutes before it was cached.
- **What limits a tick now:** the round's overhead (about 4 ms), the CPU's phases between rounds (about 7.5 ms late), and the longest chain (8.5 ms alone, about 16 ms crowded). The overlap plan (`docs/overlap-plan.md`, paused) targets the second; its numbers need redoing against this balance before it resumes.

## 3. Tests
- Suite `warp`: the GPU checks run DXC's kernels (32 lanes) and FXC's (the 2- and 4-lane worlds, and a 32-lane world with `warp.kernel = fxc`), every word equal to the reference.
- `warp.verify` on the busy warp world with DXC: no differences.
- From the 35,000 snapshot: `gpu`, `auto` and the reference reach one hash with DXC (ec7042dbf711bc1f at 36,000, as now).
- `nova bench warp` on the late and young dumps: every kernel equal to the reference. These dumps put records on every tier and on the plain kernel.
- The determinism suites unchanged: the same hashes at 1, 4 and 20 threads, under shuffled dispatch, and through a snapshot.
- A build with `-DNOVA_DXC=OFF` builds, runs FXC, and passes.

## 4. Risks
- **Short-circuiting:** DXC's HLSL 2021 short-circuits `&&`, `||` and `?:`. `warp_isa.inc`'s rule (no side effect in their operands) makes that safe; the word-for-word checks would catch anything else.
- **Unsupported devices:** a GPU or driver without Shader Model 6.0, or one that refuses the DXIL, runs FXC's kernel with a note. The RTX 5080 and the laptop's RTX 3050 Ti both offer 6.0.
- **Signing:** DXIL must be signed. The SDK's `dxc.exe` signs with the `dxil.dll` beside it; an unsigned kernel would fail at pipeline creation and fall back to FXC.
- **The model's balance:** it moves toward the GPU, and the CPU threads' share shrinks. If the late tick ends up bound by the longest chain (about 8.5 ms alone, 16.4 ms crowded), that's the next limit to look at.
- **Startup:** quicker. With 32 lanes nothing compiles at run time, where today the first run compiles FXC for about 3 minutes before caching.
