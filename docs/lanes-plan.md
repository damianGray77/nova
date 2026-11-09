# Plan: each warp its own lanes; swallowed prey hand over their cores

> **Agreed by Damian, 2026-11-09** ("agreed"), after the points below were
> settled in conversation. Built the same day (`docs/design.md` 6.29). Points
> marked *(Claude's call)* were filled in while building; say if any should
> change.
>
> Decided in conversation (Damian):
> - He asked why small programs paid more on wide nodes ("I thought the lane
>   cost was consistent regardless of size?"). They did because every warp
>   was its node's width and every step paid for all of its lanes. Of the
>   three fixes offered (pay per 32-lane chunk still running; pay only the
>   lanes that run; let a program choose its lanes), he liked the second "but
>   I also think #3 is more natural". The first two help only once spare
>   lanes stop running, and a lane past a small program's code rarely halts;
>   the second also makes branching free, which is what makes GPU code
>   different from CPU code. So: **a warp's lanes are its own, chosen at
>   launch, and it pays for the lanes it asked for** (L1-L3).
> - "Even if the biter can't utilize those resources immediately, the victim
>   should not be able to either." Of freezing a swallowed prey's cores or
>   handing them over as it is digested, he chose the second: "This seems
>   fairer for both swallower and victim." (H1).

## Why

long6 (2026-11-09, ticks 0-383,000) never got replicators going. Copying per
node fell with the node's width: over the run, 46-58 copies a node at 32-64
lanes, 17 at 256, 4.7 at 512, 2.3 at 1,024. long5's takeoff (ticks
115,000-125,000) was straight-line GPU programs of 32 and 62 codons whose one
or two COPY steps copy the whole genome on a 32-lane warp. On a wide node the
same program paid for 1,024 lanes a step, nearly all of them reading past its
code.

## L. Lanes

**L1. A warp's lanes** (Damian). Each warp has its own lane count, a power of
two from `warp.lanes_min` to its node's width (the node's width is now the
most a warp there may have).
- *Launch:* the GPU verb's launch (BX 20) asks for CX lanes: the first power
  of two from `warp.lanes_min` that holds them, at most the node's width. CX
  0 gets `warp.lanes_min` *(Claude's call: powers of two keep the seven GPU
  kernels; the narrowest is the floor)*.
- Only its lanes run. Heads move by them, SENSE 21 reads them, and the step's
  lockstep, write phase and live-lane counts are over them. The rest of its
  slot is halted and never runs.

**L2. Where a warp's lanes come from** *(Claude's call, except the first)*.
- A daughter born by a warp's DIVIDE gets that warp's lanes (Damian), cut to
  the node's width if she disperses; they travel with her as cargo.
- Any other body born on the GPU -- a spark, the seed, a CPU's daughter born
  on the GPU because she holds only GPU units -- gets the fewest lanes that
  cover her code (a lane a codon). For a straight-line copier that is the
  width at which one COPY step copies it whole.
- A dormant program keeps its first warp's lanes and wakes with them (cut to
  where it wakes); a crash restarts a GPU-born body's warp with its first
  warp's lanes.

**L3. Costs** (Damian). A step costs the warp's own lanes: with a timed table
lanes x a GPU lane-op, every lane whether it runs or not (a step fills the
warp's slots, as on a GPU); with the classic table `warp.step_cost` plus
`warp.lane_cost` a lane that runs, as before. Upkeep paid from warps and a
warp's host instructions cost a step of the body's first warp *(Claude's
call)*.

**L4. Seeing it.** `stats.csv` gains `lanes`, the lanes of every warp alive
(divide by `warps` for the mean). The viewer shows each warp's own lanes.

**Not done yet** *(Claude's call)*: each warp slot still holds its node's
width of lanes, and the GPU still runs a group of the node's width for every
record, its threads past a warp's lanes idle. So memory and the real GPU's
time don't shrink yet; storing lanes at each warp's width and dispatching by
it is a follow-up.

## H. Swallowed prey

**H1. Cores go with the codons** (Damian). Each tick a swallowed body loses
`engulf.digest` codons to its eater (inject-plan S2), and with them the same
share of its millicores (each kind), GPU and NPU units, oldest first, each
keeping its heat: the share its digested codons were of its memory that
tick, rounded to the nearest *(Claude's call)*. It runs on what it has left
and may still INJECT its way out (S4); its eater can use what it has taken at
once. Its threads, warps and disk go with the rest of it when nothing of its
code is left, as before.

## Format

Snapshot v10 (Warp grew 8 bytes, Dormant 4, each cargo a word; older ones
refused); the warp round dump is version 6 (each warp entry's WW_LANES and
WW_STEPC).
