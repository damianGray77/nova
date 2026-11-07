# Viewer and live analysis (A2, A5, W8)

Genebank saves feed the shared novelty model. Stats expose mean surprise,
novel share and successful novelty; the viewer converts the fixed-point
columns to displayed units. New kinds of life link to the genome browser.
Observer history is not snapshotted: a resumed world starts a fresh novelty
model, without changing its simulation hash.

The map of life hashes 3-codon runs into 1,024 counts and fits PCA on a fixed
reference. A time slider filters points without refitting their coordinates;
headless rendering writes animation frames. Live populations size points.
The substrate map cycles all eight resource types, disaster markers show
their kind and fade, and RunArchive reads the per-node substrate, occupancy
and disaster columns. The memory strip exposes a fast-region accessor for
the memory-tier integration.

Tests cover live/offline agreement, stable PCA coordinates, novelty on/off
world hashes and snapshot resume, archive columns, linked genome selection,
all substrate modes, disaster markers, chart series, and headless panels.
The final integration adds the real memory boundary and the combined
1/4/20-thread, shuffled-dispatch and snapshot determinism gate.
