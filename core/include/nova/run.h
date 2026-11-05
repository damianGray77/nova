#pragma once

// Run: one simulation run -- config, world, thread pool, scheduled knob changes
// and the optional outputs (CSV stats, replay log). The `nova` app and the
// tests drive the world through this one path.
//
// Knob timing: a knob scheduled for tick T is applied at the barrier *before*
// tick T runs. It also updates the run's config, so a snapshot always carries
// the effective settings, and it is written to the replay log with its tick.
#include "nova/array.h"
#include "nova/config.h"
#include "nova/io.h"
#include "nova/pool.h"
#include "nova/replay.h"
#include "nova/stats.h"
#include "nova/world.h"

namespace nova {

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
    bool init_from_snapshot(const u8* data, usize n, u32 threads, TextBuf* err);

    // Test hook (see ThreadPool::set_dispatch_order).
    void set_dispatch_order(const u32* order) { pool_.set_dispatch_order(order); }

    // Schedules a live knob change for `tick` (>= the current tick). Several
    // changes at one tick apply in the order they were scheduled.
    bool schedule_knob(u64 tick, Str key, Str value, TextBuf* err);

    void step();
    void run_until(u64 tick);

    bool open_stats(const char* path, u32 window, TextBuf* err);
    // Phase 0 replay logs start at tick 0 (resume-then-log comes later).
    bool open_replay(const char* path, u64 checkpoint_every, TextBuf* err);
    // Writes the final partial stats row and the replay 'end' record; closes both.
    void finish();

    void snapshot(ByteWriter& out) const;

    World& world() { return world_; }
    const World& world() const { return world_; }
    u32 threads() const { return pool_.threads(); }

  private:
    struct Knob {
        u64 tick;
        u32 key_off, key_len, val_off, val_len;
    };

    void apply_due_knobs();

    Config config_;
    World world_;
    ThreadPool pool_;
    Array<Knob> knobs_;
    Array<char> knob_text_;
    usize next_knob_ = 0;
    StatsCsv stats_;
    ReplayWriter replay_;
    u64 checkpoint_every_ = 0;
};

struct ReplayResult {
    bool ok = false;
    usize checkpoints_checked = 0;
    u64 diverged_at = 0; // tick of the first mismatching checkpoint (when !ok)
    u64 expected = 0;
    u64 actual = 0;
    char msg[192] = {};
};

// Re-runs a logged run from its config and knob events at `threads` threads,
// comparing every checkpoint and the end record.
bool replay_verify(const ReplayLog& log, u32 threads, ReplayResult* out);

} // namespace nova
