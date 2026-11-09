#pragma once

// Reading what runs produce (docs/analysis-plan.md; docs/results/analysis.md):
// a run's files as streams, the novelty meter (A2), family trees from genomes (A4), a
// run's scores and the settings search (A1). Nothing here writes into a run
// folder or changes a world: the tools read files, and `nova search` runs its
// trials as ordinary `nova run` processes.
//
//   nova analyse novelty <run>   innovation per window from genebank.txt
//   nova analyse tree <run>      a tree by alignment and neighbour joining
//   nova analyse scores <run>    the search's scores for any run folder
//   nova search <search file>    the autopilot for tuning settings
#include "nova/array.h"
#include "nova/config.h"
#include "nova/hashmap.h"
#include "nova/isa.h"
#include "nova/keys.h"
#include "nova/learn.h"
#include "nova/profile.h" // A3: the sample's actions and the live rules
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

// --- run files, streamed --------------------------------------------------------

// A text file a line at a time, opened with shared read and write access
// (FILE_SHARE_READ | FILE_SHARE_WRITE), so a live run can keep writing it. A
// last line with no newline yet (still being written) is left out.
class LineReader {
  public:
    LineReader() = default;
    ~LineReader() { close(); }
    LineReader(const LineReader&) = delete;
    LineReader& operator=(const LineReader&) = delete;

    bool open(const char* path);
    bool is_open() const { return h_ != nullptr; }
    // The next complete line without its line end, valid until the next
    // call; false at the end.
    bool next(Str* line);
    u64 size() const { return size_; }         // the file's size when opened
    u64 consumed() const { return consumed_; } // bytes handed out so far
    void close();

  private:
    void* h_ = nullptr;
    Array<char> buf_;
    usize pos_ = 0, end_ = 0;
    bool eof_ = false;
    u64 size_ = 0, consumed_ = 0;
};

// A CSV header's columns, found by name, so added or reordered columns (other
// work adds stats columns and lineages.csv's founder_origin) don't matter.
class CsvHeader {
  public:
    bool parse(Str line);
    i32 find(Str name) const; // the column's index, or -1
    u32 count() const { return static_cast<u32>(off_.size()); }

  private:
    Array<char> text_;
    Array<u32> off_, len_;
};

// Splits a line at its commas (Nova's CSVs never quote). Returns how many
// fields it found, at most max.
u32 csv_split(Str line, Str* fields, u32 max);

// A lineage id written "node.count" (design.md 6.6), and back.
bool parse_lineage_id(Str s, u64* id);
void append_lineage_id(TextBuf& out, u64 id);
// Exactly 1 to 16 hex digits, no prefix.
bool parse_hex(Str s, u64* out);

// One genebank.txt entry (design.md 6.6). The views are valid until the next
// read.
struct BankEntry {
    Str name;   // "0031aaa": length, then letters; unique within its node only
    Str parent; // the parent's name, "unsaved" or "none"
    Str genome; // op names, space separated
    u64 tick = 0, lineage = 0, hash = 0;
    u64 parent_hash = 0; // 0 for a founder (parent=none)
    u32 node = 0;
};

class GenebankReader {
  public:
    bool open(const char* path);
    // The next entry; false at the end or at a malformed entry (then error()
    // is not empty).
    bool next(BankEntry* e);
    u64 entries() const { return entries_; }
    const char* error() const { return err_; }
    u64 size() const { return r_.size(); }
    u64 consumed() const { return r_.consumed(); }

  private:
    LineReader r_;
    Array<char> head_; // the entry's first line, copied
    u64 entries_ = 0;
    char err_[160] = {};
};

// A codon table's distinct ops as symbols 0 .. size - 1, in codon order (49
// for nova_v2: 16 doubled core ops, 32 rich ones and INERT), so a genome's op
// names become a sequence the n-gram model and the alignment can read.
class OpAlphabet {
  public:
    void init(const CodonTable& t);
    u32 size() const { return size_; }
    // Space-separated op names to symbols; false if a name isn't one of the
    // table's ops.
    bool encode(Str names, Array<u8>* out) const;

  private:
    HashMap<u32> by_name_; // hash of the name -> symbol
    u32 size_ = 0;
};

// A run folder's config: its run.cfg if there is one (a search trial), else
// the config lines at the top of its replay.log.
bool load_run_config(const char* dir, Config* cfg, TextBuf* err);
// The codon table a run used (isa.table, isa.codons; the registry's default
// when unset).
bool run_codon_table(Config& cfg, CodonTable* t, TextBuf* err);

// The lineages that met the gate: lineages.csv's `detected` rows and
// replay.log's replicator events (a living lineage has no lineages.csv row
// until it dies or the run ends). Either file may be missing.
struct GatedSet {
    HashMap<u8> ids;
    HashMap<u64> first_tick; // tick + 1 of a lineage's first replicator event in replay.log
    u64 csv_rows = 0, from_csv = 0, from_replay = 0;
    bool csv = false, replay = false;
    bool contains(u64 id) const { return ids.contains(id); }
    // Gated by the end of `tick` by replay.log's events (the live meter's view).
    bool gated_by(u64 id, u64 tick) const {
        const u64* t = first_tick.find(id);
        return t && *t <= tick + 1;
    }
};
bool load_gated(const char* dir, GatedSet* out, TextBuf* err);

// Paths.
void path_join(TextBuf& out, Str dir, Str leaf);
bool path_exists(const char* path);
bool make_dirs(const char* path); // every missing level
// True if `inner` is `outer` or inside it (both made absolute first): the
// tools refuse to write into a run folder.
bool path_inside(const char* inner, const char* outer);

// --- A2: the novelty meter -----------------------------------------------------

struct NoveltyOptions {
    u64 window = 10000;     // ticks a window
    double threshold = 3.0; // bits per codon at or above which a genotype is novel
    u32 order = 4;          // the n-gram length: 4 codons
    u32 top = 5;            // the most surprising successful genotypes listed per window
    // The live meter's rule (novelty.h; --as-live): successful only if the
    // lineage met the gate by the end of the genotype's window, by replay.log's
    // event ticks, rather than at any time in the run. With the stats window as
    // the window, the figures are the live stats columns'.
    bool as_live = false;
};

struct NoveltyWindow {
    u64 first = 0, last = 0;     // ticks
    u64 saves = 0;               // genebank entries
    u64 fresh = 0;               // ... new genotypes: one saved again (on another node) is a repeat
    u64 novel = 0;               // fresh, at or above the threshold
    u64 successful = 0;          // fresh, in a lineage that met the gate
    u64 novel_success = 0;       // ... and novel
    double surprise = 0;         // summed over the fresh genotypes (bits per codon each)
    double success_surprise = 0; // ... over the successful ones

    double innovation() const { return fresh ? surprise / fresh : 0.0; }
    double novel_share() const { return fresh ? static_cast<double>(novel) / fresh : 0.0; }
    double success_innovation() const { return successful ? success_surprise / successful : 0.0; }
};

// Genotypes in save order through a 4-codon n-gram model: each new one's
// surprise (bits per codon) is measured before the model learns from it.
// Windows by tick. Shared with the live columns that come later.
class NoveltyMeter {
  public:
    bool init(u32 alphabet, const NoveltyOptions& o);
    // One genotype: its surprise, or -1 for a repeat of one already scored
    // (neither scored nor learned again). Saves come in tick order; an
    // earlier tick joins the current window.
    double add(u64 tick, u64 hash, const u8* sym, u32 len, bool successful);
    // Closes the last window.
    void finish();
    const Array<NoveltyWindow>& windows() const { return win_; }
    const NoveltyWindow& total() const { return total_; }
    const NgramModel& model() const { return model_; }
    // New genotypes by surprise, in half-bit bins (the last: 9.5 and over);
    // [1] counts only the successful ones.
    static constexpr u32 kBins = 20;
    const u64* histogram(u32 successful) const { return hist_[successful ? 1 : 0]; }

  private:
    NgramModel model_;
    HashMap<u8> seen_;
    NoveltyOptions o_;
    Array<NoveltyWindow> win_;
    NoveltyWindow cur_, total_;
    u64 hist_[2][kBins] = {};
    bool open_ = false;
};

struct NoveltyReport {
    Array<NoveltyWindow> windows;
    NoveltyWindow total;
    u64 hist[2][NoveltyMeter::kBins] = {}; // as NoveltyMeter::histogram
    u64 skipped = 0;   // entries whose genome has an op the table lacks
    u32 alphabet = 0;
    char table[16] = {};
    u64 gated = 0;     // gated lineages known
    bool csv = false, replay = false;
    TextBuf top_csv;   // per window, the most surprising successful genotypes
};
// Streams a run folder's genebank.txt through a NoveltyMeter.
bool novelty_analyse(const char* dir, const NoveltyOptions& o, NoveltyReport* out, TextBuf* err);
void novelty_csv(const NoveltyReport& r, TextBuf& out);
// The headline: each measure's first and last quarter, its least-squares
// slope, and whether innovation is falling.
void novelty_summary(const NoveltyReport& r, TextBuf& out);

// --- A4: family trees -------------------------------------------------------------

inline constexpr u32 kTreeMaxLeaves = 4000; // neighbour joining grows as the cube

struct TreeOptions {
    bool by_lineage = false;
    u64 lineage = 0;     // --lineage: its saved genotypes ...
    u32 neighbours = 20; // ... and its founder's nearest earlier genotypes, as context
    u32 sample = 0;      // --sample N: N / 2 genotypes at random and their saved parents
    u32 max = 2000;      // leaves at most (capped at kTreeMaxLeaves)
    u32 pool = 20000;    // earlier genotypes kept to link founders to
    u64 seed = 1;        // the sample's own stream
    u32 threads = 1;     // for the distance matrix
};

enum : u32 { kLeafMember, kLeafNeighbour, kLeafPool };

struct TreeGenome {
    u64 hash, parent_hash, lineage, tick;
    u32 node, len;
    u32 off;  // its symbols in TreeInput::sym
    u32 kind; // kLeaf*
    char name[16];
};

struct TreeInput {
    Array<TreeGenome> leaves; // the tree's genotypes
    Array<TreeGenome> pool;   // earlier ones, only to link founders to
    Array<u8> sym;
};

struct TreeReport {
    u32 leaves = 0;
    u64 candidates = 0; // genotypes eligible before the cap
    bool capped = false;
    u32 links = 0;         // tracker parent links with both ends in the tree
    u32 agree = 0;         // ... whose parent is the child's nearest leaf along the tree
    u32 agree3 = 0;        // ... among its three nearest
    u32 aligned_agree = 0; // ... the nearest by alignment alone
    double parent_distance = 0; // ... the mean alignment distance from child to parent
    u32 founders = 0;      // members with no parent: founders, and arrivals on other nodes
    u32 linked = 0;        // ... given a likely ancestor
    double mean_confidence = 0;
    TextBuf newick, links_csv;
};

// Distances, the tree, the agreement and the founders' likely ancestors.
bool tree_build(const TreeInput& in, const TreeOptions& o, TreeReport* out, TextBuf* err);
// Reads a run folder's genebank.txt for tree_build.
bool tree_analyse(const char* dir, const TreeOptions& o, TreeReport* out, TextBuf* err);
// The k lineages with the most saved genotypes, a line each (to pick one).
bool top_lineages(const char* dir, u32 k, TextBuf& out, TextBuf* err);

// --- A1: a run's scores ------------------------------------------------------------

enum : u32 {
    kScoreLifespan,    // gated lineages' median lifespan, ticks
    kScoreDepth,       // their 90th-percentile depth
    kScoreGenotypes,   // distinct genotypes saved
    kScoreEvenness,    // Pielou's evenness of those genotypes over lineages
    kScoreNovelty,     // successful novelty: bits per codon per 1,000 ticks, second half
    kScorePersistence, // the share of stats windows with bodies at or above the floor
    kScoreSpeed,       // ticks a second
    kScoreCount
};
const char* score_name(u32 i);

struct ScoreOptions {
    double body_floor = 0;  // bodies a window needs to count as persisting (0: one per node)
    double novelty_threshold = 3.0;
};

struct RunScores {
    double s[kScoreCount] = {};
    u64 ticks = 0, gated = 0, saves = 0;
    double min_bodies = 0, body_floor = 0, seconds = 0;
    u64 hash = 0; // the world hash result.txt gives (0 if none)
};
bool score_run(const char* dir, const ScoreOptions& o, RunScores* out, TextBuf* err);

// --- A1: the settings search --------------------------------------------------------

struct SearchRange {
    const Key* key; // its registry entry
    double lo, hi;
    bool log;       // searched on a log scale
};

// A search file: `key = value` lines parsed with Config. Its keys are the
// search's own, not world settings, so they aren't in the registry:
//   search.base, search.out, search.seed, search.trials, search.init,
//   search.seeds, search.parallel, search.threads, search.ticks,
//   search.best, search.body_floor, search.floor_penalty, search.threshold
//   range.<setting> = lo hi [log | linear]  or  = registry [log | linear]
//   set.<setting> = value
//   score.<score> = weight      floor.<score> = minimum
struct SearchSpec {
    TextBuf path, base, out;
    Config base_cfg; // the base config, as loaded
    Config sets;     // set.*, without the prefix
    u64 seed = 1;
    u32 trials = 40, init = 20, seeds = 2, parallel = 2, threads = 2, best = 5;
    u64 ticks = 50000;
    double weight[kScoreCount] = {};
    double floor[kScoreCount] = {};
    double body_floor = 1;    // bodies per node, for persistence
    double floor_penalty = 2; // the objective lost per whole floor missed
    double threshold = 3.0;   // novelty's threshold, bits per codon
    Array<SearchRange> ranges;
};
bool search_load(const char* path, SearchSpec* s, TextBuf* err);

struct SearchOptions {
    const char* nova_exe = nullptr; // the program each run is (`nova run`)
    bool dry_run = false;           // print the plan and the first trials only
    bool quiet = false;
};
bool search_run(SearchSpec& s, const SearchOptions& o, TextBuf* err);

// The objective: sum of weight x ln(1 + score), less floor_penalty for each
// floor's shortfall (as a share of the floor). The default weights make it
// ln((1 + lifespan)(1 + novelty)): lineage length times novelty.
double search_objective(const SearchSpec& s, const double* scores);
// The setting's value for a point u in [0, 1] of its range, as config text;
// *snapped is where that value sits in the range (what the model sees).
void search_value(const SearchRange& r, double u, TextBuf& text, double* snapped);
// A trial run's full config (see search.cpp): the base, set.*, the trial's
// values, its seed and its outputs.
void search_trial_config(const SearchSpec& s, const double* u, u64 world_seed, TextBuf& out);

// --- A3: behaviour roles and group behaviour (docs/results/profiles.md) ---------------
//
//   nova analyse roles <run>    clusters of profiles.csv's samples, named by
//                               their leading actions; role shares and the
//                               honest complexity measures per window
//   nova analyse groups <run>   moving together (arrivals.csv), kin clustering
//                               and listening then acting (profiles.csv), each
//                               against chance

// One profiles.csv row as the tools read it (columns found by name).
struct ProfileRow {
    u64 tick = 0, end_tick = 0, ticks = 0;
    u64 lineage = 0, genotype = 0;
    u64 instr = 0, lane_instr = 0, copies = 0; // copies: COPY, WRITE and movii run
    u32 node = 0, genome_len = 0;
    u32 work_len = 0, distinct_ops = 0, action_kinds = 0;
    u32 role = kRoleIdle; // the live rules'
    u32 act[kPaCount] = {};
    u32 lta[6] = {};
    u32 node_bodies = 0, lin_alive = 0, geno_alive = 0, nb_left = 0, nb_right = 0;
};

class ProfileReader {
  public:
    bool open(const char* path, TextBuf* err); // reads the header; the columns it needs must be there
    // The next row; false at the end or at a malformed row (then error() is set).
    bool next(ProfileRow* r);
    const char* error() const { return err_; }
    u64 rows() const { return rows_; }

  private:
    LineReader r_;
    CsvHeader h_;
    i32 col_tick_ = -1, col_end_ = -1, col_ticks_ = -1, col_node_ = -1, col_lineage_ = -1, col_genotype_ = -1;
    i32 col_glen_ = -1, col_role_ = -1, col_instr_ = -1, col_lane_ = -1, col_work_ = -1, col_ops_ = -1;
    i32 col_kinds_ = -1, col_act_[kPaCount] = {}, col_lta_[6] = {};
    i32 col_nb_[5] = {}; // node_bodies, lin_alive, geno_alive, nb_left, nb_right
    i32 col_copy_[3] = {-1, -1, -1}; // op_COPY, op_WRITE, op_movii (whichever the table has)
    i32 max_col_ = 0;                 // the last column read (under 256: a row splits into at most 256)
    u32 fields_ = 0;
    u64 rows_ = 0;
    char err_[160] = {};
};
// Every row of a run folder's profiles.csv.
bool load_profiles(const char* dir, Array<ProfileRow>* out, TextBuf* err);
// Every row of arrivals.csv (a missing file gives none and false).
bool load_arrivals(const char* dir, Array<ArrivalRecord>* out, TextBuf* err);

// The clustering's features, per sample: each action (counts scaled to a full
// profile, log(1 + x)), the instructions run (likewise) and the share of them
// that copy. Scaled to zero mean and unit spread before k-means.
enum : u32 {
    kRfEngulfTry, kRfEngulf, kRfBiteWaste, kRfInjectTry, kRfInject, kRfInside, kRfDivide, kRfDisperse, kRfDormant,
    kRfDiskCompare, kRfDiskStore, kRfDiskLoad, kRfSelfLoad, kRfEmit, kRfListen, kRfGpu, kRfNpu, kRfSpawn,
    kRfInstr, kRfCopyShare,
    kRoleFeatures
};
const char* role_feature_name(u32 f);

struct RolesOptions {
    u64 window = 10000;     // ticks a census window
    u32 kmin = 2, kmax = 8; // the range k is chosen in (by the mean silhouette)
    u64 seed = 1;           // the analysis's own stream (seeding, subsamples)
    u32 restarts = 3;       // k-means runs per k (the lowest inertia kept)
    u32 silhouette_sample = 1500; // points the silhouette is measured on
    u32 max_samples = 200000;     // samples clustered at most (a uniform reservoir beyond)
};

struct RoleCluster {
    char name[32] = {};
    u32 size = 0;                   // samples
    double mean[kRoleFeatures] = {}; // raw means per sample (actions per full profile; copy share)
    double z[kRoleFeatures] = {};    // the centroid, scaled
    u32 lead[3] = {};               // its leading features (largest scaled centroid first)
    u32 nlead = 0;
    u32 live[kRoleCount] = {};      // its samples by the live rules' role
};

struct RolesReport {
    u64 samples = 0, clustered = 0; // rows read; clustered (all, or the reservoir)
    u32 k = 0;
    double silhouette[16] = {};     // by k (0 where not tried)
    Array<RoleCluster> clusters;    // by size, largest first
    Array<u32> labels;              // per row read: its cluster (into clusters)
    u32 live[kRoleCount] = {};      // every row by the live rules
    TextBuf census_csv, complexity_csv, clusters_csv;
};
// Clusters the rows, names the clusters, and fills the windows' census and
// complexity (the CSVs). Deterministic for a given seed.
bool roles_build(const Array<ProfileRow>& rows, const RolesOptions& o, RolesReport* out, TextBuf* err);
bool roles_analyse(const char* dir, const RolesOptions& o, RolesReport* out, TextBuf* err);
void roles_summary(const RolesReport& r, TextBuf& out);

struct GroupOptions {
    u32 gap = 2;        // moving together: arrivals this close in ticks are a pair
    u32 block = 50;     // ... compared within blocks of this many ticks per node (labels shuffled there)
    u32 shuffles = 200; // shuffles for the baselines
    u64 seed = 1;       // the analysis's own stream
    double found = 3.0; // a measure is found at z >= this
};

struct GroupMeasure {
    double observed = 0, expected = 0, sd = 0; // the statistic, and chance's mean and spread
    double z = 0;
    u64 n = 0;          // what it was measured on (pairs, sides, samples)
    u64 extra = 0;      // listening: samples individually significant (p < 0.05)
    bool found = false;
};

struct GroupsReport {
    GroupMeasure together, apart; // same-lineage arrival pairs: all; from different neighbours
    GroupMeasure kin_lineage, kin_genotype; // memory neighbours of the same lineage; the same genome
    GroupMeasure listening;       // heard value against the next branch (mutual information, pooled)
    u64 arrivals = 0, samples = 0;
    bool have_arrivals = false;
};
// The three measures, each against chance (see groups.cpp).
void moving_together(const Array<ArrivalRecord>& a, const GroupOptions& o, GroupMeasure* all, GroupMeasure* apart);
void kin_clustering(const Array<ProfileRow>& rows, const GroupOptions& o, GroupMeasure* lineage,
                    GroupMeasure* genotype);
void listening_acting(const Array<ProfileRow>& rows, const GroupOptions& o, GroupMeasure* out);
bool groups_analyse(const char* dir, const GroupOptions& o, GroupsReport* out, TextBuf* err);
void groups_summary(const GroupsReport& r, const GroupOptions& o, TextBuf& out);
void groups_csv(const GroupsReport& r, TextBuf& out);

} // namespace nova
