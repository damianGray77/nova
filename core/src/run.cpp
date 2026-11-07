#include "nova/run.h"

#include <cstdio> // snprintf

#include "nova/budget.h"
#include "nova/keys.h" // the settings registry (pre-v4 snapshots' defaults)
#include "nova/snapshot.h"

namespace nova {

void genome_names(const CodonTable& t, const u8* codons, u32 len, TextBuf& out) {
    for (u32 i = 0; i < len; ++i) {
        if (i)
            out.append_char(' ');
        out.append(op_name(t.op[codons[i] & 63]));
    }
}

Str Run::event_text(usize i) const {
    const Event& e = events_[i - events_base_];
    return Str(event_text_.data() + e.off, e.len);
}

bool Run::init(u32 threads, TextBuf* err) {
    // Budgets first (Phase 7b): they become ordinary settings, so the world,
    // the config a snapshot carries and the replay log all see the same ones.
    budget_notes_.clear();
    if (!resolve_budgets(config_, threads ? threads : ThreadPool::hardware_threads(), &budget_notes_, err))
        return false;
    if (!world_.init(config_)) {
        err->append(config_.value_error());
        return false;
    }
    pool_.start(threads);
    return true;
}

bool Run::init_from_snapshot(const u8* data, usize n, u32 threads, TextBuf* err, bool (*edit)(Config&, void*),
                             void* ctx) {
    SnapshotHeader h;
    if (!snapshot_parse(data, n, &h, err))
        return false;
    Config::ParseError pe;
    if (!config_.parse(h.config_text, &pe)) {
        err->append("snapshot: embedded config: ");
        err->append(pe.msg);
        return false;
    }
    if (edit && !edit(config_, ctx)) {
        err->append("snapshot: the config's changes were refused");
        return false;
    }
    // A varied world (docs/world-plan.md W9): a snapshot from before v4 comes
    // from a world without substrates, sparks by activity or the archive (disk
    // tiers, W4; its disk section has none), so it resumes with all three
    // off, unless its config (or an edit above) names them.
    if (h.version < 4 && key_peek_str(config_, "world.model").eq("soup")) {
        const Str g = key_peek_str(config_, "world.graph");
        const bool map = g.eq("procedural") || g.eq("landscape"); // (substrates are read only there)
        if (!config_.has("spark.activity"))
            config_.set("spark.activity", "0");
        if (map && !config_.has("substrate.enabled"))
            config_.set("substrate.enabled", "false");
        if (!config_.has("world.archive"))
            config_.set("world.archive", "0");
    }
    budget_notes_.clear(); // a snapshot's budgets were resolved when it ran: this only checks the fit
    if (!resolve_budgets(config_, threads ? threads : ThreadPool::hardware_threads(), &budget_notes_, err))
        return false;
    if (!world_.init(config_)) {
        err->append("snapshot: embedded config: ");
        err->append(config_.value_error());
        return false;
    }
    ByteReader r(data + h.world_offset, h.world_size);
    if (!world_.load_state(r, err, h.version))
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
    StepProfile* prof = world_.profile();
    const double t0 = prof ? clock_seconds() : 0;
    after_step();
    if (prof)
        prof->after += clock_seconds() - t0;
}

void Run::after_step() {
    if (world_.model() == Model::soup)
        instructions_ += world_.last_soup_tick().instructions;
    // World events (Phase 4 lifecycle) first, then each node's detector.
    for (usize i = 0; i < world_.world_events().size(); ++i) {
        const WorldEvent& we = world_.world_events()[i];
        TextBuf t;
        t.append(we.kind == 0 ? "collapse node=" : "recovered node=");
        t.append_u64(we.node);
        t.append(" life=");
        t.append_u64(we.life);
        Event ev;
        ev.tick = we.tick;
        ev.off = static_cast<u32>(event_text_.size());
        ev.len = static_cast<u32>(t.size());
        event_text_.append(t.view().p, t.size());
        events_.push(ev);
        if (replay_.is_open())
            replay_.event(we.tick, t.view());
    }
    world_.clear_world_events();
    drain_detection();
    stats_.record(world_);
    if (replay_.is_open() && checkpoint_every_ && world_.tick() % checkpoint_every_ == 0)
        replay_.checkpoint(world_.tick(), world_.hash());
    if (snap_every_ && world_.tick() % snap_every_ == 0) {
        TextBuf path;
        path.append(snap_path_.view());
        path.append_char('.');
        path.append_u64(world_.tick());
        ByteWriter bytes;
        snapshot(bytes);
        if (!write_file(path.c_str(), bytes.data(), bytes.size()) && snap_error_.size() == 0) {
            snap_error_.append("cannot write snapshot '");
            snap_error_.append(path.view());
            snap_error_.append_char('\'');
        }
    }
}

void Run::open_snapshots(const char* path, u64 every) {
    snap_path_.clear();
    snap_path_.append(path);
    snap_every_ = every;
}

// The barrier half of detection: each node's outputs, in node order.
void Run::drain_detection() {
    if (world_.model() != Model::soup)
        return;
    const CodonTable& table = world_.soup_params().table;
    for (u32 n = 0; n < world_.node_count(); ++n) {
        Lineages* L = world_.node_lineages(n);
        if (!L)
            return; // detection is off on every node
        const u8* codons = L->codons();

        for (usize i = 0; i < L->gates().size(); ++i) {
            const GateEvent& g = L->gates()[i];
            TextBuf t;
            const char* const kinds[kEventCount] = {"replicator node=", "gpu_replicator node=",
                                                    "npu_user node=",   "colonist node=",
                                                    "disk_user node=",  "signaller node=",
                                                    "gpu_native node=", "defended node="};
            t.append(kinds[g.kind < kEventCount ? g.kind : 0]);
            t.append_u64(n);
            t.append(" lineage=");
            append_lineage(t, g.lineage);
            t.append(" depth=");
            t.append_u64(g.depth);
            t.append(" alive=");
            t.append_u64(g.alive);
            t.append(" len=");
            t.append_u64(g.len);
            t.append(" genotype=");
            t.append_hex64(g.genotype);
            t.append(g.via == kViaInject ? " via=inject" : " via=divide");
            Event ev;
            ev.tick = g.tick;
            ev.off = static_cast<u32>(event_text_.size());
            ev.len = static_cast<u32>(t.size());
            event_text_.append(t.view().p, t.size());
            events_.push(ev);
            if (replay_.is_open())
                replay_.event(g.tick, t.view());
            if (g.kind != kEventReplicator) {
                Detection& d = g.kind == kEventGpuReplicator
                                   ? gpu_detection_
                                   : (g.kind == kEventNpuUser
                                          ? npu_detection_
                                          : (g.kind == kEventColonist
                                                 ? colonist_
                                                 : (g.kind == kEventDiskUser
                                                        ? disk_user_
                                                        : (g.kind == kEventGpuNative
                                                               ? gpu_native_
                                                               : (g.kind == kEventDefended ? defended_ : signaller_)))));
                if (!d.found) {
                    d.found = true;
                    d.tick = g.tick;
                    d.instructions = instructions_;
                    d.node = n;
                    d.lineage = g.lineage;
                    d.depth = g.depth;
                    d.alive = g.alive;
                    d.len = g.len;
                    d.via = g.via;
                    d.genotype = g.genotype;
                }
                continue;
            }
            if (!detection_.found) {
                detection_.found = true;
                detection_.tick = g.tick;
                detection_.instructions = instructions_;
                detection_.node = n;
                detection_.lineage = g.lineage;
                detection_.depth = g.depth;
                detection_.alive = g.alive;
                detection_.len = g.len;
                detection_.via = g.via;
                detection_.genotype = g.genotype;
                detection_genome_.clear();
                genome_names(table, codons + g.codon_off, g.len, detection_genome_);
            }
        }

        if (keep_bank_) {
            for (usize i = 0; i < L->saves().size(); ++i) {
                const GenebankSave& s = L->saves()[i];
                GenebankRecord r = {};
                r.tick = s.tick;
                r.genotype = s.genotype;
                r.parent = s.parent;
                r.lineage = s.lineage;
                r.node = n;
                r.len = s.len;
                r.name = s.name;
                r.parent_name = s.parent_name;
                if (s.parent_name != Genebank::kNoName) {
                    const GenotypeMeta* pm = L->genebank().meta(s.parent);
                    r.parent_len = pm ? pm->len : 0;
                }
                r.codon_off = static_cast<u32>(bank_codons_.size());
                bank_codons_.append(codons + s.codon_off, s.len);
                bank_log_.push(r);
            }
        }
        if (genebank_.is_open()) {
            for (usize i = 0; i < L->saves().size(); ++i) {
                const GenebankSave& s = L->saves()[i];
                TextBuf t;
                Genebank::format_name(s.len, s.name, t);
                t.append(" tick=");
                t.append_u64(s.tick);
                t.append(" node=");
                t.append_u64(n);
                t.append(" lineage=");
                append_lineage(t, s.lineage);
                t.append(" parent=");
                if (s.parent_name != Genebank::kNoName) {
                    const GenotypeMeta* pm = L->genebank().meta(s.parent);
                    Genebank::format_name(pm ? pm->len : 0, s.parent_name, t);
                } else {
                    t.append(s.parent ? "unsaved" : "none");
                }
                t.append(" hash=");
                t.append_hex64(s.genotype);
                t.append(" parent_hash=");
                if (s.parent)
                    t.append_hex64(s.parent);
                else
                    t.append_char('-');
                t.append("\n  ");
                genome_names(table, codons + s.codon_off, s.len, t);
                t.append_char('\n');
                genebank_.write(t.view());
            }
            if (!L->saves().empty())
                genebank_.flush();
        }

        if (lineages_.is_open()) {
            for (usize i = 0; i < L->ends().size(); ++i)
                write_lineage_row(L->ends()[i], n, false);
            if (!L->ends().empty())
                lineages_.flush();
        }
        L->clear_outputs();
    }
}

// One row per lineage per node: a lineage that spans nodes (Phase 3) has a row
// for each node it lived on, its founding node in its id.
void Run::write_lineage_row(const LineageEnd& e, u32 node, bool alive) {
    const u64 cols[] = {node,          e.info.founded, e.ended,
                        e.info.max_depth, e.info.peak_alive, e.info.members, e.info.founder_len,
                        e.info.divide_links, e.info.inject_links, e.info.gpu_links};
    TextBuf t;
    append_lineage(t, e.id);
    for (usize i = 0; i < sizeof cols / sizeof cols[0]; ++i) {
        t.append_char(',');
        t.append_u64(cols[i]);
        if (i == 2)
            t.append(alive ? ",alive" : ",extinct");
    }
    t.append_char(',');
    t.append_hex64(e.info.founder_genotype);
    t.append(e.info.detected ? ",1\n" : ",0\n");
    lineages_.write(t.view());
}

bool Run::open_genebank(const char* path, TextBuf* err) {
    if (!genebank_.open(path)) {
        err->append("cannot open genebank file '");
        err->append(path);
        err->append_char('\'');
        return false;
    }
    genebank_.write("# Nova genebank (design.md 6.6): one entry per genotype of a replicator lineage,\n"
                    "# saved when its living count first reaches detect.genebank_threshold.\n"
                    "# <name> tick node lineage parent hash parent_hash, then the genome.\n");
    genebank_.flush();
    return true;
}

bool Run::open_lineages(const char* path, TextBuf* err) {
    if (!lineages_.open(path)) {
        err->append("cannot open lineage file '");
        err->append(path);
        err->append_char('\'');
        return false;
    }
    lineages_.write("lineage,node,founded,ended,status,max_depth,peak_alive,members,founder_len,"
                    "divide_links,inject_links,gpu_links,founder_genotype,detected\n");
    lineages_.flush();
    return true;
}

void Run::run_until(u64 tick) {
    while (world_.tick() < tick)
        step();
}

bool Run::open_stats(const char* path, u32 window, TextBuf* err) {
    if (!stats_.open(path, window, world_.model())) {
        err->append("cannot open stats file '");
        err->append(path);
        err->append("' (or stats window is 0)");
        return false;
    }
    return true;
}

bool Run::open_nodes(const char* path, TextBuf* err, u32 every) {
    if (!stats_.open_nodes(path, every)) {
        err->append("cannot open node file '");
        err->append(path);
        err->append("' (it needs the soup model and output.csv)");
        return false;
    }
    return true;
}

bool Run::open_signals(const char* path, TextBuf* err) {
    if (!stats_.open_signals(path, world_.channels())) {
        err->append("cannot open signal file '");
        err->append(path);
        err->append("' (it needs signal.channels and output.csv)");
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
    if (lineages_.is_open()) {
        Array<LineageEnd> living;
        for (u32 n = 0; n < world_.node_count(); ++n) {
            const Lineages* L = world_.node_lineages(n);
            if (!L)
                break;
            L->living_report(world_.tick(), &living);
            for (usize i = 0; i < living.size(); ++i)
                write_lineage_row(living[i], n, true);
        }
        lineages_.close();
    }
    genebank_.close();
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

    // Observer events (v2): the replay must see exactly the logged events, in
    // order, up to the tick it reached.
    if (log.version() >= 2) {
        const u64 reached = run.world().tick();
        usize logged = 0;
        while (logged < log.event_count() && log.event_tick(logged) < reached)
            ++logged;
        const usize replayed = run.event_count();
        for (usize i = 0; i < logged || i < replayed; ++i) {
            bool same = i < logged && i < replayed && log.event_tick(i) == run.event_tick(i) &&
                        log.event_text(i).eq(run.event_text(i));
            if (same)
                continue;
            if (i >= replayed)
                snprintf(out->msg, sizeof out->msg, "logged event at tick %llu did not happen: %.*s",
                         static_cast<unsigned long long>(log.event_tick(i)),
                         static_cast<int>(log.event_text(i).n), log.event_text(i).p);
            else if (i >= logged)
                snprintf(out->msg, sizeof out->msg, "unlogged event at tick %llu: %.*s",
                         static_cast<unsigned long long>(run.event_tick(i)),
                         static_cast<int>(run.event_text(i).n), run.event_text(i).p);
            else
                snprintf(out->msg, sizeof out->msg, "event %llu differs: logged tick %llu, replayed tick %llu: %.*s",
                         static_cast<unsigned long long>(i),
                         static_cast<unsigned long long>(log.event_tick(i)),
                         static_cast<unsigned long long>(run.event_tick(i)),
                         static_cast<int>(run.event_text(i).n), run.event_text(i).p);
            return false;
        }
        out->events_checked = logged;
    }
    out->ok = true;
    return true;
}

} // namespace nova
