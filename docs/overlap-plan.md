# Plan: overlapping the CPU and the GPU in a tick

> **Revision 1, APPROVED by Damian (2026-11-06) as recommended.** No world rule changes: every world must come out bit for bit as it does now, at any setting of the new key.
>
> - **Why:** Phase 10 profiling (design.md 6.16, items 1–8). On the late landscape (ticks 35,000–35,300, `auto`, the game turned down) a tick takes 35.4 ms. Nsight Systems shows the GPU working 18.1 ms a round and then idle 15.5 ms while the CPU does the work between rounds. The CPU threads are busy during the round too (the reference's share of the bodies), so neither side is idle-free and neither is saturated: the two halves of a tick run one after the other.
> - **What's been done:** the between-round phases were trimmed where they were serial (design.md 6.16 item 8): 35.7 → 35.4 ms. What's left is bound by its total work over the threads.
> - **The idea:** split the nodes into two groups and stagger them, so one group's CPU phases run while the other group's warps are on the GPU. A tick then costs about the larger of the two sides instead of their sum.
> - **Source of truth:** `docs/design.md` 6.14 (warps, the split, devices), 6.16 (profiling).

## 0. The numbers it rests on

Per tick, late landscape, wall time unless marked:

| Part | Time |
|---|---:|
| Before the round: first halves 2.9, packing 2.0, the split 1.4 | 6.3 ms |
| The round: GPU kernel 16.5 (longest GPU body about 2,900 steps) + transfers 1.8, CPU threads on the reference's share at the same time (about 430 thread-ms) | 23–24 ms |
| After the round: unpacking 0.4, the host's lanes 0.6, host instructions 0.3, second halves 1.5 | 2.8 ms |
| Accelerators and the barrier | 1.6 ms |

- All the CPU's work in a tick comes to about 575 thread-ms (first halves 60, packing 43, the reference's share about 430, unpacking 7, host 5, second halves 30): about 25 ms on 23 threads if nothing waited.
- The GPU's kernel is set by its longest body (it runs a body's steps one after another): 16.5 ms for about 2,900 steps, 32.5 ms for 6,819 with every body on the GPU.
- Nodes by their longest GPU body against the limit (about 1,800): 96–99 have no GPU body, about 25 are under a quarter of it, 67–123 under half, 331–435 under three-quarters, and 320–480 at or above three-quarters. Almost every node has a long body. So a group can't be given only short bodies: each group's GPU batch will take about as long as the whole round does now, and the two batches must run **at the same time** on the GPU for the overlap to pay. Whether this GPU does that is the plan's first question (step 1).

## 1. Design calls

**C1. The unit of overlap: node groups, each a whole slice of the tick.** Each node's tick stays exactly as now (first half → the warp rounds → second half), and nodes interact only at the barrier, which still comes after every node. So which group a node is in, and when its slice runs, can't change the world. The group split decides only timing.
- *Recommendation:* two groups.

**C2. Two GPU batches in flight, on two queues.** Today `WarpGpu` holds one round at a time: one queue, one command list, one set of upload, image, readback and gather buffers. A batch per group, each with its own queue, command list, buffers and fence, lets the second group's kernel start while the first's is still running. The lane heap stays shared: each node has its own region, and the groups' nodes are disjoint. Growing the heap (when a node outgrows its region) copies every region, so it happens only with no batch in flight: if the second group needs it, that tick waits for the first batch (rare: regions double).
- *Recommendation:* as above, behind a gate. If the two queues' kernels don't run concurrently (step 1 measures it), the plan stops there: run one after the other, two batches each about as long as today's would cost more than they save.

**C3. Which nodes go in which group.** Each group's CPU work should be about the same, since the second group's first half and packing overlap the first group's kernel, and the first group's unpacking and second half overlap the second's.
- *Recommendation:* deal the nodes out by last tick's CPU time per node (first half, packing and second half, already measured), slowest first, each to the group with less so far.

**C4. The schedule in a tick (two groups, A and B).**
1. A's first halves, packing and split; A's batch goes to the GPU (queue 1).
2. B's first halves, packing and split; B's batch goes to the GPU (queue 2).
3. The CPU threads run A's then B's share of the bodies on the reference.
4. A's batch comes back: A's unpacking, host lanes and instructions, any later rounds, second halves.
5. B's batch comes back: the same for B.
6. The barrier, as now.
- The GPU is idle only during step 1 and the end of step 5 plus the barrier: about 3 + 3 ms instead of about 11.
- *Recommendation:* as above.

**C5. What isn't staggered.**
- `warp.verify` ticks run as now, one group (its comparison keeps both devices' results side by side).
- Ticks without the GPU (`auto`'s plain ticks) have nothing to overlap.
- Later rounds in a tick (warps a host instruction freed, with quota left) run within their group's step 4 or 5, one after another, as now within a node.
- `warp.groups = 1` is today's tick exactly, for tests and as a fallback.
- *Recommendation:* as above.

**C6. The split's model.** Today it balances the GPU's kernel against the CPU threads' share of the bodies, with the CPU's other phases outside the round. Staggered, the CPU threads also do the other group's phases while the GPU works, so the same split would leave the CPU the slower side. The model should balance the GPU's time against all the CPU's work in that window (its share of the bodies plus the other group's phases, both measured), which sends more of the long bodies to the GPU.
- *Recommendation:* build the schedule with today's model first and measure, then retune the model to the window (step 4).

**C7. The key.** `warp.groups`: 1 or 2.
- *Recommendation:* default 2 if step 3 shows it quicker, else 1. More than two only if the measurements suggest it.

**C8. Seeing it.** `nova resume --profile` prints each group's phases and the GPU's two batches (kernel time on the device's clock each, and how much they overlapped); an Nsight trace shows the two queues side by side.
- *Recommendation:* as above.

## 2. Steps
1. **The gate.** Two batches on two queues in `WarpGpu`. In a test build, split the late world's round into two batches sent back to back (no CPU overlap yet) and compare the GPU's time with one batch. Report the numbers and stop if they don't overlap.
2. **Groups.** `warp_step` runs the groups one after another (`warp.groups = 2`, no overlap yet): the per-round state (jobs, lists, the reference's records, lanes to fetch) per group; the per-node passes over a group's nodes. The determinism tests (section 3) pass here before any overlap.
3. **The stagger.** The schedule of C4. Measure against `warp.groups = 1` (interleaved runs on the late snapshot, and a young world), and take an Nsight trace.
4. **The model** (C6), the default (C7), design.md and the README.

### Step 1 result (the gate), 2026-11-06
- **A bug found and fixed.** Two batches first changed the world (tick 35,005, nodes 817–999). When batch A grew the lane heap, it copied every node's lanes to the new heap on its own queue, and batch B started on the new heap before the copy reached B's nodes. Now a batch waits on the GPU (`ID3D12CommandQueue::Wait`) for the other queue whenever either one copies the heap, and so does a fetch. Since the fix, one batch and two give the same hash at every tick from 35,001 to 35,200 (3b6c54d7eeedd854), in `gpu` and `auto`.
- **The kernels do run at the same time** (late landscape, ticks 35,000–35,200, the round's jobs halved, sent back to back, no CPU overlap):

  | | one batch | two batches |
  |---|---:|---:|
  | `gpu`: device time of the round | 33.5 ms | 37.4 ms span (kernels 20.7 and 23.6, overlapping 28.6) |
  | `auto`: device time of the round | ~19.3 ms | 22.1–22.7 ms span (kernels 12.5–13.1 each, overlapping 16.4–17.0) |
  | `auto`: warp rounds, wall | 23.9–25.2 ms a tick | 25.9–26.5 ms a tick |

- **But the GPU doesn't favour either batch.** B's list starts 1.2 ms after A's. Then B's upload waits about 8 ms behind A's kernel, and A's gather and download wait behind B's kernel. So A, with half the work, comes back after 17.6–19 ms, against about 19.7 ms for the whole round alone. Halving the round costs 3–4 ms of device time (12%). A high-priority queue for A made no difference on this GPU and driver.
- **What that does to the estimate.** With today's model: A's preparation (~3.2 ms), then max(GPU ~22 ms + round overheads ~3, B's preparation ~3.2 + the reference's share ~17), then B's post (~1.4) and the barrier (~2.4). That comes to about 31–32 ms a tick against 35.4, not 26–30. Retuning the model (step 4) might add a little.

### Steps 2-3 result (built 2026-11-07, after the CPU reference got twice as fast)
- **Built:** `warp.groups` (1 or 2). With 2, `World::warp_round0_groups` runs a tick's first round in two node groups on the GPU's two batches: A's first halves, packing and split, its batch (queue 0); B's the same while A's runs (queue 1); the reference's share of both; then A's batch back, its unpacking, host instructions and (with one round a tick, the default) second halves while B's runs; then B's. Groups are dealt by last tick's CPU time; auto's limit for A is last tick's and B takes a fresh one from every node's records; the model times the two batches as one round (their span). Verifying or dumping ticks, and auto before its model has rates, run one group as before.
- **The same world:** the young landscape resumed from tick 1,000 reaches the same hash at 1,300 with 1 and 2 groups (74892deea287b31e), in `auto` and `gpu`; the warp suite checks `gpu`, `mixed` and `auto` with 2 groups against the reference at 1, 20 and 4 threads and through a snapshot.
- **But it doesn't pay with today's split.** Ticks 1,000-1,300 of the young landscape (two interleaved runs each): `auto` with one group 18.3-18.4 ms a tick, with two 20.5-20.6 ms. With every body on the GPU two groups do help (23.7 -> 20.9 ms), but `auto`'s one group is quicker than both. Since the warps' shared step work (f5adc7f) halved the reference's time, `auto` gives the CPU threads a share of the round as long as the GPU's, so the CPU isn't idle during the round any more; grouping only adds the second batch's device time (the gate's 12%) and the CPU's other phases on top of that share.
- **Default:** `warp.groups = 1` (C7). Step 4 (the model retuned for the window) could only move work from an already-busy CPU to a GPU made slower by the split; it is not built. Worth measuring again if the kernel gets much quicker (section 5) or the CPU slower.

## 3. Tests
- The same hash with `warp.groups` 1 and 2 on the busy warp world, the `home_v1` warp world, the eco world and the defaults world; at 1, 4 and 20 threads; under shuffled dispatch; through a snapshot mid-run; with `warp.device = mixed`.
- `warp.verify` with `warp.groups = 2`: no differences.
- From the 35,000 snapshot: `gpu`, `auto` and the reference to one hash with `warp.groups = 2` (ec7042dbf711bc1f at 36,000, as now).
- A device error with a batch in flight stops the world as now (`warp_device_error`).

## 4. Expected gain and risks
- *Gain:* if the kernels overlap and the model is retuned, a late tick of roughly 26–30 ms instead of 35.4: the CPU's total work (about 25 ms on 23 threads) becomes the floor, and the retuned split moves some of it to the GPU. This is an estimate; step 3 measures it.
- *Risks:*
  - Two queues writing one buffer at once (disjoint ranges) must be allowed for the heap's state; the gate checks it on this GPU.
  - More GPU memory: a second set of round buffers (about 25 MB each way).
  - Complexity in `warp_step` and `WarpGpu`; `warp.groups = 1` keeps the old path whole.
  - If the second group's batch needs the heap to grow, that tick loses its overlap.

## 5. Not in this plan: a faster kernel
The kernel waits on its longest body at about 5.7 µs a serial step: each step of a 32-lane warp takes several group barriers and shared-memory atomics. A kernel built around one hardware warp (wave intrinsics in shader model 6, which needs the DXC compiler, a new dependency; or CUDA, also a dependency) could cut that several times over. The GPU could then take nearly every body, and the CPU threads' share, today most of their work, would mostly go. That is likely the larger gain, and it compounds with this plan. It needs its own dependency decision and a measuring spike first.
