# Plan: INJECT moves the program; swallowed prey are digested; warp widths by node

> **Approved by Damian, 2026-11-09** ("Approved", after the points below were settled in conversation). M and S are built (`docs/design.md` 6.27, with the calls made while building); W is built (6.28), with its ceiling raised to 1,024 lanes (Damian, while building). Points marked *(Claude's call)* were filled in while writing this up; say if any should change.
>
> Decided in conversation (Damian):
> - **INJECT inserts the program, not a copy** (M1). "It should bring everything over, including machinery. The parasite is still a living functioning entity within the host program, so the old program should not die."
> - The memory it leaves is **zeros: free space, but not information** (M1).
> - It may overwrite its host's code; the host may overwrite it (M2). It runs only on its own machinery, never the host's cores or NPU (M3).
> - To propagate it **copies itself inside the host**; its daughters take over the host's resources; the host eventually dies, "just like viruses in a cell" (M4). When the host dies the parasite lives on (M5).
> - **INJECT fails unless the victim is at least twice the injector's size** (M6). Meant to stop daughter infection as a way to propagate, and more realistic. *Lowered to "just larger" (Damian, 2026-11-10; see M6).*
> - INJECT no longer costs what COPY does: priced like a bite instead, its mirror image (M7).
> - **A whole swallow doesn't kill at once**: the prey is digested over time, long enough to INJECT itself into its eater and stop the digestion (S).
> - **Each node's GPUs have their own warp width, 16 to 256 lanes** (W).
>
> Source of truth for today's rules: `docs/design.md` 6.1 (private code), 6.14 (warps), 6.16 (bites), 6.26 (wounds, INJECT, parasites); `docs/bites-plan.md`.

## 0. Why: what long5 shows (2026-11-09, ticks 750,000-1,225,000)

- INJECT copies today: CX codons from the injector's read head are written into the victim, a thread starts there, and the injector carries on. So it is COPY by another route.
- A CPU family (22 codons, INJECT and no COPY) reproduces by **budding a blank daughter and injecting its own code into her**. At tick 800,000 two-thirds of the 17,788 bodies carrying a parasite thread were this family; it was about 40% of all descendants (genome survey, ticks 748,000-757,000).
- The rest of the world is GPU biter-copiers (32 and 48 codons) and, since tick ~920,000, a boom of sleeping 47-48-codon biters.
- 32 codons is the single strongest genome length because one 32-lane COPY copies it whole.

## 1. INJECT moves the program (M)

**M1. What moves** (Damian). A landed INJECT moves the injecting body into its victim. The body keeps its identity: its serial, lineage, threads, millicores, GPU and NPU units, warps and disk all stay its own. What changes is where its code lives:
- Its genome (its first block, whole) is written into the host (M2).
- Every block it owned is released to waste **cleared to codon 0** (INERT in the default tables): free space, not information, so nothing can bud or scavenge a copy from it.
- Its threads carry on in the moved code: an IP, head or stacked address in its old first block moves by the same offset into the new place; anything that pointed elsewhere goes to the start of its code.
- It is now a **guest** of the host: a parasite.

**M2. Where it lives** (agreed). Its genome overwrites the host's codons just inside the wound it came in by, as I2 does today: the host block's last codons for a tail wound, its first for a head wound. The codons stay the host's (the host's memory), so:
- the parasite may read, write and run anywhere in its host;
- the host may overwrite the parasite (an immune response can evolve);
- bleeding and bites at that end take the parasite's code with the host's.
- The parasite's region (where its code was written, clipped as the host's first block shrinks) is kept with it for M4 and M5.
- A genome whose outer codon is a NOP seals the wound behind it, as today.

**M3. Machinery** (agreed). A parasite runs only on what it brought: its own millicores and GPU and NPU units. It never uses the host's. A parasite that brought no cores can't run.

**M4. Propagating** (agreed). A parasite multiplies by copying itself inside its host:
- Its ALLOC carves the block out of the host's first block, from the end away from the parasite, not from free waste. Every copy eats the host. (It fails if the host has nothing left outside the parasite's region.)
- Its DIVIDE releases the copy as a free-living daughter (not a guest). She takes her share (CX/64) of the **host's** millicores and GPU and NPU units.
- The host loses code and machinery with every copy, and dies when it has no thread or core left (today's death rule).

**M5. The host dies** (Damian: "the parasite lives on"). Its guests are freed:
- a parasite's region (what's left of it) becomes its own block, its threads' IPs inside it;
- a swallowed body (S) simply stops being swallowed.
- The rest of the host goes to waste as today.

**M6. Size** (Damian). An INJECT lands only if the victim owns more than `contact.inject_ratio` (1) times the memory the injector owns. A bud is the same size as its parent, so injecting one's own daughter fails. The room rule stays: the host's first block must be at least the injector's genome (`contact.inject_room`, 1).
- *Was "at least 2 times" until 2026-11-10.* long7 showed it shut replicators out: with nearly every program 32-48 codons, lineage members landed 2 of about 17,800 INJECT attempts in a night, and 99.8% of parasites were sparks (5-25 codons) with no lineage. Damian: "lower the barrier of entry to just larger rather than 2x. This should allow parasites to almost completely overwrite their hosts if the hosts are small enough while still making physical sense, and I think this is a viable strategy (eg. parasitic wasps)."

**M7. Price** (Damian: not COPY's price). Priced like a bite, its mirror image:
- `contact.cost_per_codon` per codon moved, as ENGULF pays;
- it lasts the codons moved at `engulf.codons_per_tick` (8) with a timed table, its thread busy meanwhile, as an ENGULF;
- its start is charged (the bug the COPY change fixed stays fixed).
- So moving a 22-codon program takes 3 ticks. The COPY-price code (`copy_time`, `pay_at_start`) goes.

**M8. What a parasite can't do** *(Claude's call)*.
- It can't INJECT again (it spreads only by M4), bite, disperse or go dormant.
- It has no surface of its own, so it isn't a target for bites or INJECTs; harm reaches it through its host (M2).
- A host can carry several parasites; a parasite's own daughter blocks are its own (private) memory.

**M9. Engineering** (from the code map).
- Guest state lives in per-body side arrays: host, kind (parasite or swallowed) and, for a parasite, its region's offset and length in the host's first block. They're saved and hashed with the soup; snapshot version up. Body's raw layout is unchanged. A guest's threads keep `Cpu::parasite` set.
- One rights rule, used everywhere ownership is checked today: a body may use a codon it owns, or, if it is a parasite, a codon its host owns. This covers execution, read, write, templates, search span, the warp record (a parasite's warp record includes its host's first block, marked as its own), the accelerator barrier and `Soup::check`.
- Warp records then overlap (host and guest). Each record still reads the round's starting memory, and records are written back in body order, so the result stays deterministic and the same on CPU and GPU.
- The detector keeps a guest's lineage alive while it has no block of its own (a guest count per lineage). A parasite's threads carry the injector's tag (`cpu_tags_`), so its daughters join its lineage via the parasite link, as now.
- A host with guests doesn't go dormant (frozen too long, it dies instead and frees them). A guest can't go dormant or disperse. A collapse kills guests with everything else.
- Nothing INJECTs into a resting dormant program either (Damian, 2026-11-09: "refuse dormant hosts"), so no dormant body ever holds guests (`Soup::check` holds it to that). Allowed before, a parasite could sit in a dormant host that then drifted to another node without it; its host's slot was reused, and the guest count broke (long7, tick 106,000). Letting guests and holdings travel with a program is an open question.

## 2. Swallowed prey are digested (S)

**S1. Swallowed, not dead** (Damian). A bite big enough to swallow its prey whole makes the prey a **swallowed guest** of its eater instead of killing it. It keeps its threads, memory and machinery, and keeps running on its own cores.

**S2. Digestion.** Each tick a swallowed body loses `engulf.digest` (2) codons of its memory to its eater, as a bite piece does, from the end of its first block. The pieces go to one growing block of the eater's (its "stomach", the latest block, extended while the pieces stay adjacent), not a new block each tick. `engulf.digest = 0` keeps today's instant swallow. Only bites swallow this way; the old whole-body ENGULF (`engulf.bite = false`) is unchanged.
- When it has no memory left, it dies and its eater gets its remaining machinery (millicores, GPU and NPU units, disk, warps), as a whole swallow gives today.
- If it dies first (its threads halt), the same happens at once.
- A 22-codon prey lasts 11 ticks.

**S3. While swallowed** *(Claude's call)*. It can't ALLOC, DIVIDE, bite, disperse or go dormant, and it doesn't bleed. Nothing else can bite it or INJECT into it.

**S4. The way out** (Damian). A swallowed body's INJECT targets its swallower:
- it needs no wound (it is already inside);
- the size rule (M6) still applies, and always passes for a whole swallow, since a bite is at most a third of its eater;
- it is written just inside the eater's first block at the end nearest the prey.
- A landed INJECT makes it a parasite of its eater (M), and digestion stops.

**S5. The eater dies** first: the prey is freed, a free body again.

## 3. Warp widths by node (W)

**W1. Widths** (Damian: 16 to 256; later "bump it up to 1024", then "Set the default range to the full range, 16 to 1024"). Each node's GPUs have their own warp width, drawn from the world seed like the node's other properties (`derive_node`):
- a power of two from `warp.lanes_min` (16) to `warp.lanes_max` (256), each width equally likely *(Claude's call: powers of two, as real hardware has)*;
- `warp.lanes` goes;
- `nodes.csv` and the viewer show each node's width.
- SENSE 21 already reports the width a warp runs at. Dispersal already rebuilds a warp at its destination's width.

**W2. Costs.** Unchanged in form:
- with a timed table a warp step costs `width x gpu_thr`, so a wide node's every step costs more and copies more;
- with the classic table, `step_cost + lane_cost x lanes`.
- A 256-lane COPY copies 256 codons in one step.

**W3. Engineering** (from the code map; the GPU path assumes one width today):
- the lane count and step cost move from the shared constants into the warp record (as `WB_SIZE` and the tier prices already are);
- a kernel per width (16, 32, 64, 128, 256), each built by DXC at build time with its groupshared tiers (256 lanes leaves at most 7,410 words of image), and dispatches split by width;
- the GPU heap's per-node lane regions use the node's width;
- the CPU reference goes to 256 lanes: `kMaxLanes` 256, a 256-bit flaw mask, and an O(lanes) write-conflict pass (highest lane wins, as now) instead of today's O(lanes²) one;
- the result stays bit-identical on CPU and GPU.

## 4. Seeing it

`stats.csv` gains:
- `parasites`: gauge, guests living in hosts;
- `swallowed`: gauge;
- `digested`: codons;
- `escapes`: swallowed bodies that injected into their eater;
- `freed`: guests outliving their hosts.

`injects`, `parasite_divides` and `infected` keep their meanings; `parasite_threads` becomes the number of programs that moved into a host. Events gain `escape` (the first gated lineage to inject its way out of a swallower). The viewer marks guests in the memory strip.

## 5. Steps

1. M7 and M6: INJECT priced like a bite; the size rule.
2. M1-M5, M8: guests. A body field for its host and kind, rights for guests, ALLOC carving, DIVIDE's share from the host, freeing at the host's death, the detector's parasite tags. Snapshot version up (old snapshots refused).
3. S1-S5: swallowed guests and digestion.
4. W1-W3: warp widths by node.
5. Tests for every rule; suites moved to the new rules; pins re-pinned; `docs/design.md` 6.27, keys and README.
