#pragma once

// The view frame (Phase 6, design.md 6.12; plan C10): everything the viewer's
// panels draw, built by plain functions with no Win32 in them, so it can be
// tested. The Win32 shell (app/) only draws frames and forwards input.
//
//   ViewSession  watches a live Run. after_step() after every Run::step()
//                keeps per-node rates and chart samples; build() fills a frame
//                between ticks (the only time the world may be read, 7.1 D).
//                Watching never changes the world: a session reads, nothing
//                more (a test checks the hash).
//   RunArchive   the same frames from a finished run's files (stats.csv,
//                nodes.csv, replay.log, genebank.txt), without simulating.
//
// A frame carries the state now (nodes, edges, cargo, knobs, the selected
// node and body) and the *new* things since the previous frame (chart
// samples, events, genebank saves), which the viewer appends to its logs.
//
// docs/results/viewer.md adds: the live novelty meter's "new kinds of life"
// (A2), the map of life (A5, LifeMap), substrates, occupancy and the
// disasters' per-node figures on the map (world plan W8, disasters X7), and
// the memory tiers' fast region in the strip (W3, view_fast_end).
#include "nova/array.h"
#include "nova/hashmap.h"
#include "nova/learn.h"
#include "nova/replay.h"
#include "nova/run.h"
#include "nova/stats.h"
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

// --- colours (0x00RRGGBB) --------------------------------------------------

// A stable colour per lineage id (territories): the same id always gets the
// same colour, in any frame and any run.
u32 lineage_color(u64 id);
// A sequential ramp for t in 0..1 (dark blue through teal and green to
// yellow), and a heat ramp (black, red, orange, white).
u32 ramp_color(double t);
u32 heat_color(double t);
// Mixes a toward b by t (0..1).
u32 mix_color(u32 a, u32 b, double t);

// --- what the map can show -------------------------------------------------

enum ColorBy : u32 {
    kColorBodies,
    kColorBirths,
    kColorColonists,
    kColorHealth,     // live/fallow and health
    kColorLineage,    // the largest lineage: territories
    kColorFreeCpu,
    kColorWaste,
    kColorMemory,     // node properties from here on
    kColorMillicores,
    kColorClimate,
    kColorSparks,
    kColorRadiation,
    kColorDisk,
    kColorVolatility,
    kColorSignals,    // signal activity
    kColorLanes,      // GPU lanes running (Phase 8 warps)
    kColorEcores,     // E millicores' share of the node's (Phase 9c)
    kColorMaterial,   // the share of its waste that isn't dead matter (Phase 10)
    // A varied world (docs/world-plan.md W8) and the disasters (X7, X10):
    kColorSubstrate,  // one substrate type's level (the viewer picks the type: node_metric's sub_type)
    kColorOccupancy,  // the share of its memory held by living bodies (W5)
    kColorSymptoms,   // the share of the ticks it had disaster symptoms
    kColorOffline,    // its cores offline (burnout)
    kColorCount
};
const char* color_by_name(u32 c);

// Substrates on the map (W8): a level's colour, the floor deep red, 0.5x
// orange, 1x a neutral grey-blue, 1.5x green and 2x bright teal; and the
// level (0-4) a multiplier stands for, given the floor's (substrate.floor).
u32 substrate_level_color(u32 level);
u32 substrate_level_of(double mult, double floor_mult);

// How the memory strip is coloured.
enum StripBy : u32 { kStripOwner, kStripLineage, kStripHeat, kStripStamp, kStripCodon, kStripCount };
const char* strip_by_name(u32 s);

// One node as the map sees it.
struct NodeView {
    float x, y; // map position in 0..1 (hidden coordinates, or a circle in a ring world)
    u32 live;   // 1 a node, 0 a fallow site (lifecycle)
    u32 life;   // lives after the first
    u32 bodies, cpus;
    u32 health; // 0-100
    u32 lineages, colonists;
    u32 top_alive;
    u64 top_lineage; // id of the lineage with the most members here, 0 = none
    u32 soup_size, millicores, free_millicores, waste;
    u32 gpus, npus, disk, degree;
    float births, deaths, sent, arrived, emits; // per tick, since the previous frame
    float cool, spark_rate, radiation, volatility;
    u32 holding; // living copies of the requested genotype here
    u32 known;   // bit k set: colour-by k has data (an archive lacks some)
    u32 mass;    // its landmass (a landscape, Phase 7; 0 otherwise)
    u32 lanes;   // GPU lanes running: not halted, in live warps (Phase 8)
    u32 gpu_bodies; // bodies born on a GPU
    u32 emillicores; // of its millicores, E (Phase 9c)
    float material;  // the share of its waste that isn't INERT, sampled (Phase 10)
    // A varied world (W8): each substrate type's multiplier and level (1x and
    // level 2 without substrates), and its occupancy (W5).
    float sub[kSubstrateTypes];
    u8 sub_level[kSubstrateTypes];
    float occupancy;
    // The disasters (X7, X10): the share of the ticks since the previous frame
    // (an archive: the window) it had symptoms, the disasters that hit it in
    // that time, and its cores offline now.
    float symptoms;
    u32 struck, cores_offline;
};

// A landmass with nodes on it (Phase 7): where to label it, and how many.
struct MassView {
    float x, y; // the centroid of its nodes, in map coordinates
    u32 id, nodes;
};

// The number the map colours a node by, and whether it has a fixed range
// (health 0-100, fractions 0-1); otherwise the viewer scales over the nodes.
// sub_type: kColorSubstrate's type (SubstrateType); its number is the level.
double node_metric(const NodeView& n, u32 color_by, u32 sub_type = 0);
bool metric_range(u32 color_by, double* lo, double* hi);

struct EdgeView {
    u32 a, b;      // a < b: each undirected link once
    u32 down;      // a failed link (Phase 4)
    u32 long_link; // much longer than its neighbours'
    u32 bridge;    // a bridge between landmasses (Phase 7) ...
    u32 shut;      // ... shut this tick
};

struct CargoView {
    float x, y; // where it is now, between its ends
    u32 len;
    u32 pad;
};

// A body of the selected node.
struct BodyRow {
    u64 serial;
    u64 lineage;  // 0 = not a lineage member
    u64 genotype; // when it joined its lineage
    u32 slot, start, len; // its first block: the genome
    u32 ncpu, units, mem, gpus, npus, disk;
    u32 depth;
    u32 form, warps; // what it was born on (kFormCpu, kFormGpu); its warps (Phase 8)
    // Phase 9, actions in progress (0 = none): the Op one of its CPUs is
    // doing and the ticks left; what is acting on it and the ticks left.
    u32 doing_op, doing_left;
    u32 threat_op, threat_left;
    u32 eunits; // of its millicores, E (Phase 9c)
};

// A lane of the selected body's warps (Phase 8).
struct LaneView {
    u32 warp, lane;
    u32 ip, rh, wh, ax, bx, cx, dx, sp;
    u32 err, errs, halted, yielded;
    u32 next; // at the lowest IP among its warp's running lanes: runs in the warp's next step
    u32 wait; // its warp waits on a host instruction
    u64 steps; // its warp's steps since launch
};

struct CpuView {
    u32 ax, bx, cx, dx, ip, rh, wh, sp;
    u32 stack[kMaxStack];
    u64 executed;
    u32 err, errs, halted, yielded;
    u32 result, pending; // async accelerator slot and ops pending (Phase 5)
    // Phase 9c: the kind of core its thread prefers, and its share of a core
    // last tick, in all and on E millicores (0-1; threads or E millicores only)
    u32 kind;
    float share, eshare;
};

// A world or genebank event, parsed from its text.
enum EventKind : u32 {
    kEvReplicator,
    kEvGpuReplicator,
    kEvNpuUser,
    kEvColonist,
    kEvDiskUser,
    kEvSignaller,
    kEvGpuNative, // Phase 8
    kEvDefended,  // Phase 9
    kEvSleeper,   // docs/lifehistory-plan.md V7
    kEvCollapse,
    kEvRecovered,
    // The disasters' strikes (docs/disasters-plan.md X7: their events' first
    // words): the map marks a node struck in the last kStrikeFade ticks.
    kEvSurge,     // a power surge's epicentre (its region collapses: collapse events)
    kEvCrash,
    kEvBurnout,
    kEvDiskFail,
    kEvLightning,
    kEvOther,
    kEvCount
};
struct ViewEvent {
    u64 tick;
    u32 kind, node;
    u32 off, len; // text
};
const char* event_kind_name(u32 k);
void parse_event(Str text, u32* kind, u32* node);
// X7's map marker: a strike's kinds, colours and how long its marker shows.
inline bool event_is_strike(u32 kind) { return kind >= kEvSurge && kind <= kEvLightning; }
u32 strike_color(u32 kind);
inline constexpr u64 kStrikeFade = 3000;

// The most genebank saves a viewer keeps (the newest): at the default
// detect.genebank_threshold of 2 a 100-node world saves about 10 a tick.
inline constexpr usize kMaxSaves = 200000;

// A genebank save, as the browser lists it.
struct GenomeEntry {
    u64 tick, genotype, parent, lineage;
    u32 node, len;
    char name[16];        // "0008aab"
    char parent_name[16]; // "0008aaa", "unsaved" or "none"
    u32 off, n;           // the genome as op names, in the owner's text
    float surprise;       // the live novelty meter's (GenebankRecord::surprise; kNoSurprise: not measured)
    u32 pad;
};

// "New kinds of life" (docs/analysis-plan.md A2): one of a window's most
// surprising successful genotypes -- a new genotype in a lineage that has met
// the gate by the window's end, ranked by its surprise. Live, from the run's
// novelty meter; a finished run's from `nova analyse novelty`'s
// novelty_top.csv (whose rows carry no genotype hash: genotype 0).
struct NoveltyPick {
    u64 window_first, window_last;
    u64 tick, lineage, genotype;
    double surprise;  // bits per codon
    u32 rank;         // 1 = the most surprising
    u32 node, len;
    u32 open;         // 1: the window is still open, and this is a leader so far
    char name[16];
    u32 off, n;       // the genome as op names, in the owner's text
};
inline constexpr u64 kNoveltyListWindow = 10000; // ticks: nova analyse novelty's default window
inline constexpr u32 kNoveltyListTop = 5;        // ... and its --top

// --- the map of life (docs/analysis-plan.md A5) ------------------------------

inline constexpr u32 kLifeDims = 1024; // a genome's 3-codon runs, hashed into this many counts
inline constexpr u32 kLifeRef = 2000;  // genotypes in the reference window the axes are fitted on

// A genome (op names, space separated) as its 3-codon runs hashed into
// kLifeDims counts, scaled to unit length -- each count becomes the square root
// of its share of the runs -- so its make-up places it, not its length. Fills
// dense[kLifeDims]; false (all zero) for fewer than 3 codons.
bool life_features(Str ops, double* dense);

// One genotype on the map: its first save's details, and its place on the
// fitted axes (fixed: it never moves once the axes are final).
struct LifePoint {
    u64 genotype, lineage;
    u64 first, last; // ticks of its first and latest genebank save
    float x, y;
    u32 saves;       // genebank saves of it: how many nodes it reached the threshold on
    u32 node, len;
    char name[16];
};

// Every genotype as a point: its hashed 3-codon runs reduced to 2D by PCA
// (pca.cpp), fitted once on a reference window, so positions mean the same
// thing at every time and the map can be animated.
//   - Live (and LifeRef first): the reference window is the first kLifeRef
//     distinct genotypes. Until they are all in, the axes are fitted
//     provisionally on what there is (at 8, 16, 32, ... genotypes) and every
//     point is placed again; at kLifeRef the axes become final.
//   - A finished run (RunArchive) can fit on a sample spread over the whole
//     run instead (fit(), then add() every save).
class LifeMap {
  public:
    void clear(); // (keeps the reference size)
    // How many genotypes the first-N reference window holds (kLifeRef; the
    // tests use fewer). Before the first add().
    void set_reference(u32 n) { ref_target_ = n >= 2 ? n : 2; }
    // Every genebank save, in save order (a genotype saved again counts a save).
    void add(u64 tick, u64 genotype, u64 lineage, u32 node, u32 len, const char* name, Str ops);
    // Fits final axes on these genomes (op names; text[off[i] .. + len[i]]),
    // the reference window from tick `first` to `last`, and places every point.
    bool fit(const Array<char>& text, const Array<u32>& off, const Array<u32>& len, u64 first, u64 last);
    // Makes the axes final on what the reference window holds now (a run with
    // fewer than kLifeRef genotypes).
    void finalize();
    bool fitted() const { return fitted_; }
    bool final() const { return final_; }
    u32 reference() const { return ref_n_; } // genotypes the axes were fitted on
    u64 ref_first() const { return ref_first_; }
    u64 ref_last() const { return ref_last_; }
    // The share of the reference's variance along each axis (0-1).
    double variance_share(u32 axis) const;
    const Array<LifePoint>& points() const { return pts_; }
    usize saves() const { return saves_; }
    u64 first_tick() const { return t0_; }
    u64 last_tick() const { return t1_; }
    // Where a genome would sit on the axes (0, 0 unfitted or too short).
    void place(Str ops, float* x, float* y) const;
    // The box positions are drawn in (x0, y0, x1, y1): the reference points'
    // spread, fixed with the axes.
    const float* box() const { return box_; }
    const Array<double>& mean() const { return mean_; }
    const Matrix& axes() const { return comps_; }
    // The point for a genotype, or kNone.
    u32 find(u64 genotype) const;
    // Forgets the genotypes not saved since `tick`, keeping the axes and every
    // other position: a long live session keeps only the newest saves
    // (Viewer::trim_logs), and the map follows them rather than growing for
    // ever. Only once the axes are final (until then the held reference
    // genomes follow the points).
    void drop_before(u64 tick);

  private:
    bool fit_on(const Array<char>& text, const Array<u32>& off, const Array<u32>& len);
    void place_all();

    Array<LifePoint> pts_;
    HashMap<u32> index_; // genotype -> point
    usize saves_ = 0;
    u64 t0_ = 0, t1_ = 0;
    bool fitted_ = false, final_ = false;
    u32 ref_n_ = 0, next_fit_ = 8, ref_target_ = kLifeRef;
    u64 ref_first_ = 0, ref_last_ = 0;
    Array<double> mean_, var_;
    Matrix comps_;
    double mu_dot_[2] = {}; // mean . each axis
    double total_var_ = 0;
    float box_[4] = {-1, -1, 1, 1};
    // The reference window's genomes, held until the axes are final; and
    // every point's genome while they are provisional (to place it again).
    Array<char> held_text_;
    Array<u32> held_off_, held_len_;
};

// What the viewer wants built in detail.
struct ViewRequest {
    u32 node = kNone;     // the inspector's node
    u32 body = kNone;     // its body slot ...
    u64 serial = 0;       // ... and that body's serial (a reused slot is another body)
    u32 strip = kStripOwner;
    u64 genotype = 0;     // count living copies of this genotype on every node (0 = none)
    bool hash = false;    // compute the world hash (it reads everything: on pause)
    bool populations = false; // A5: living members of every genotype (the map of life sizes points by them)
    // Each node's dead-matter share (NodeView::material, the map coloured by
    // material): a scan of every node's soup, so the viewer asks only while
    // it shows it; without, the field is 0 and not marked known.
    bool material = true;
};

// The memory tiers' boundary (docs/world-plan.md W3) for the inspector's
// strip: codons [0, fast_end) of the node's soup are fast memory, 0 = no fast
// region. The accessor uses the node's page-aligned memory-tier boundary.
u32 view_fast_end(const Soup& s);

inline constexpr u32 kMaxKnobs = 8;

struct ViewFrame {
    // the run
    u64 tick = 0, instructions = 0, hash = 0;
    bool hashed = false, procedural = false, archive = false;
    bool kinds = false; // Phase 9c: threads or E millicores (the inspector shows kinds and shares)
    u32 nodes = 0;
    u64 detect_tick = 0; // first replicator's tick + 1, 0 = none yet
    // A landscape (Phase 7), in the first frame only (the viewer keeps it):
    // the land as an image over box (x0, y0, x1, y1 in map coordinates), and
    // each landmass with nodes.
    bool landscape = false, land_sent = false;
    u32 land_w = 0, land_h = 0;
    Array<u32> land;
    float box[4] = {0, 0, 1, 1};
    Array<MassView> masses;
    Array<NodeView> node;
    Array<EdgeView> edge;
    Array<CargoView> cargo;
    u32 knobs = 0;
    char knob_key[kMaxKnobs][24] = {};
    double knob_value[kMaxKnobs] = {};
    // the selected node
    u32 sel_node = kNone;
    u32 strip_w = 0, strip_h = 0; // the strip: one pixel per codon, row by row
    u32 fast_end = 0;             // its fast memory: codons [0, fast_end) (W3; view_fast_end; 0 = none)
    Array<u32> strip;
    Array<u32> strip_owner; // per codon: the owning body's slot, or kNone (waste)
    Array<BodyRow> bodies;
    // the selected body (req_*: the body the request asked for, found or not)
    u32 sel_body = kNone;
    u32 req_body = kNone;
    u64 req_serial = 0;
    bool body_ok = false;
    BodyRow body = {};
    Array<char> genome; // op names
    Array<CpuView> cpus;
    Array<LaneView> lanes; // its warps' lanes (Phase 8), warp after warp
    // new since the previous frame
    Array<u64> sample_tick;
    Array<double> sample; // rows of kStatsColumns, per tick (see chart_row)
    Array<ViewEvent> events;
    Array<char> event_text;
    Array<GenomeEntry> saves;
    Array<char> save_text;
    // "New kinds of life" (A2): novelty is true when there is a list at all (a
    // live meter, or an archive's novelty_top.csv); picks are the windows
    // closed since the previous frame; picks_open, the open window's leaders
    // so far, replace the previous frame's (live only).
    bool novelty = false;
    u64 novelty_window = 0;
    Array<NoveltyPick> picks, picks_open;
    Array<char> pick_text;
    // A5: living members per genotype, when the request asked (pop_sent);
    // genotypes in increasing order.
    bool pop_sent = false;
    Array<u64> pop_genotype;
    Array<u32> pop_count;
    // A5: a finished run's map of life, in its first frame (life_sent).
    bool life_sent = false;
    LifeMap life;
    // A5: behaviour profiles' roles by genotype (A3's live rules, kRole...;
    // the genotype's latest sample), new since the previous frame (a finished
    // run's: all of them, in its first frame), for the map's colour by role.
    Array<u64> role_genotype;
    Array<u8> role_of;

    void clear_new();
};

// Columns summed over a stats window (the tallies and the link counts); the
// charts show them per tick. The rest are taken at the window's end.
bool stats_column_is_sum(u32 i);
// A stats row as the charts plot it: sums divided by the window's ticks.
void chart_row(const StatsRow& r, double* out);

// Every chart sample so far, at most kChartPoints of them: when full, every
// other one is dropped, so the whole history stays at a coarser step.
class ChartLog {
  public:
    static constexpr usize kChartPoints = 4096;
    void clear();
    void add(u64 tick, const double* row); // kStatsColumns values
    usize size() const { return ticks_.size(); }
    u64 tick(usize i) const { return ticks_[i]; }
    double value(usize i, u32 col) const { return vals_[i * kStatsColumns + col]; }
    u32 step() const { return step_; } // samples merged per point (1, 2, 4, ...)

  private:
    Array<u64> ticks_;
    Array<double> vals_;
    u32 step_ = 1, skip_ = 0;
};

// The phylogeny of one lineage (plan C8): its saved genotypes as a tree by
// parent link, time across (x = first tick) and one row per genotype in
// depth-first order, children after their parent. A genotype saved on several
// nodes appears once (its first save). Entries whose parent was not saved hang
// from the lineage's root row.
struct PhyloNode {
    u32 entry;  // index into the entries given
    u32 parent; // index into the layout, or kNone
    u64 tick;
    u32 row, depth;
};
void phylo_layout(const GenomeEntry* e, usize n, u64 lineage, Array<PhyloNode>* out);

// Map positions: hidden coordinates in a procedural world, a landscape's
// scaled into the unit box keeping its shape, a circle otherwise.
void map_positions(const World& w, Array<float>* x, Array<float>* y);
// A landscape's land image (water and each landmass's land), its box in map
// coordinates and its landmasses (Phase 7).
void land_view(const World& w, ViewFrame* f);

class ViewSession {
  public:
    // Starts watching; chart samples every `sample_every` ticks (0 = none).
    void attach(Run* run, u32 sample_every);
    // "New kinds of life" (A2): the list's window and how many per window
    // (kNoveltyListWindow, kNoveltyListTop by default). Before stepping.
    void set_novelty_list(u64 window, u32 top);
    // After every Run::step().
    void after_step();
    // Fills f with the state now and everything new since the previous build.
    void build(const ViewRequest& q, ViewFrame* f);
    // Takes the partial chart window now (used at a pause or the end).
    void flush_sample();

    // One node's NodeView (build's node pass, run on the run's thread pool
    // between ticks: it only reads the world, and writes f->node[n] alone).
    void node_view(u32 n, const ViewRequest& q, float inv, ViewFrame* f) const;

  private:
    void sample();
    void take_saves();                        // the run's new genebank saves into the open window (A2)
    void close_window();                      // its top picks, gated by now
    void top_picks(bool open, Array<NoveltyPick>* out, Array<char>* text); // the open window's best, gated by now

    Run* run_ = nullptr;
    u32 every_ = 0;
    SoupTally sum_ = {};
    u64 first_ = 0;
    u32 ticks_ = 0;
    StatsMarks marks_;
    Array<float> acc_; // per node: births, deaths, sent, arrived, emits, symptom ticks, struck
    u32 acc_ticks_ = 0;
    Array<u64> new_tick_;
    Array<double> new_rows_;
    usize events_sent_ = 0;
    Array<float> px_, py_;
    bool hash_valid_ = false;
    u64 hash_tick_ = 0, hash_ = 0;
    bool land_sent_ = false;
    // A2's list: the open window's new genotypes (candidates, their codons),
    // and the picks of windows closed since the last frame.
    u64 nov_window_ = kNoveltyListWindow;
    u32 nov_top_ = kNoveltyListTop;
    usize bank_seen_ = 0;
    u64 open_first_ = ~0ull;
    Array<NoveltyPick> cand_;
    Array<u8> cand_codons_;
    Array<NoveltyPick> closed_;
    Array<char> closed_text_;
    // ... the open window's leaders as last scored, at this tick (~0: afresh)
    Array<NoveltyPick> open_;
    Array<char> open_text_;
    u64 open_tick_ = ~0ull;
    // A5: finished profile samples' roles since the last frame
    Array<u64> role_genotype_;
    Array<u8> role_of_;
    // A5: living members per genotype, last counted at this tick
    u64 pop_tick_ = ~0ull;
};

// A finished run, from its files (plan C9). The directory holds what
// output.* wrote: stats.csv, nodes.csv, replay.log, genebank.txt (any of
// them; at least one).
class RunArchive {
  public:
    // Before load(): the map of life (A5) and "new kinds of life" (A2). Each
    // is built when asked (1), never (0), or by default (-1) when genebank.txt
    // is at most kArchiveAutoBytes. The map's reference window: kLifeRefAll, a
    // sample of kLifeRef genotypes spread over the whole run (by the lowest
    // hashes: no random draw); kLifeRefFirst, the first kLifeRef (as live);
    // kLifeRefWindow, a sample of those first saved in ticks [a, b].
    enum : u32 { kLifeRefAll, kLifeRefFirst, kLifeRefWindow };
    static constexpr u64 kArchiveAutoBytes = 256ull << 20;
    void set_life(int build, u32 ref = kLifeRefAll, u64 a = 0, u64 b = 0) {
        want_life_ = build;
        life_ref_ = ref;
        life_a_ = a;
        life_b_ = b;
    }
    void set_novelty(int build) { want_novelty_ = build; }

    bool load(const char* dir, TextBuf* err);
    // nodes.csv windows (0 without it), and the last tick of window i.
    u32 windows() const { return static_cast<u32>(win_tick_.size()); }
    u64 window_tick(u32 i) const { return win_tick_[i]; }
    // Window `window`'s map; the first build also hands over every chart
    // sample, event and genebank save, the novelty picks and the map of life.
    void build(u32 window, const ViewRequest& q, ViewFrame* f);
    Str config_text() const { return config_.view(); }
    // What load() did about the map of life and the novelty list, a line each.
    Str notes() const { return notes_.view(); }
    bool has_novelty() const { return novelty_; }
    usize pick_count() const { return picks_.size(); }
    const LifeMap& life() const { return life_; } // (handed to the first frame: empty after)

  private:
    bool load_stats(Str text, TextBuf* err);
    bool load_nodes(Str text, TextBuf* err);
    bool load_genebank(Str text, TextBuf* err);
    bool load_novelty(const char* dir, TextBuf* err);
    bool load_life(const char* dir, TextBuf* err);

    TextBuf config_;
    u32 nodes_ = 0;
    bool procedural_ = false, sent_ = false;
    Array<u64> win_tick_;   // per window
    Array<u32> win_ticks_;  // ticks in it
    Array<double> win_vals_; // windows x nodes x kNodeCols
    Array<EdgeView> edges_;
    Array<float> px_, py_; // from the config's world, when replay.log is there
    // A landscape's land and bridges (Phase 7)
    bool landscape_ = false;
    u32 land_w_ = 0, land_h_ = 0, open_every_ = 0, open_ticks_ = 0;
    Array<u32> land_, edge_phase_, node_mass_;
    float box_[4] = {0, 0, 1, 1};
    Array<MassView> masses_;
    Array<u64> sample_tick_;
    Array<double> samples_;
    Array<ViewEvent> events_;
    Array<char> event_text_;
    Array<GenomeEntry> saves_;
    Array<char> save_text_;
    // nodes.csv's later columns, by name (W8, X7): which the file has
    bool has_sub_ = false, has_occ_ = false, has_dis_ = false;
    double sub_floor_ = 0.1; // the run's substrate.floor, for the levels
    // A2 and A5
    int want_life_ = -1, want_novelty_ = -1;
    u32 life_ref_ = kLifeRefAll;
    u64 life_a_ = 0, life_b_ = 0;
    bool novelty_ = false;
    u64 pick_window_ = 0;
    Array<NoveltyPick> picks_;
    Array<char> pick_text_;
    LifeMap life_;
    // A5: profiles.csv's roles by genotype (each genotype's latest sample)
    Array<u64> role_genotype_;
    Array<u8> role_of_;
    TextBuf notes_;
};

} // namespace nova
