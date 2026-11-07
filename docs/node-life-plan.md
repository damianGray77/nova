# Plan: rarer node collapse, and migration that works

> **Revision 1, for Damian's approval (2026-11-07).** World rule changes, each behind a key, measured before it becomes the default.
>
> - **Why:** Damian, after the hour-long run: about 500 node collapses, each a mini extinction that held back growth. Either collapse should be rarer, or programs should be able to migrate far more easily so a collapse wipes out less.
> - **Source of truth:** `docs/design.md` 4.4/4.4a (collapse, starvation, wear, recovery), 6.9 (dispersal: DIVIDE to a neighbour, transit, arrival), 6.10 (the Phase 4 lifecycle).

## 0. What the measurements say

From the late landscape snapshot (`runs/det_gpu/snap.35000`, 1,000 nodes, the default world). New diagnostic counters in `nova resume` (they change no result; the hashes are the same with and without them):

- **Every collapse is starvation; none is wear.** 29 collapses over 5,000 ticks (35,000–40,000), all starved. They took about 86 bodies each, against about 110 on an average node.
  - Starvation means a node's free CPU pool stayed under `node.starve_below` (0.2% of its millicores, about 4 of 2,000) for more than half the time. The meter ebbs 0.00025 a tick, so it settles at 4,000 under constant starvation and collapses at 2,000.
  - **A node that stays full collapses after about 2,800 ticks.** In practice that means a node at carrying capacity: its programs hold all its CPU because they're thriving, not because they're idle hoarders. Starvation collapse was meant to punish hogging (design 4.4a A), but it can't tell hogging from success. So it wipes out the busiest nodes and the lineages that filled them.
  - Wear never fires: `node.endurance` is 10⁹ writes, and no node got there in 40,000 ticks.
- **Migration mostly fails on arrival.** Over 2,000 ticks (35,000–37,000), 12,953 daughters arrived on a neighbour, and **10,978 of them (85%) found no free millicore and died in their first sweep.**
  - An arriving daughter draws her CPU from the destination's free pool, capped by what's there (4.4a E). The neighbours of a crowded node are crowded too.
  - On top of that, a disperser names its destination by number (BX → neighbour k, nearest first), so a lineage keeps sending to the same neighbour.
  - Transit isn't the problem (40,000-tick run): of 346,131 daughters sent, 1.3% found no room in the destination's memory, 0.9% were corrupted, and 81 were lost in transit.
- **Lineages rarely establish elsewhere.** In the 40,000-tick run, `colonised` (nodes holding a living member of a lineage founded elsewhere) stayed near zero, even though 341,500 daughters arrived somewhere. So when a node collapses, its lineages are almost always gone with it.
- **Heat isn't involved:** halving `heat.ambient` changed nothing that matters (29 → 36 collapses over 5,000 ticks, noise-level). It's back at 0.05.

## 1. Design calls

**L1. Starvation should measure hogging, not success.**
- Today a tick counts as starving when the free pool is nearly empty.
- Proposed: it counts only when the pool is nearly empty **and** most of the held CPU is idle. Idle means units held by bodies that did little work that tick (their CPU heat input, `Body::speed`, below a fraction of the node's mean); the exact measure would be settled in step 1.
- A node full of working replicators no longer starves. A node whose CPU sits with idle holders still does, which is the tragedy-of-the-commons price 4.4a A intended.
- Keys: `node.starve_idle` (the idle share that counts as hogging; 0 = today's rule).
- *Alternatives:*
  - (a) Simply rarer: `node.starve_ticks` 2,000 → 3,500, so a node must be full for about 8,300 ticks (3× longer). One key, but it still punishes success, only later.
  - (b) Gradual: starvation raises the node's error rates and volatility first, as wear does (4.4a C), and collapses only at the meter's cap.
- *Recommendation:* L1, measured against (a). Keep (b) in reserve if collapses are still too sharp.

**L2. Arrivals wait for room.**
- Today an arrival with no free millicore dies.
- Proposed: she waits at the destination, as cargo with no CPU that doesn't run, for up to `edge.wait` ticks (default 100). At each barrier, waiting arrivals take free millicores as they appear, in the fixed delivery order (arrival tick, then source, then send order), so it stays deterministic.
- She takes her room in memory when she's placed, as now. Past `edge.wait` she dies, counted as `lost`.
- It's the world running what the code asked (a daughter sent there), with a queue in place of instant death. Crowded nodes still turn most migrants away, but a node that frees CPU now and then can take them in.
- *Recommendation:* as above. It addresses the 85% directly.

**L3. A destination for "the roomiest neighbour".**
- A DIVIDE destination that picks the neighbour with the most free millicores, ties going to the nearest.
- It's read from the counts taken at the last barrier, never from another node mid-tick, so it stays deterministic.
- It's reachable like the numbered destinations, through the selector map, so it's one mutation away from a disperser's DIVIDE. Then a lineage can evolve to send its daughters where there's room, instead of always to neighbour 1.
- *Recommendation:* after L2, only if arrivals still mostly fail. It changes the ISA's meaning of one destination value, so it's a bigger step.

**L4. Leave transit alone.** Edge capacity, bandwidth and latency cost 1–2% of cargo. They aren't what stops migration.
- *Recommendation:* no change.

**L5. Measuring it.**
- The new counters go into the stats CSV: `collapsed_starved`, `collapsed_worn`, `arrived_dry`, and with L2, `waited` and `wait_lost`. `nova resume` prints them.
- Runs, one change at a time, then together:
  - from the 35,000 snapshot, 10,000 ticks;
  - from the start, 40,000 ticks (the hour-long scale).
- Compare:
  - collapses (and their cause);
  - arrivals that survive;
  - `colonised` and colonist events;
  - bodies, lineages, and gated replicators alive (`detect_alive`).
- *Recommendation:* as above. Each key defaults to on only if its runs show fewer wipe-outs and more spread without wrecking the population.

## 2. Steps
1. **The counters** into the stats CSV and `nova resume`'s summary (already in `nova resume`, uncommitted).
2. **L1** behind `node.starve_idle`, measured against today and against (a).
3. **L2** behind `edge.wait`, measured.
4. Both together; the defaults; design.md (4.4a, 6.9, 6.10) and the README.
5. **L3** only if step 4 still shows most arrivals dying.

## 3. Tests
- Each key at 0 is today's world exactly: the same hashes in every suite.
- Suite `lifecycle`:
  - a node full of busy bodies no longer starves under L1;
  - a node of idle holders still does;
  - arrivals wait and are placed when CPU frees (L2);
  - a waiter past `edge.wait` is lost.
- Determinism: the 90-node landscape with L1 and L2 on gives the same hash at 1, 4 and 20 threads, under shuffled dispatch, and through a snapshot taken while arrivals are waiting (waiting cargo goes into snapshots).

## 4. Risks
- **L1's idle measure** could be gamed (a hog working just enough to look busy). The idle threshold is a knob, and (a) stays available.
- **L2** lets crowded regions hold a queue of waiting migrants; `edge.wait` bounds it, and waiting cargo takes no CPU or memory until placed.
- **Fewer collapses means less habitat renewal** (returning nodes, ruins, succession). If renewal matters, a modest wear collapse could take its place: `node.endurance` low enough to fire now and then, with the gradual decline as warning.
