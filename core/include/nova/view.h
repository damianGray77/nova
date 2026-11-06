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
#include "nova/array.h"
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
    kColorCount
};
const char* color_by_name(u32 c);

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
};

// A landmass with nodes on it (Phase 7): where to label it, and how many.
struct MassView {
    float x, y; // the centroid of its nodes, in map coordinates
    u32 id, nodes;
};

// The number the map colours a node by, and whether it has a fixed range
// (health 0-100, fractions 0-1); otherwise the viewer scales over the nodes.
double node_metric(const NodeView& n, u32 color_by);
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
    kEvCollapse,
    kEvRecovered,
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
};

// What the viewer wants built in detail.
struct ViewRequest {
    u32 node = kNone;     // the inspector's node
    u32 body = kNone;     // its body slot ...
    u64 serial = 0;       // ... and that body's serial (a reused slot is another body)
    u32 strip = kStripOwner;
    u64 genotype = 0;     // count living copies of this genotype on every node (0 = none)
    bool hash = false;    // compute the world hash (it reads everything: on pause)
};

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
    // After every Run::step().
    void after_step();
    // Fills f with the state now and everything new since the previous build.
    void build(const ViewRequest& q, ViewFrame* f);
    // Takes the partial chart window now (used at a pause or the end).
    void flush_sample();

  private:
    void sample();

    Run* run_ = nullptr;
    u32 every_ = 0;
    SoupTally sum_ = {};
    u64 first_ = 0;
    u32 ticks_ = 0;
    StatsMarks marks_;
    Array<float> acc_; // per node: births, deaths, sent, arrived, emits
    u32 acc_ticks_ = 0;
    Array<u64> new_tick_;
    Array<double> new_rows_;
    usize events_sent_ = 0;
    Array<float> px_, py_;
    bool hash_valid_ = false;
    u64 hash_tick_ = 0, hash_ = 0;
    bool land_sent_ = false;
};

// A finished run, from its files (plan C9). The directory holds what
// output.* wrote: stats.csv, nodes.csv, replay.log, genebank.txt (any of
// them; at least one).
class RunArchive {
  public:
    bool load(const char* dir, TextBuf* err);
    // nodes.csv windows (0 without it), and the last tick of window i.
    u32 windows() const { return static_cast<u32>(win_tick_.size()); }
    u64 window_tick(u32 i) const { return win_tick_[i]; }
    // Window `window`'s map; the first build also hands over every chart
    // sample, event and genebank save.
    void build(u32 window, const ViewRequest& q, ViewFrame* f);
    Str config_text() const { return config_.view(); }

  private:
    bool load_stats(Str text, TextBuf* err);
    bool load_nodes(Str text, TextBuf* err);
    bool load_genebank(Str text, TextBuf* err);

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
};

} // namespace nova
