# Memory tiers (W3)

The first `mem.fast_share` of each soup is fast memory, rounded to pages;
the default is one eighth. CPU reads, writes, searches and bulk copies use
the measured 755 ps and 3.6 ns prices, and GPU lanes use 18.8 ns and 145.7 ns,
divided by each substrate's memory speed. Instruction fetch remains free.
The new fast-only ALLOC selector fails when the fast region has no room.
Fast pages have four times the wipe probability by default.

With `mem.fast_share = 0`, the selector mapping, random draws and prices
retain the legacy behavior. Old formats explicitly select that default.
Fast holdings and allocation counts precede the final compress/ambient
columns in stats.csv.

Verification: all 45 CTest suites passed on 2026-11-07. The memory-tier suite
covers tier-crossing copies and searches, substrate speeds, fast allocation,
volatility, legacy files, and a busy world at 1, 4 and 20 threads, shuffled
dispatch and snapshot resume. Its GPU test verifies every hardware round
against the CPU reference. Release measurements are recorded with the final
integration results.
