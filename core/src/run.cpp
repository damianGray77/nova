#include "nova/run.h"

#include <cstdio> // snprintf

#include "nova/snapshot.h"

namespace nova {

bool Run::init(u32 threads, TextBuf* err) {
    if (!world_.init(config_)) {
        err->append(config_.value_error());
        return false;
    }
    pool_.start(threads);
    return true;
}

bool Run::init_from_snapshot(const u8* data, usize n, u32 threads, TextBuf* err) {
    SnapshotHeader h;
    if (!snapshot_parse(data, n, &h, err))
        return false;
    Config::ParseError pe;
    if (!config_.parse(h.config_text, &pe)) {
        err->append("snapshot: embedded config: ");
        err->append(pe.msg);
        return false;
    }
    if (!world_.init(config_)) {
        err->append("snapshot: embedded config: ");
        err->append(config_.value_error());
        return false;
    }
    ByteReader r(data + h.world_offset, h.world_size);
    if (!world_.load_state(r, err))
        return false;
    if (r.remaining() != 0) {
        err->append("snapshot: trailing bytes after the world section");
        return false;
    }
    if (world_.tick() != h.tick || world_.seed() != h.seed) {
        err->append("snapshot: header does not match the world section");
        return false;
    }
    pool_.start(threads);
    return true;
}

bool Run::schedule_knob(u64 tick, Str key, Str value, TextBuf* err) {
    if (tick < world_.tick()) {
        err->append("knob ");
        err->append(key);
        err->append(": tick ");
        err->append_u64(tick);
        err->append(" is in the past (world is at tick ");
        err->append_u64(world_.tick());
        err->append_char(')');
        return false;
    }
    KnobResult r = world_.check_knob(key, value);
    if (r != KnobResult::ok) {
        err->append("knob ");
        err->append(key);
        err->append(": ");
        err->append(describe(r));
        return false;
    }
    Knob k;
    k.tick = tick;
    k.key_off = static_cast<u32>(knob_text_.size());
    k.key_len = static_cast<u32>(key.n);
    knob_text_.append(key.p, key.n);
    k.val_off = static_cast<u32>(knob_text_.size());
    k.val_len = static_cast<u32>(value.n);
    knob_text_.append(value.p, value.n);
    // Stable insert: after every knob already scheduled at or before this tick.
    usize at = knobs_.size();
    while (at > next_knob_ && knobs_[at - 1].tick > tick)
        --at;
    knobs_.insert(at, k);
    return true;
}

void Run::apply_due_knobs() {
    while (next_knob_ < knobs_.size() && knobs_[next_knob_].tick == world_.tick()) {
        const Knob& k = knobs_[next_knob_];
        Str key(knob_text_.data() + k.key_off, k.key_len);
        Str value(knob_text_.data() + k.val_off, k.val_len);
        KnobResult r = world_.set_knob(key, value);
        NOVA_ASSERT(r == KnobResult::ok); // validated when scheduled
        (void)r;
        config_.set(key, value);
        if (replay_.is_open())
            replay_.knob(k.tick, key, value);
        ++next_knob_;
    }
}

void Run::step() {
    apply_due_knobs();
    world_.step(pool_);
    stats_.record(world_);
    if (replay_.is_open() && checkpoint_every_ && world_.tick() % checkpoint_every_ == 0)
        replay_.checkpoint(world_.tick(), world_.hash());
}

void Run::run_until(u64 tick) {
    while (world_.tick() < tick)
        step();
}

bool Run::open_stats(const char* path, u32 window, TextBuf* err) {
    if (!stats_.open(path, window)) {
        err->append("cannot open stats file '");
        err->append(path);
        err->append("' (or stats window is 0)");
        return false;
    }
    return true;
}

bool Run::open_replay(const char* path, u64 checkpoint_every, TextBuf* err) {
    if (world_.tick() != 0) {
        err->append("replay logs must start at tick 0");
        return false;
    }
    if (!replay_.open(path, config_)) {
        err->append("cannot open replay log '");
        err->append(path);
        err->append_char('\'');
        return false;
    }
    checkpoint_every_ = checkpoint_every;
    replay_.checkpoint(0, world_.hash());
    return true;
}

void Run::finish() {
    stats_.finish(world_);
    if (replay_.is_open()) {
        replay_.end(world_.tick(), world_.hash());
        replay_.close();
    }
}

void Run::snapshot(ByteWriter& out) const {
    snapshot_write(world_, config_, out);
}

bool replay_verify(const ReplayLog& log, u32 threads, ReplayResult* out) {
    *out = ReplayResult{};
    Run run;
    Config::ParseError pe;
    if (!run.config().parse(log.config_text(), &pe)) {
        snprintf(out->msg, sizeof out->msg, "logged config: %s", pe.msg);
        return false;
    }
    TextBuf err;
    if (!run.init(threads, &err)) {
        snprintf(out->msg, sizeof out->msg, "%s", err.c_str());
        return false;
    }
    for (usize i = 0; i < log.knob_count(); ++i) {
        if (!run.schedule_knob(log.knob_tick(i), log.knob_key(i), log.knob_value(i), &err)) {
            snprintf(out->msg, sizeof out->msg, "%s", err.c_str());
            return false;
        }
    }

    auto check = [&](ReplayCheckpoint cp) {
        run.run_until(cp.tick);
        u64 h = run.world().hash();
        ++out->checkpoints_checked;
        if (h == cp.hash)
            return true;
        out->diverged_at = cp.tick;
        out->expected = cp.hash;
        out->actual = h;
        snprintf(out->msg, sizeof out->msg,
                 "diverged at tick %llu: logged %016llx, replayed %016llx",
                 static_cast<unsigned long long>(cp.tick), static_cast<unsigned long long>(cp.hash),
                 static_cast<unsigned long long>(h));
        return false;
    };

    for (usize i = 0; i < log.checkpoints().size(); ++i)
        if (!check(log.checkpoints()[i]))
            return false;
    if (log.has_end() && !check(log.end()))
        return false;
    out->ok = true;
    return true;
}

} // namespace nova
