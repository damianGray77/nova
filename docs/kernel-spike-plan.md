# Plan: a spike for a faster warp step on the GPU

> **Revision 1, APPROVED by Damian (2026-11-06), DXC only for now:** no CUDA variant in this spike (K6). A spike: it measures, in a bench command, and changes nothing in the world's path. Every variant must give the same words as the CPU reference, or it fails.
>
> - **Why:** the GPU's warp kernel waits on its longest body, one serial step after another, at about 5.5 µs a step with the game turned down (8.6 µs with it at full settings, about 3.1 µs on an idle GPU). On the late landscape that's 16.7 ms of kernel for a 3,000-step body in `auto`, and 30.7 ms for 6,819 steps with every body on the GPU. Because a step is that slow, `auto` keeps about 17 ms of the bodies on the CPU threads (about 410 thread-ms a tick).
> - **The prize, roughly:** at about 1 µs a step, every body could go to the GPU (about 7 ms for 6,819 steps, if the GPU's throughput keeps up). The CPU's share of the bodies would go away, and a late tick would come down toward its CPU phases plus the kernel, about 20 ms against 35. The overlap plan (`docs/overlap-plan.md`) would then compound, since its CPU phases would hide under a shorter kernel. This is an estimate; the spike measures the real step time.
> - **What it decides:** whether the step is slow from **contention** (other bodies' groups crowd the long body's SM: cheap fixes in dispatch order and resources) or from the **step's own latency** (barriers and hand-overs: a wave-intrinsic kernel, which needs DXC). Then, which fix, with numbers.
> - **Source of truth:** `docs/design.md` 6.14 (warps on the GPU), 6.15 (the GPU's measured costs), 6.16 (profiling).

## 0. What a step does now

`w_run_warp` in `core/src/warp_isa.inc` is compiled by FXC (`cs_5_1`), with one thread group per body record and one thread per lane (32). Each step of the lockstep loop:
- **8 group barriers.** FXC only lets a barrier sit in a loop whose control every thread provably agrees on, so every value that steers the loop is decided by one thread and handed to the rest through groupshared memory, with a barrier on each side.
- **3 groupshared atomics:** the lowest IP (`InterlockedMin`), the count of lanes that ran, and the slowest lane's search.
- **One thread finds the step's instruction** while 31 wait: the lowest IP's block, its word, its owner and its op, a chain of dependent loads.
- **The writer check:** each lane that writes scans the higher lanes' addresses (up to 31 groupshared reads in a row) so the highest lane's value stays.
- **Memory placement:** the lane's stack is in the lane heap (global memory). The body's image is in groupshared memory only when its record fits in 2,048 words; larger records run on the plain kernel, from global memory.

The GPU's measured costs (6.15) give a sense of scale: a dependent add 1.4 ns, a load from the nearest cache 18.8 ns, from the L2 146 ns, from video memory 369 ns. A 5.5 µs step is about 38 L2 loads in a chain, or about 290 L1 loads. Nothing here yet says which parts of the step take the time; the spike's first job is to find out.

## 1. Design calls

**K1. A replay harness: `nova bench warp`.**
- `nova resume <snapshot> --ticks T --dump-round PATH` writes one round as the GPU receives it: the constants, every job's image and records, and its lanes.
- `nova bench warp PATH [--variant ...]` replays that round on the GPU through each kernel variant. Each repeat starts from a freshly loaded lane heap.
- It checks every word of the images and lanes against the CPU reference (`warp.cpp`).
- It prints, per variant: kernel ms on the device's clock (median of 5), the longest body's steps, µs per serial step, and two diagnostic rounds (K2).
- The same input every time, so variants compare like for like, without the world's split or the model getting in the way.
- *Recommendation:* as above. The late landscape (35,000 snapshot, every body on the GPU) is the main dump, plus a young world for contrast.

**K2. Diagnostics first, with what we have.**
- *The longest body alone:* a round of just that record. If it runs much faster alone than in the full round, the problem is contention, not the step itself.
- *Everything but the long bodies:* the GPU's throughput floor for the round. This is the kernel time to expect once the chain stops being the limit.
- *Which kernel the long bodies run on:* their record sizes against the 2,048-word groupshared limit.
- *Dispatch order:* the table puts groupshared-sized records first, in node order, and the large ones in a second dispatch. A long body in the second dispatch may start late.
- *Recommendation:* as above, reported before any variant is built.

**K3. Variants that need no new dependency (FXC, the same source, switched by macros).**
- (a) Long chains first: the records with the longest expected chains (the guesses the split already keeps) go first, in a dispatch of their own, so each starts at once on an SM of its own.
- (b) A larger groupshared limit. D3D12 allows 32 KB a group, so records up to about 7,900 words could run in groupshared memory. The cost is fewer groups per SM; the limit could go up for the long bodies only.
- (c) The stack in groupshared memory (2 KB a group) instead of the lane heap.
- (d) Half the barriers: double-buffered hand-overs, so a step needs one barrier where it now needs two.
- *Recommendation:* build and measure each one alone, then the winners together. These are cheap and keep the current toolchain. If K2 points at contention, (a) and (b) may be most of the answer.

**K4. A wave-intrinsic kernel (Shader Model 6.5–6.6, compiled by DXC).**
- A thread group is one hardware wave of 32 lanes, so the step needs no barriers and no hand-overs:
  - `WaveActiveMin` gives the lowest IP; `WaveActiveCountBits` the count; `WaveActiveMax` the slowest search.
  - The step's instruction is looked up by every lane as uniform code (the same loads, served once by the cache) instead of one thread plus a hand-over.
  - `WaveMatch` (SM 6.5) answers "does a higher lane write here?" in one operation.
  - The stack becomes a local array (FXC refused one in a barrier loop).
- `[WaveSize(32)]` (SM 6.6) asks for 32-lane waves on GPUs that offer other sizes. At run time, a GPU without 32-lane waves, or a world with `warp.lanes` other than 32, keeps the FXC kernel.
- `warp_isa.inc` stays one source: a few new macros (the lowest IP, the count, a broadcast, the last writer). They map to plain code in C++, to today's groupshared form under FXC, and to wave operations under DXC.
- Determinism: the wave operations used are exact integer reductions (min, max, count, match), so their order can't matter.
- *Recommendation:* as above, if K2 or K3 leave the step well above 1 µs.

**K5. The dependency for K4: DXC from the Windows SDK, at build time only.**
- The SDK the build already uses (10.0.26100, for `d3d12.h`) ships `dxc.exe` 1.9.2602 with `dxil.dll`, which signs the output.
- CMake would compile the wave kernel's variants to DXIL with it and embed the bytes in `nova.exe`, the way `warp_isa_bytes.inc` embeds the source now. There are only a few variants: 32 lanes, with and without groupshared images.
- So: no new header in the code, no DLL beside `nova.exe`, nothing new at run time. A machine whose SDK lacks `dxc.exe` builds without the wave kernel and runs the FXC one.
- *Rejected:* compiling at run time through `dxcapi.h`, `dxcompiler.dll` and `dxil.dll` shipped beside `nova.exe`. That means a new header and 15 MB of DLLs, and is only needed if the kernel changed at run time. It doesn't: `warp.lanes` and the groupshared limit are known at build time.
- *Recommendation:* approve DXC as a build-time tool on those terms. It's Microsoft's own, part of the SDK already installed, and it slots into the existing D3D12 path. Only the kernel's bytecode changes.

**K6. CUDA: not recommended.**
- The CUDA 13.5 install here holds only the Visual Studio integration and the Nsight tools. There's no `nvcc`, no headers and no runtime, so using CUDA starts with installing the compiler.
- It would mean a second backend: the round, the lane heap, the gather and the fetch in CUDA (or D3D12 interop), only on NVIDIA, with new headers in the code.
- What it adds over K4: warp shuffles (the same as wave intrinsics), an in-kernel clock (`clock64`) and Nsight Compute's per-instruction profiling. Nsight Compute 2026.4 is installed but profiles CUDA, not D3D12, and needs the GPU performance-counter permission either way.
- *Recommendation:* leave it unless K4 disappoints and we need to see inside the kernel cycle by cycle.

**K7. Scope and exit.**
- Everything is in `nova bench warp`; the world's path and every hash stay as they are.
- The spike ends with a table of variants (kernel ms, µs a step, the longest body alone, the throughput floor), every one equal word for word to the reference, and a recommendation.
- If a variant wins, a separate plan puts it in the world: determinism tests, `warp.verify`, the fallback, the split's model, and the overlap plan revisited for the new balance.
- *Recommendation:* as above.

**K8. Measuring with the game on the GPU.** Times move with the game's load (3.1 / 5.5 / 8.6 µs a step: idle, game turned down, game at full).
- *Recommendation:* interleaved repeats of every variant against the baseline in one run, medians reported, with the game's state noted alongside.

## 2. Steps

1. **The harness and diagnostics** (K1, K2): the dump, `nova bench warp`, the reference check, the baseline, the lone longest body, the throughput floor, record sizes and dispatch order. Report the numbers.
2. **FXC variants** (K3), one at a time, then together. Report.
3. **The wave kernel** (K4, K5): the DXC build step, the new macros in `warp_isa.inc`, the SM 6.6 kernel, checked word for word against the reference. Report.
4. **The write-up:** design.md 6.16 (a new item), the table, and the recommendation for the follow-up plan.

The plan can stop after step 1 or step 2 if the numbers say the rest won't pay.

### Step 1 result (the harness and diagnostics), 2026-11-06
- **The harness.** `nova resume <snap> --ticks T --set warp.device=gpu --dump-round T:PATH` writes the round; it verifies that round, so its lanes are all on the host, and the world runs on unchanged (tick 35,001's hash is 81ca99ca484aa84a with or without the dump). `nova bench warp PATH [--variants ...]` replays it. The late round (35,001, every body on the GPU) has 906 jobs, 14,475 records, 20.7 MB of image, 1.59 million lanes and 7.06 million warp steps; the longest body has 6,818. The GPU's round equals the reference's word for word: images and all 15,128 warps' lanes.
- **The step isn't slow by itself; it's crowded.** On the device's clock, medians of 5, with the game at its lowest settings:

  | what the round holds | records | warp steps | longest | kernel | µs a step of the longest | million warp steps a second |
  |---|---:|---:|---:|---:|---:|---:|
  | the whole round | 14,475 | 7,059,172 | 6,818 | 35.6 ms | 5.23 | 198 |
  | the longest body alone | 1 | 6,818 | 6,818 | 8.8 ms | 1.29 | — |
  | bodies over a quarter of the longest | 333 | 922,133 | 6,818 | 9.1 ms | 1.33 | 102 |
  | bodies up to 1/2 of the longest | 14,456 | 6,982,993 | 3,409 | 27.6 ms | 8.10 | 253 |
  | bodies up to 1/4 | 14,142 | 6,137,039 | 1,703 | 19.4 ms | 11.40 | 316 |
  | bodies up to 1/8 | 10,814 | 2,017,945 | 851 | 5.5 ms | 6.46 | 367 |
  | bodies up to 1/16 | 9,141 | 946,511 | 426 | 2.7 ms | 6.42 | 346 |

  - Alone, or with only the other long bodies, a step takes 1.3 µs. Among the short bodies it takes 4–11 µs: they share its SM.
  - The GPU's throughput tops out at about 250–370 million warp steps a second. So with today's step, the whole round can't take less than about 20 ms, however it's scheduled. Today's 35.6 ms is that plus the long bodies' tail.
  - The longest body is a 4,093-word record, so it runs on the plain kernel, in the second dispatch, after all 14,048 groupshared records have been issued.
- **FXC's optimizer makes it worse:** the whole round takes 48.1 ms at `NOVA_WARP_O=3` against 35.6 ms (it compiles for 11 minutes, too). The default of skipping it stands.
- **So the levers are throughput first** (more warps resident on an SM, less work a step) **and scheduling second** (the long bodies first and uncrowded). The K3 variants measure both.

### Step 2 result (FXC variants) and DXC on today's kernel, 2026-11-07
Every variant below gave the reference's words exactly, on both dumps. Times are the kernel on the device's clock, medians of 9–11 interleaved repeats. The game had closed by then, so the GPU was idle; with the game running, every number was higher and noisier.

- **Where a step's time goes: not the searching, and not the barriers.**
  - The reference's instruction mix (`--ops`): template searches come to 0.05 places per lane-instruction.
  - The longest body is a loop of ADD (50%), SETH (25%) and NOP (25%) with no searching, and still takes 1.29 µs a step alone.
  - K3 (d), three barriers a step instead of eight, made the round *slower* (30.8 against 25.9 ms, late; 13.9 against 9.9, young), and the lone step no quicker. So the barriers and hand-overs that a wave kernel (K4) would remove aren't where the time goes.
- **Groupshared tiers help, scheduling depends on the world** (FXC):
  - 11,103 of the late round's 14,475 records are 256 words or less, and carry 75% of the steps (79% in the young round). Today each reserves a 2,048-word image plus the constants, which caps how many fit on an SM.
  - Tiers of 256–2,048 words raise throughput.
  - Long bodies first helps the late round and hurts the young one, where 1,463 records count as long. Capping it at the 96 longest didn't settle it either.
  - Images in groupshared up to 7,680 words (72.6 against 31.9 ms) and the stacks in groupshared memory both lower occupancy and lose.
- **FXC's own code is a large part of the cost.** The same kernel source, unchanged, compiled by the SDK's DXC (`dxc -T cs_6_0 -O3`, 2 s against FXC's ~100 s) and loaded in place of FXC's bytecode:

  | whole round | FXC (today) | DXC | DXC, plain kernel only | FXC, tiers from 256, long first | DXC, tiers from 256 | DXC, tiers from 256, long first |
  |---|---:|---:|---:|---:|---:|---:|
  | late (7.06 M steps) | 26.8 ms | 23.0 | 20.0 | 18.3 | **16.4** | 16.7 |
  | young (3.59 M steps) | 12.8 ms | 11.5 | 10.9 | 10.7 | 9.7 | **9.3** |
  | late, bodies up to 1/4 of the longest (about `auto`'s share) | 14.3 ms | 10.1 | 8.5 | 9.9 | 8.6 | **7.6** |

  - DXC with tiers cuts the whole round by 24–39%, and an `auto`-sized share by up to 47%. Throughput reaches about 800 million warp steps a second.
  - A lone step barely moves (1.29 → 1.25 µs late, 1.17 → 1.06 young). The serial chain still has a floor of about 1.1–1.25 µs a step, and the longest body's 8.5 ms alone is now half the round.
- **What that says about K4.** A wave kernel's main gain, no barriers and no hand-overs, measured as nothing (K3 (d)), and a better compiler didn't shorten the lone step either. What remains for it is several bodies per thread group (occupancy past the per-SM group limit), which is uncertain. Recommendation: don't build K4 now.
- **Reproducing:**
  - `NOVA_WARP_SRC_OUT=<file>` makes any run write the kernel's source.
  - Compile it with `dxc -T cs_6_0 -E main -D W_LANES=32 -D W_LDS=<0|256|512|1024|2048> -O3 -Fo warp_32_<lds>.dxil <file>`.
  - Then run `nova bench warp <dump> --dxil <folder> --variants base,dxbase,dxt256,dxt256long`.

## 3. Tests (in the spike)
- Every variant's images and lanes equal the CPU reference's on the late and the young dumps, at every repeat.
- The existing suites pass unchanged (the world's path doesn't move).
- A build without `dxc.exe` still builds and runs (FXC only).

## 4. Risks
- DXC compiles the same source into different machine code. The integer-division rule (no `/` or `%`, 6.14) still holds in the source, and the word-for-word check catches anything else.
- `[WaveSize]` needs SM 6.6, which Windows 11's own D3D12 runtime offers with current drivers. No Agility SDK is needed, which would be another dependency. The spike checks with `CheckFeatureSupport` at run time.
- The step may be bound by memory latency in `w_exec` itself (the body's image, the lanes) more than by barriers. Then K4 helps less than hoped, and K3 (b) and (c) matter more. K2 and K3 are there to find this out before K4 is built.
