#pragma once

// The replay log (design.md 7.1 H and K): everything needed to re-run a run
// exactly, plus checkpoints to prove that it did.
//
// Text format v2, one record per line, in time order:
//   nova-replay 2
//   config <key> = <value>        the config at tick 0 (one line per entry)
//   hash <tick> <16 hex digits>   world hash once <tick> ticks have completed
//   knob <tick> <key> = <value>   a live knob change, applied before tick <tick> ran
//   event <tick> <text>           something the observers saw during tick <tick>,
//                                 e.g. the replicator gate (Phase 1b); replay
//                                 must see exactly the same events
//   end <tick> <16 hex digits>    final tick and hash
// Lines starting with '#' are comments. Version 1 logs (no events) still load.
//
// Knob events are what keep a run reproducible once the analysis UI lets people
// turn knobs mid-run: the change is logged with its tick, and replay reapplies
// it at the same tick. A run stopped by the clock or by Esc stays exact the
// same way: its `end` record says which tick it stopped at.
#include "nova/array.h"
#include "nova/config.h"
#include "nova/io.h"
#include "nova/str.h"

namespace nova {

inline constexpr u32 kReplayVersion = 2;

class ReplayWriter {
  public:
    bool open(const char* path, const Config& initial);
    bool is_open() const { return file_.is_open(); }
    void knob(u64 tick, Str key, Str value);
    void event(u64 tick, Str text); // text: one line, no '\n'
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

    u32 version() const { return version_; }

    usize knob_count() const { return knobs_.size(); }
    u64 knob_tick(usize i) const { return knobs_[i].tick; }
    Str knob_key(usize i) const;
    Str knob_value(usize i) const;

    usize event_count() const { return events_.size(); }
    u64 event_tick(usize i) const { return events_[i].tick; }
    Str event_text(usize i) const;

    const Array<ReplayCheckpoint>& checkpoints() const { return checkpoints_; }
    bool has_end() const { return has_end_; }
    ReplayCheckpoint end() const { return end_; }

  private:
    struct Knob {
        u64 tick;
        u32 key_off, key_len, val_off, val_len;
    };

    struct Event {
        u64 tick;
        u32 off, len;
    };

    TextBuf config_;
    Array<char> arena_; // knob keys and values, event texts
    Array<Knob> knobs_;
    Array<Event> events_;
    u32 version_ = 0;
    Array<ReplayCheckpoint> checkpoints_;
    bool has_end_ = false;
    ReplayCheckpoint end_ = {};
};

} // namespace nova
