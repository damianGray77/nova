#pragma once

// The replay log (design.md 7.1 H and K): everything needed to re-run a run
// exactly, plus checkpoints to prove that it did.
//
// Text format v1, one record per line, in time order:
//   nova-replay 1
//   config <key> = <value>        the config at tick 0 (one line per entry)
//   hash <tick> <16 hex digits>   world hash once <tick> ticks have completed
//   knob <tick> <key> = <value>   a live knob change, applied before tick <tick> ran
//   end <tick> <16 hex digits>    final tick and hash
// Lines starting with '#' are comments.
//
// Knob events are what keep a run reproducible once the analysis UI lets people
// turn knobs mid-run: the change is logged with its tick, and replay reapplies
// it at the same tick.
#include "nova/array.h"
#include "nova/config.h"
#include "nova/io.h"
#include "nova/str.h"

namespace nova {

inline constexpr u32 kReplayVersion = 1;

class ReplayWriter {
  public:
    bool open(const char* path, const Config& initial);
    bool is_open() const { return file_.is_open(); }
    void knob(u64 tick, Str key, Str value);
    void checkpoint(u64 tick, u64 hash);
    void end(u64 tick, u64 hash);
    void close() { file_.close(); }

  private:
    void tick_hash(const char* kind, u64 tick, u64 hash);

    TextFile file_;
};

struct ReplayCheckpoint {
    u64 tick;
    u64 hash;
};

class ReplayLog {
  public:
    bool parse(Str text, TextBuf* err);
    bool load_file(const char* path, TextBuf* err);

    Str config_text() const { return config_.view(); }

    usize knob_count() const { return knobs_.size(); }
    u64 knob_tick(usize i) const { return knobs_[i].tick; }
    Str knob_key(usize i) const;
    Str knob_value(usize i) const;

    const Array<ReplayCheckpoint>& checkpoints() const { return checkpoints_; }
    bool has_end() const { return has_end_; }
    ReplayCheckpoint end() const { return end_; }

  private:
    struct Knob {
        u64 tick;
        u32 key_off, key_len, val_off, val_len;
    };

    TextBuf config_;
    Array<char> arena_; // knob keys and values
    Array<Knob> knobs_;
    Array<ReplayCheckpoint> checkpoints_;
    bool has_end_ = false;
    ReplayCheckpoint end_ = {};
};

} // namespace nova
