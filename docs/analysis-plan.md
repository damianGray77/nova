# Plan: learning from the runs (settings search, novelty, behaviour, family trees, a map of life)

> **Revision 1, APPROVED by Damian as recommended (2026-11-07).** Machine learning outside the world: tools that read what runs produce and help tune them. Nothing here changes how a world behaves. Damian asked for all five ideas to be written up. Built after the current build (`docs/world-plan.md`, `docs/disasters-plan.md`, `docs/config-plan.md`). The open details carry recommendations.
>
> - **Why:** a long run produces a lot of data with no labels. `runs\long1` saved about 1.6 million genotypes and 5.8 million lineage records in 466,000 ticks, with about a million births every 1,000 ticks. So far we've read that data by hand:
>   - finding the most complex program;
>   - looking for group behaviour;
>   - spotting the ring replicator's sweep;
>   - judging whether evolution has stalled.
>
>   And tuning about 300 settings by full-day runs is the bottleneck Damian named.
> - **Rules:** no Python and no third-party libraries. Every model here is small enough to hand-roll in C++ (Gaussian processes, n-gram models, k-means, PCA, neighbour joining). Determinism is untouched: the analysis reads files, and the two in-run parts only observe, with streams of their own.

## 0. Decided with Damian
- **All five parts:**
  - settings search;
  - a novelty and plateau meter;
  - behaviour profiles;
  - family trees from genomes;
  - a map of life.
- **Outside the world:** none of it adds to what programs can do or sense.

## 1. Design calls

**A0. Shared ground.**
- *`nova analyse <task> <run folder>`:* one command family that streams a finished or running run's files (`genebank.txt`, `lineages.csv`, `stats.csv`, `nodes.csv`, and the new profiles) without loading them whole.
- *A small hand-rolled library* (`core/src/learn/`):
  - vectors and matrices, and Cholesky solves;
  - PCA by power iteration;
  - k-means;
  - n-gram counts with smoothing;
  - sequence alignment;
  - neighbour joining.
- *Settings:* every setting the in-run parts use goes into the settings registry, so it appears on the configuration page.
- *Recommendation:* as above.

**A1. Settings search: an autopilot for tuning.**
- *What it does:* `nova search <search file>` runs many short, small worlds headless, scores each, and fits a model from settings to scores. The model picks the next settings to try, aiming at the worlds we want rather than guessing.
- *The search file* names:
  - the base configuration, by default a smaller, shorter world (for example 100 nodes for 50,000 ticks);
  - the settings to vary, with ranges taken from the registry or narrowed;
  - how many trials, and seeds per trial (2 by default, since one seed is noisy);
  - how many trials run at once, and threads per trial;
  - the score weights.
- *Scores,* computed from each trial's own outputs:
  - how long gated lineages last, and how deep they get (with the descent-based tracker);
  - diversity: distinct genotypes alive, and their evenness;
  - novelty, from A2;
  - persistence: the population doesn't crash;
  - later, complexity from A3: working code and the actions used;
  - speed, ticks a second, so a slow world costs something.

  The weights combine them into one objective. The best trade-offs (those not beaten on every score at once) are listed too, so one weighting doesn't hide good worlds.
- *The model:*
  - a Gaussian process with a length scale for each setting;
  - it starts from about 20 trials spread evenly across the ranges;
  - each next trial goes where the expected improvement is highest.
  - Hundreds of trials keep its matrices small.
  - The fitted length scales also show which settings matter and which barely do.
- *The output:* a search folder with each trial's `run.cfg` and scores, a summary CSV, the best configurations, and the settings ranked by influence. The best configurations load straight into the configuration page.
- *Checking it:* the top few configurations are rerun at full size for longer, to confirm that short small worlds predicted them (see Risks).
- *Determinism:* each trial is an ordinary deterministic run, and the search draws from its own seed, so the same search file repeats exactly.
- *Open, for Damian:* what counts as a good world. Recommendation: by default, lineage length times novelty, with a floor on diversity and population. Damian sets the weights in the search file.

**A2. The novelty and plateau meter.**
- *The model:* an n-gram model of genomes (runs of 4 codons, smoothed). Genotypes are fed through it in the order they were saved.
- *Surprise:* each new genotype's surprise is how many bits a codon the model needed to predict it, measured before it learns from it.
- *Per window, it measures:*
  - **innovation:** mean surprise;
  - **novel share:** the share of genotypes above a threshold;
  - **successful novelty:** surprise counted only for genotypes whose lineage goes on to meet the gate. This keeps random code and sparks from passing as innovation.
- *Plateaus:* a plateau shows as innovation falling and staying low while the population holds.
- *Where it runs:*
  - offline: `nova analyse novelty <run>` over the genebank;
  - live: an in-run observer adds `novelty`, `novel_share` and `novel_success` to `stats.csv`. It only reads genotypes as they're saved.
- *In the viewer:*
  - the columns are chart series;
  - a "new kinds of life" list shows each window's most surprising successful genotypes, each linked to the genome browser and its lineage.
- *A cross-check:* the soup's compressibility, already measured, is a second view of the same question.
- *Recommendation:* as above, offline first, then live.

**A3. Behaviour profiles: what programs do, not what they look like.**
- *Sampling (observe-only):*
  - Every `profile.every` ticks (1,000), a few bodies per node (`profile.per_node`, 4) are followed for `profile.ticks` (50).
  - The samples are chosen by the observer's own stream, so nothing in the world changes and the same run gives the same samples.
- *What's recorded for each sample:*
  - instructions run, by op;
  - which codons of its genome ran (a bitmap);
  - its actions: ENGULF and INJECT, tried and landed; DIVIDE; dispersals; DISK by kind, including loads into its own block; EMIT and LISTEN; GPU and NPU ops;
  - what it holds;
  - its lineage and genotype.

  This goes to `profiles.csv`.
- *Roles:*
  - the profiles are scaled and clustered with k-means, with the number of clusters chosen by fit;
  - each cluster is named by its leading actions: predator, parasite, copier, signaller, repairer (loads its own code from disk), wanderer (disperses or goes dormant often), and so on;
  - a census of role shares per window goes into the stats and charts.
- *Working code against junk:*
  - the codons that ran give each genome's working length;
  - with distinct ops run and the number of action kinds used, this makes honest complexity measures, unlike the raw length and instruction variety that made a 62-codon genome full of passenger code look "complex".
- *Group behaviour (from the review of `long1`)*, each compared with what chance would give:
  - **moving together:** same-lineage arrivals at one node bunched in time;
  - **kin clustering:** relatives side by side in memory;
  - **listening then acting:** a LISTEN whose heard value changes which way the next branch goes.
- *The cost:* sampling a few bodies per node every 1,000 ticks adds little. It's measured in step 7.
- *Recommendation:* as above.

**A4. Family trees from genomes.**
- *Building them:*
  - `nova analyse tree <run> [--lineage X]` measures how far apart genotypes are, by alignment that tolerates shifts and rotations (as the tracker fix does);
  - it builds a tree by neighbour joining.
- *What it gives:*
  - **agreement:** the share of the tracker's parent links the tree confirms. It's an independent check on the descent-based tracker.
  - **orphans:** lineages founded with no parent are linked to their nearest earlier genotype as a likely ancestor, with a confidence.
- *Scale:* neighbour joining grows as the cube of the count, so a tree covers up to a few thousand genotypes: one lineage and its neighbours, or a sample clustered first.
- *In the viewer:* the phylogeny panel can show the inferred tree beside the tracker's, with links coloured by agreement.
- *Recommendation:* as above.

**A5. A map of life.**
- *Building it:*
  - each genome becomes a vector of its 3-codon runs (hashed into 1,024 counts), plus its behaviour profile where A3 has one;
  - PCA reduces the vectors to 2D, fitted once on a reference window, so positions mean the same thing over time and the map can be animated.
- *In the viewer:*
  - a "map of life" panel shows each genotype as a point, coloured by lineage or role and sized by population, with a time slider;
  - species clusters appear, drift, split and die.
  - `--render` writes frames for an animation.
- *Sources:* offline from the genebank, and live from view frames.
- *Recommendation:* as above, PCA first. A sharper non-linear layout is a later option for single snapshots, since it doesn't hold still over time.

## 2. Steps
1. **Shared ground** (A0): the library and `nova analyse`, reading run files as streams.
2. **The novelty meter** (A2): offline, then the live columns and the viewer's list.
3. **Settings search** (A1): the search file, trials run at once, the scores (with A2's novelty), the Gaussian process, the output. Then a first real search, and its top configurations checked at full size.
4. **Behaviour profiles** (A3): sampling, `profiles.csv`, roles, working length, the group behaviour measures. A3's complexity scores then join A1's.
5. **Family trees** (A4).
6. **The map of life** (A5).
7. **Tests, the cost of sampling, the docs** (`design.md`, the README).

## 3. Tests
- *Determinism:* `nova analyse` never writes to a run. With profiles and the live novelty columns on, every world hash is unchanged at 1, 4 and 20 threads and under shuffled dispatch.
- *The novelty meter:* surprise matches hand-worked values on a toy genome stream, and a planted novel genome scores high.
- *Settings search:*
  - the Gaussian process fits a known function and finds its best point on a toy problem;
  - the same search file repeats the same trials and choices;
  - a trial's `run.cfg` reproduces its run exactly.
- *Behaviour profiles:*
  - a hand-written predator, parasite and copier land in different clusters;
  - the codons that ran match a traced run;
  - each group measure finds a planted effect and reports none on shuffled data.
- *Family trees:* neighbour joining recovers a known tree; a rotation or shift of a genome sits next to its original.
- *The map of life:* PCA recovers planted axes, and the same input gives the same map.

## 4. Risks
- **Small worlds may not predict big ones.** A short 100-node world can reward settings that fail at 1,000 nodes over a day. The full-size checks catch that. If short and long rankings disagree, the trials get longer.
- **A search games its scores.** It could find, for example, long lineages in a world where nothing changes. That's why novelty and diversity are in the objective, the best trade-offs are listed rather than one winner, and the winners are checked by eye.
- **Compute:** a search is hours of runs on a machine shared with long runs. Trials at once and threads per trial are set in the search file. A search can run overnight between long runs.
- **Names can mislead.** A cluster called "predator" is a summary of its actions, not a proof. The census shows each role's defining actions beside its name.
- **Data size:** the genebank grows by about 400 MB a day. Every tool streams, and tree-building samples.

## Result (2026-11-07)

Implemented offline novelty, search and family trees, behavior profiles with role and group reports, live novelty, and the fixed-reference PCA map with headless frames. Observer on/off hashes and snapshot continuity are checked. Long-world validation and ecological tuning remain experiments; see design section 6.23 and results/final-integration.md.
