#pragma once

// Run: one simulation run -- config, world, thread pool, scheduled knob changes
// and the optional outputs (CSV stats, replay log, genebank, lineage log). The
// `nova` app and the tests drive the world through this one path.
//
// Knob timing: a knob scheduled for tick T is applied at the barrier *before*
// tick T runs. It also updates the run's config, so a snapshot always carries
// the effective settings, and it is written to the replay log with its tick.
//
// Detection (Phase 1b): after every tick the run drains each node's replicator
// detector in node order. A gate event becomes an `event` record in the replay
// log (so `nova replay` re-checks it) and the first one is kept as the run's
// detection; genebank saves go to the genebank file and extinct lineages to the
// lineage log.
#include "nova/array.h"
#include "nova/config.h"
#include "nova/io.h"
#include "nova/pool.h"
#include "nova/replay.h"
#include "nova/stats.h"
#include "nova/world.h"

namespace nova {

// The first replicator a run detected.
struct Detection {
    bool found = false;
    u64 tick = 0;         // the tick during which the gate was met
    u64 instructions = 0; // instructions executed up to the end of that tick
    u64 lineage = 0;      // world-unique id (lineage.h), written n.k
    u32 node = 0, depth = 0, alive = 0, len = 0;
    u32 via = 0; // kViaDivide or kViaInject: the kind of link that met the gate
    u64 genotype = 0;
};

// A genebank save kept in memory (Phase 6, for the viewer's genome browser and
// phylogeny): what genebank.txt holds, with the genome as codons.
struct GenebankRecord {
    u64 tick;
    u64 genotype;
    u64 parent;      // the parent's genotype, 0 = a lineage founder
    u64 lineage;
    u32 node;
    u32 len;
    u32 name;        // Genebank name index among genotypes of its length
    u32 parent_name; // Genebank::kNoName if the parent is unsaved (or none)
    u32 parent_len;
    u32 codon_off;   // the genome, in Run::genebank_codons()
};

class Run {
  public:
    Run() = default;
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    // Fill (parse) the config before init().
    Config& config() { return config_; }
    const Config& config() const { return config_; }

    // threads: total threads, 0 = all hardware threads.
    bool init(u32 threads, TextBuf* err);
    // Resumes from snapshot bytes: config and world both come from the snapshot.
    // `edit`, if given, may change the snapshot's config before the world is
    // made (nova resume --set: warp.device, warp.verify and the like); false
    // from it stops the resume.
    bool init_from_snapshot(const u8* data, usize n, u32 threads, TextBuf* err,
                            bool (*edit)(Config&, void*) = nullptr, void* ctx = nullptr);
    // What the budgets (budget.h) resolved to, a line each ("" if none).
    Str budget_notes() const { return budget_notes_.view(); }

    // Test hook (see ThreadPool::set_dispatch_order).
    // order: a permutation of the world's nodes.
    void set_dispatch_order(const u32* order) { pool_.set_dispatch_order(order, world_.node_count()); }

    // Schedules a live knob change for `tick` (>= the current tick). Several
    // changes at one tick apply in the order they were scheduled. Refused once
    // the knobs are locked.
    bool schedule_knob(u64 tick, Str key, Str value, TextBuf* err);
    // The knob lock (docs/config-plan.md C3, Damian): a run's settings are
    // fixed from its start. The front ends (nova run, nova view) lock their
    // run as soon as it is made, so nothing can change a setting mid-run;
    // changes scheduled before the lock -- a replay log's knob records, which
    // replay_verify schedules up front -- still apply at their ticks.
    void lock_knobs() { knobs_locked_ = true; }
    bool knobs_locked() const { return knobs_locked_; }

    void step();
    void run_until(u64 tick);

    bool open_stats(const char* path, u32 window, TextBuf* err);
    // The per-node file (Phase 3), on the stats window: after open_stats;
    // rows every `every` windows (stats.nodes_every, Phase 7).
    bool open_nodes(const char* path, TextBuf* err, u32 every = 1);
    // The signal tap (Phase 4), on the stats window: after open_stats.
    bool open_signals(const char* path, TextBuf* err);
    // Replay logs start at tick 0 (resume-then-log comes later).
    bool open_replay(const char* path, u64 checkpoint_every, TextBuf* err);
    // genebank.txt (one block per saved genotype) and the lineage CSV.
    bool open_genebank(const char* path, TextBuf* err);
    bool open_lineages(const char* path, TextBuf* err);
    // Numbered snapshots every `every` ticks (output.snapshot_every, Phase 6):
    // `<path>.<tick>` at each multiple of it.
    void open_snapshots(const char* path, u64 every);
    // The first numbered snapshot that could not be written ("" if none).
    Str snapshot_error() const { return snap_error_.view(); }
    // Keeps every genebank save in memory too (the viewer asks for this, and
    // empties the log once it has taken the saves: a long run saves millions).
    void keep_genebank(bool on) { keep_bank_ = on; }
    void clear_genebank_log() {
        bank_log_.clear();
        bank_codons_.clear();
    }
    usize genebank_count() const { return bank_log_.size(); }
    const GenebankRecord& genebank_record(usize i) const { return bank_log_[i]; }
    const u8* genebank_codons() const { return bank_codons_.data(); }
    // Writes the final partial stats row, the living lineages and the replay
    // 'end' record; closes every output.
    void finish();

    void snapshot(ByteWriter& out) const;

    World& world() { return world_; }
    const World& world() const { return world_; }
    u32 threads() const { return pool_.threads(); }

    // Instructions executed so far (soup model).
    u64 instructions() const { return instructions_; }
    const Detection& detection() const { return detection_; }
    // The first gated lineage to copy with the GPU (Phase 2), and the first
    // whose member used the NPU (Phase 2b).
    const Detection& gpu_detection() const { return gpu_detection_; }
    const Detection& npu_detection() const { return npu_detection_; }
    // The first colonist: a gated lineage arriving on a node it wasn't founded
    // on (Phase 3); `node` is where it arrived.
    const Detection& colonist() const { return colonist_; }
    // The first gated lineage whose member restored its own code from disk
    // (Phase 4).
    const Detection& disk_user() const { return disk_user_; }
    // The first gated lineage whose members emitted on signal.sustained ticks.
    const Detection& signaller() const { return signaller_; }
    // The first gated lineage whose GPU-born member made an inheriting
    // daughter (Phase 8): replication on the GPU alone.
    const Detection& gpu_native() const { return gpu_native_; }
    const Detection& defended() const { return defended_; } // Phase 9
    // The first detection's genome as op names ("" if none).
    Str detection_genome() const { return detection_genome_.view(); }

    // Every observer event so far, as written to the replay log, numbered
    // from 0. Those before event_base() were dropped (Phase 7: a long run
    // keeps only what hasn't been handed on, see drop_events).
    usize event_count() const { return events_base_ + events_.size(); }
    usize event_base() const { return events_base_; }
    u64 event_tick(usize i) const { return events_[i - events_base_].tick; }
    Str event_text(usize i) const;
    // Forgets the events held so far (they keep their numbers).
    void drop_events() {
        events_base_ += events_.size();
        events_.clear();
        event_text_.clear();
    }

  private:
    struct Knob {
        u64 tick;
        u32 key_off, key_len, val_off, val_len;
    };
    struct Event {
        u64 tick;
        u32 off, len;
    };

    void apply_due_knobs();
    void after_step(); // events, detection, stats, checkpoints, numbered snapshots
    void drain_detection();
    void write_lineage_row(const LineageEnd& e, u32 node, bool alive);

    Config config_;
    World world_;
    ThreadPool pool_;
    Array<Knob> knobs_;
    Array<char> knob_text_;
    usize next_knob_ = 0;
    bool knobs_locked_ = false; // lock_knobs(): settings fixed for the whole run
    StatsCsv stats_;
    ReplayWriter replay_;
    u64 checkpoint_every_ = 0;
    TextFile genebank_;
    TextFile lineages_;
    TextBuf snap_path_, snap_error_, budget_notes_;
    u64 snap_every_ = 0;
    bool keep_bank_ = false;
    Array<GenebankRecord> bank_log_;
    Array<u8> bank_codons_;

    u64 instructions_ = 0;
    Detection detection_;
    Detection gpu_detection_;
    Detection npu_detection_;
    Detection colonist_;
    Detection disk_user_;
    Detection signaller_;
    Detection gpu_native_;
    Detection defended_;
    TextBuf detection_genome_;
    Array<Event> events_;
    Array<char> event_text_;
    usize events_base_ = 0;
};

struct ReplayResult {
    bool ok = false;
    usize checkpoints_checked = 0;
    usize events_checked = 0;
    u64 diverged_at = 0; // tick of the first mismatching checkpoint (when !ok)
    u64 expected = 0;
    u64 actual = 0;
    char msg[256] = {};
};

// Re-runs a logged run from its config and knob events at `threads` threads,
// comparing every checkpoint, the end record and every logged event.
bool replay_verify(const ReplayLog& log, u32 threads, ReplayResult* out);

// Writes a genome as space-separated op names of table t.
void genome_names(const CodonTable& t, const u8* codons, u32 len, TextBuf& out);

} // namespace nova
