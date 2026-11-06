# Phase 2b plan: a richer GPU and the in-world NPU

> **Revision 1, APPROVED by Damian as recommended (2026-11-05) and BUILT.** What the engine does, and the first short check, are in `docs/design.md` 6.8. The GPU and NPU ledgers were folded into one `UnitLedger` (one instance per unit kind) while building. The text below is the plan as approved.
>
> - **Asked for by Damian (2026-11-05):** "a richer instruction set to the gpu, and also add npu into the mix". This un-defers the tensor/NPU unit that design.md 5.1 D deferred.
> - **Source of truth:** `docs/design.md`; the GPU as built is 6.7.
> - **Long runs:** checked with runs of minutes; the 12-hour runs wait until everything is built (9.1).

## 0. What stays fixed
- **Virtual hardware (design.md 3):** the in-world GPU and NPU are simulated resources, implemented on the CPU. Organisms never touch the real GPU or NPU.
- **Off means unchanged:** `world.npus = 0` (the default) leaves everything exactly as Phase 2, and `world.gpus = 0` still leaves everything exactly as Phase 1b.
- **No free codon:** the 64-slot table is full (61 is GPU; 58–60 are EMIT, LISTEN and DISK, waiting for Phase 4). So the NPU has to share a verb (B3).

## 1. Design calls

**B1. The richer GPU: 16 ops.** BX 0–15 picks the op through a 16-entry selector map. Elements run from the read head (source) to the write head (destination), as now.

| BX | op | effect | kind |
|---:|---|---|---|
| 0 | compare | CX ← how many source/destination pairs differ | reduction |
| 1 | count | CX ← how many source codons equal AX mod 64 | reduction |
| 2 | find | CX ← offset of the first source codon equal to AX mod 64; the read head stops on it; fails if absent | reduction |
| 3 | sum | CX ← the sum of the source codons | reduction |
| 4 | copy | dest ← source, with copy errors | elementwise |
| 5 | fill | dest ← AX mod 64 (reads nothing) | elementwise |
| 6 | not | dest ← 63 − source (the complement) | elementwise |
| 7 | rcopy | dest ← source read backwards (the read head runs down): a reversed copy, with copy errors | elementwise |
| 8 | add | dest ← dest + source, mod 64 | elementwise |
| 9 | sub | dest ← dest − source, mod 64 | elementwise |
| 10 | xor | dest ← dest xor source | elementwise |
| 11 | and | dest ← dest and source | elementwise |
| 12 | or | dest ← dest or source | elementwise |
| 13 | min | dest ← the smaller | elementwise |
| 14 | max | dest ← the larger | elementwise |
| 15 | mul | dest ← dest × source, mod 64 | elementwise |

- *Order:* 0 stays compare, the op that changes nothing, as for DISK; the rest group by kind. This moves Phase 2's copy from BX 1 to BX 4.
- *Recommendation:* as above. Left out on purpose: translate (a codon lookup table, i.e. a programmable genetic code — powerful, worth its own call later) and gather/scatter.

**B2. Partial work and reductions.**
- *Elementwise ops* keep Phase 2's rule: they do as many elements as this tick's capacity allows and leave the rest in CX.
- *Reductions (compare, count, find, sum)* become all-or-nothing. They need capacity for all n elements, and every element readable; otherwise they fail and change nothing. A partial count can't be told from a full one, so a program could never trust it. Find is charged only for the elements it scans up to the match.
- This changes Phase 2's compare, which did partial work.
- *Recommendation:* as above.

**B3. One verb for both units.** The GPU instruction becomes the accelerator verb:
- BX 0–15 runs a GPU op on the body's GPU units; BX 16–19 runs an NPU op on its NPU units.
- It keeps the name GPU, so saved genomes and genebank names stay valid.
- This is the "verbs plus selectors" rule (6.2), the way ALLOC covers every resource.
- NPU ops sit at high selector values, reachable by arithmetic (INC then SHL four times gives 16). The NPU is something to be discovered after the GPU.
- *Recommendation:* as above. The alternative, taking a codon from another instruction, would change every existing table.

**B4. The NPU as a resource.** `world.npus` units per node (default 0), held exactly like GPU units:
- ALLOC and RELEASE with a new resource selector 4 (the ALLOC/RELEASE map grows to 5 entries when there is an NPU);
- the DIVIDE share, ENGULF, death, and `npu.wear`;
- per-unit heat `heat.npu_gain` and `heat.npu.*`, freed past max;
- SENSE readings 11 = my NPU units, 12 = their mean heat, 13 = free NPU units on the node.
- Capacity is counted in multiply-accumulates: each unit does `npu.macs` (default 256) a tick, times its heat speed factor.
- *Gain:* the NPU runs hot like the GPU, gain 9 (settles near 71 flat out).
- *Recommendation:* as above, all knobs.

**B5. What the NPU computes.** Codons are read as signed fixed-point values: v − 32, i.e. −32…31 standing for −1…+1 in 32nds. Results saturate back into 0–63.

| BX | op | effect | MACs |
|---:|---|---|---:|
| 16 | dot | CX ← Σ (source[i] − 32)(dest[i] − 32), signed, over n = CX elements | n |
| 17 | matvec | k = CX; vector v at the read head (k codons), matrix M at AX (k×k, row-major); dest[j] ← sat(Σᵢ (M[j][i] − 32)(v[i] − 32) ÷ 32) + 32 | k² |
| 18 | matmul | k = CX; A at the read head, B at AX, both k×k; dest ← sat(A·B ÷ 32) + 32, k×k | k³ |
| 19 | argmax | CX ← offset of the largest source codon among n = CX (the first on ties) | n |

- All NPU ops are all-or-nothing: they need capacity for every MAC and every element readable (and writable for the outputs).
- *What it is for:* a genome can carry weights and compute a weighted sum (a neuron) over memory it has written sensed values into, then pick an action with argmax. That's "neural" decision-making evolution can reach, and the NPU is the only cheap way to do MAC-heavy work.
- *Recommendation:* as above. Kept to four ops.

**B6. Costs.** The CPU pays the same setup (`gpu.setup`, 4) for either unit, plus `gpu.cpu_per_element` (1/16) per GPU element or `npu.cpu_per_mac` (1/64) per NPU MAC. An 8×8 matmul is 512 MACs: two NPU units' whole tick, and 12 CPU instructions.

**B7. Watching for use (detection).**
- GPU writes keep marking codons as GPU-written; NPU outputs count as ordinary writes.
- A new replay event `npu_user` fires the first time a member of a lineage that has met the gate does an NPU op. That's the NPU counterpart of `gpu_replicator`, since the NPU doesn't copy.
- New columns: `npu_macs`, `npu_released`, `npu_worn`, `npu_held`.

**B8. Shuffled control.** The GPU op map (16 entries) and the NPU op map (4 entries) are both drawn in the shuffled arm, each redrawn until its first op isn't left at 0. The SENSE map is drawn over the readings that exist: 8, 11 with a GPU, 14 with an NPU too. Readings for a missing unit fail.

**B9. Harness, bench, tests.**
- Arms: `npu` (`world.npus = 200`, `npu.wear = 0.001`) and `accel` (GPU and NPU both), beside `gpu`.
- Bench: an `npu` workload, a hand-written seeded program doing matvecs (tests and bench only).
- Tests cover every op, the all-or-nothing rules, the NPU ledger, sensing, the maps, NPU-off identical to Phase 2, the determinism gate with both units, and `npu_user` replaying.

**B10. The real NPU (out of scope).** Running in-world NPU ops on the machine's real NPU would be an optimisation for later, like real-GPU batching in Phase 6.
- It would need a dependency (DirectML or OpenVINO), which the no-third-party rule says to discuss first.
- 7.1 G judged the real NPU a poor fit for the interpreter. The in-world NPU's matmuls are tensor-shaped, though, so that's worth measuring when we get there.

## 2. Evidence
- `verify.ps1` green with the above.
- A short check: `baseline`, `gpu`, `npu` and `accel` arms, a few seeds, a few minutes each.
- A bench including the `npu` workload.

## 3. Docs
- design.md: 6.7 extended (or a new 6.8), 5.1 D (the NPU un-deferred), 9.1, the status line.
- README.
