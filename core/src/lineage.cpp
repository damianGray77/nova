#include "nova/lineage.h"

#include "nova/keys.h" // the settings registry
#include "nova/soup.h"

namespace nova {

bool detect_params_from_config(Config& cfg, DetectParams* p) {
    p->enabled = key_flag(cfg, "detect.enabled");
    p->similarity = key_f64(cfg, "detect.similarity");
    p->shift = key_u32(cfg, "detect.shift"); // X12: DIVIDE alignments shifted up to this many codons each way
    p->genebank_threshold = key_u32(cfg, "detect.genebank_threshold");
    p->written = key_flag(cfg, "detect.written");
    p->gate_depth = key_u32(cfg, "detect.gate_depth");
    p->gate_alive = key_u32(cfg, "detect.gate_alive");
    p->signal_sustained = key_u32(cfg, "signal.sustained");
    p->defended = key_u32(cfg, "detect.defended");
    p->parasite = key_u32(cfg, "detect.parasite"); // docs/bites-plan.md
    p->repairs = key_u32(cfg, "detect.repairs");
    return cfg.ok();
}

void Lineages::init(const DetectParams* p, u32 node) {
    p_ = p;
    node_ = node;
    next_id_ = 1;
    links_ = parasite_links_ = gpu_links_ = founded_ = detections_ = gpu_detections_ = 0;
    npu_detections_ = colonisations_ = disk_detections_ = signal_detections_ = defended_detections_ = 0;
    sleep_detections_ = parasite_detections_ = healer_detections_ = 0;
    for (u32 i = 0; i < kOriginCount; ++i)
        founded_by_[i] = 0;
    max_depth_ever_ = 0;
    tags_.clear();
    guest_tags_.clear();
    table_.clear();
    colonised_.clear();
    written_.clear();
    gpu_written_.clear();
    bank_.clear(p->genebank_threshold);
    clear_outputs();
}

void Lineages::reset(usize block_slots, u32 soup_size) {
    tags_.resize(block_slots);
    tags_.fill(BlockTag{});
    guest_tags_.clear();
    written_.resize((static_cast<usize>(soup_size) + 63) / 64);
    written_.fill(0);
    gpu_written_.resize(written_.size());
    gpu_written_.fill(0);
}

void Lineages::clear_outputs() {
    gates_.clear();
    saves_.clear();
    ends_.clear();
    codons_.clear();
}

u32 Lineages::stash(const u8* code, u32 len) {
    u32 off = static_cast<u32>(codons_.size());
    codons_.append(code, len);
    return off;
}

void Lineages::on_new_block(u32 k, u32 start, u32 len) {
    if (k >= tags_.size())
        tags_.resize(static_cast<usize>(k) + 1); // zero-filled: untagged
    else
        tags_[k] = BlockTag{};
    for (u32 a = start; a < start + len; ++a) { // nothing written into it yet
        written_[a >> 6] &= ~(1ull << (a & 63));
        gpu_written_[a >> 6] &= ~(1ull << (a & 63));
    }
}

void Lineages::on_free_block(u32 k, u64 tick) {
    if (k < tags_.size() && tags_[k].lineage != 0)
        leave(k, tick);
}

void Lineages::join(u32 k, const u8* code, u32 len, u64 tick, u64 lineage, u32 depth, u64 parent) {
    join_tag(&tags_[k], code, len, tick, lineage, depth, parent);
}

// A lineage founded now by `body` (kNone: unknown), whose code is
// [code, code + len): its id. The founder joins it next (join, join_tag).
u64 Lineages::found(const Soup& s, u32 body, const u8* code, u32 len, u64 tick) {
    const u64 id = (static_cast<u64>(node_) << 32) | next_id_++;
    ++founded_;
    LineageInfo& L = table_.get_or_insert(id);
    L = LineageInfo{};
    L.founded = tick;
    L.founder_len = len;
    L.founder_genotype = genotype_hash(code, len);
    L.founder_origin = body != kNone ? s.origin(body) : kOriginUnknown; // W7
    ++founded_by_[L.founder_origin < kOriginCount ? L.founder_origin : kOriginUnknown];
    return id;
}

void Lineages::join_tag(BlockTag* tag, const u8* code, u32 len, u64 tick, u64 lineage, u32 depth, u64 parent) {
    const u64 g = genotype_hash(code, len);
    *tag = BlockTag{g, tick, lineage, depth, 0};
    LineageInfo* L = table_.find(lineage);
    NOVA_ASSERT(L);
    ++L->alive;
    ++L->members;
    if (L->alive > L->peak_alive)
        L->peak_alive = L->alive;
    if (depth > L->max_depth)
        L->max_depth = depth;
    L->last_birth = tick;
    if (depth > max_depth_ever_)
        max_depth_ever_ = depth;
    if (bank_.observe(g, GenotypeMeta{parent, tick, lineage, len})) {
        // The record describes the genotype as first seen: its first parent,
        // not the parent of the copy that happened to reach the threshold.
        const GenotypeMeta* m = bank_.meta(g);
        GenebankSave s = {};
        s.tick = tick;
        s.genotype = g;
        s.parent = m->parent;
        s.lineage = m->lineage;
        s.len = len;
        s.name = bank_.name_index(g);
        s.parent_name = m->parent ? bank_.name_index(m->parent) : Genebank::kNoName;
        s.codon_off = stash(code, len);
        saves_.push(s);
    }
}

void Lineages::leave(u32 k, u64 tick) { leave_tag(&tags_[k], tick); }

// docs/inject-plan.md M1: a program moving into its host keeps its
// membership on the body until it owns a block again (or dies).
void Lineages::on_move_in(u32 guest, u32 k) {
    if (guest_tags_.size() <= guest)
        guest_tags_.resize(static_cast<usize>(guest) + 1);
    guest_tags_[guest] = k < tags_.size() ? tags_[k] : BlockTag{};
    if (k < tags_.size())
        tags_[k] = BlockTag{}; // (freeing the block then leaves nothing)
}

void Lineages::on_guest_freed(const Soup& s, u32 guest, u32 k, u64 tick) {
    (void)s;
    (void)tick;
    if (guest >= guest_tags_.size() || guest_tags_[guest].lineage == 0)
        return;
    if (k >= tags_.size())
        tags_.resize(static_cast<usize>(k) + 1);
    tags_[k] = guest_tags_[guest];
    guest_tags_[guest] = BlockTag{};
}

void Lineages::on_guest_gone(const Soup& s, u32 guest, u64 tick) {
    (void)s;
    if (guest < guest_tags_.size() && guest_tags_[guest].lineage != 0)
        leave_tag(&guest_tags_[guest], tick);
}

void Lineages::leave_tag(BlockTag* tp, u64 tick) {
    BlockTag& t = *tp;
    const u64 id = t.lineage;
    bank_.release(t.genotype);
    t = BlockTag{};
    LineageInfo* L = table_.find(id);
    NOVA_ASSERT(L && L->alive > 0);
    if (!L || L->alive == 0)
        return;
    if (--L->alive == 0) {
        LineageEnd e = {};
        e.info = *L;
        e.ended = tick;
        e.id = id;
        ends_.push(e);
        table_.erase(id);
    }
}

bool Lineages::similar(u32 match, u32 len_a, u32 len_b) const {
    return static_cast<double>(match) >=
           p_->similarity * static_cast<double>(max_of(len_a, len_b));
}

namespace {

// The rotation key (her first codons looked for in the parent), and the
// fewest matching codons that show descent (X12).
constexpr u32 kDescentFloor = 4;

// share x n, rounded up (share in (0, 1]), at most n.
u32 share_of(double share, u32 n) {
    const double want = share * static_cast<double>(n);
    u32 k = static_cast<u32>(want);
    if (static_cast<double>(k) < want)
        ++k;
    return k < n ? k : n;
}

} // namespace

u32 Lineages::aligned(const u8* par, u32 plen, const u8* m, u32 dstart, u32 dlen, u32 r, u32 misses,
                      u32* by_gpu) const {
    // Only codons the parent's body wrote into the daughter since her ALLOC
    // count (detect.written): budding onto a relative's remains is not copying.
    u32 match = 0, miss = 0, gpu = 0;
    u32 j = r; // the parent's codon facing her codon i, read cyclically
    for (u32 i = 0; i < dlen; ++i) {
        const u32 a = dstart + i;
        if (par[j] == m[a] && (!p_->written || is_written(a))) {
            ++match;
            gpu += is_gpu_written(a);
        } else if (++miss > misses) {
            break; // this alignment can no longer pass
        }
        if (++j == plen)
            j = 0;
    }
    *by_gpu = gpu;
    return match;
}

// Descent, not likeness (disasters plan X12): her codons, written by the
// parent's body and matching it at the best alignment, against her length.
// The first alignment that passes is taken: straight, then shifts nearest
// first, then the rotation candidates in the parent's order. A failing
// alignment stops once more than (1 - similarity) of her codons miss (a
// quarter, at 0.75), so a birth costs a few hundred codon comparisons at
// most, outside world time.
bool Lineages::descends(const Soup& s, u32 p, u32 k, u32* match, u32* by_gpu) const {
    const Block& P = s.block(p);
    return descends_from(s, P.start, P.len, k, match, by_gpu);
}

bool Lineages::descends_from(const Soup& s, u32 pstart, u32 plen, u32 k, u32* match, u32* by_gpu) const {
    const Block& D = s.block(k);
    if (plen == 0 || D.len == 0)
        return false;
    const u8* m = s.mem();
    const u8* par = m + pstart;
    // The fewest matches that pass: similarity x her length, rounded up, and
    // never fewer than kDescentFloor codons (or similarity x the parent's
    // length, if that is fewer): one written codon matching somewhere in a
    // long parent is chance, not descent. Every link the same-offset rule
    // made before X12 passes this too (its matches are a subset of the
    // straight comparison's, and it needed similarity x the longer length).
    const u32 need = max_of(share_of(p_->similarity, D.len), min_of(kDescentFloor, share_of(p_->similarity, plen)));
    if (need > D.len)
        return false; // too short to show descent
    const u32 misses = D.len - need; // the most misses an alignment may have
    u32 n = 0, g = 0;
    auto passes = [&](u32 r) {
        n = aligned(par, plen, m, D.start, D.len, r, misses, &g);
        return n >= need;
    };
    bool ok = passes(0); // the straight comparison
    // Shifts of 1 .. detect.shift codons, forward then back (a head off by a
    // codon or two); past half the parent they would repeat one another.
    const u32 smax = min_of(p_->shift, plen / 2);
    for (u32 d = 1; d <= smax && !ok; ++d)
        ok = passes(d) || (plen - d != d && passes(plen - d));
    // Rotations (a copy that starts part-way through the parent): only where
    // her first 4 codons (all of hers, if fewer) appear in the parent, read
    // cyclically, and not already tried as a shift; at most 8.
    const u32 key = min_of(kDescentFloor, D.len);
    u32 tried = 0;
    for (u32 r = smax + 1; r + smax < plen && tried < 8 && !ok; ++r) {
        u32 j = 0, q = r;
        while (j < key && par[q] == m[D.start + j]) {
            ++j;
            if (++q == plen)
                q = 0;
        }
        if (j < key)
            continue;
        ++tried;
        ok = passes(r);
    }
    if (ok) {
        *match = n;
        *by_gpu = g;
    }
    return ok;
}

void Lineages::on_divide(const Soup& s, u32 p, u32 k, u64 tick) {
    // A CPU running inside the block it splits off is not a parent copying
    // itself: the daughter would trivially match.
    if (p == kNone || p == k)
        return;
    u32 match = 0, by_gpu = 0;
    if (descends(s, p, k, &match, &by_gpu))
        link(s, p, k, tick, kViaDivide, 2 * by_gpu > match);
}

void Lineages::on_npu(const Soup& s, u32 k, u64 tick) {
    if (k == kNone || k >= tags_.size() || tags_[k].lineage == 0)
        return;
    LineageInfo* L = table_.find(tags_[k].lineage);
    if (!L || !L->detected || L->npu_detected)
        return;
    L->npu_detected = 1;
    ++npu_detections_;
    const Block& B = s.block(k);
    push_event(kEventNpuUser, tick, tags_[k].lineage, *L, tags_[k], s.mem() + B.start, B.len, kViaDivide);
}

// The first DISK load by a member of a gated lineage that writes into the
// block it runs in: restoring its own code from disk (Phase 4).
void Lineages::on_disk_load(const Soup& s, u32 k, u64 tick) {
    if (k == kNone || k >= tags_.size() || tags_[k].lineage == 0)
        return;
    LineageInfo* L = table_.find(tags_[k].lineage);
    if (!L || !L->detected || L->disk_detected)
        return;
    L->disk_detected = 1;
    ++disk_detections_;
    const Block& B = s.block(k);
    push_event(kEventDiskUser, tick, tags_[k].lineage, *L, tags_[k], s.mem() + B.start, B.len, kViaDivide);
}

// The first voluntary sleep by a member of a gated lineage
// (docs/lifehistory-plan.md V7). Random code yields with BX 1 and some CX
// often; the gate filters it, as for the other milestones.
void Lineages::on_sleep(const Soup& s, u32 k, u64 tick) {
    if (k == kNone || k >= tags_.size() || tags_[k].lineage == 0)
        return;
    LineageInfo* L = table_.find(tags_[k].lineage);
    if (!L || !L->detected || L->sleep_detected)
        return;
    L->sleep_detected = 1;
    ++sleep_detections_;
    const Block& B = s.block(k);
    push_event(kEventSleeper, tick, tags_[k].lineage, *L, tags_[k], s.mem() + B.start, B.len, kViaDivide);
}

// Sustained signal use (Phase 4): once a lineage has met the gate, count the
// distinct ticks on which any member here emits; reaching signal.sustained is
// the signaller event. Random code emits all the time; the gate filters it.
void Lineages::on_emit(const Soup& s, u32 k, u64 tick) {
    if (k == kNone || k >= tags_.size() || tags_[k].lineage == 0)
        return;
    LineageInfo* L = table_.find(tags_[k].lineage);
    if (!L || !L->detected || L->signal_detected || L->last_emit == tick + 1)
        return;
    L->last_emit = tick + 1;
    if (++L->emit_ticks < p_->signal_sustained)
        return;
    L->signal_detected = 1;
    ++signal_detections_;
    const Block& B = s.block(k);
    push_event(kEventSignaller, tick, tags_[k].lineage, *L, tags_[k], s.mem() + B.start, B.len, kViaDivide);
}

// Defences (Phase 9): once a lineage has met the gate, count the actions
// aimed at its members that failed because the member changed in time;
// reaching detect.defended is the `defended` event. Chance alone (an attack
// that rolled badly against an unchanged target) never counts.
void Lineages::on_defended(const Soup& s, u32 k, u64 tick) {
    if (k == kNone || k >= tags_.size() || tags_[k].lineage == 0)
        return;
    LineageInfo* L = table_.find(tags_[k].lineage);
    if (!L || !L->detected || L->defences >= p_->defended)
        return;
    if (++L->defences < p_->defended)
        return;
    ++defended_detections_;
    const Block& B = s.block(k);
    push_event(kEventDefended, tick, tags_[k].lineage, *L, tags_[k], s.mem() + B.start, B.len, kViaDivide);
}

void Lineages::push_event(u32 kind, u64 tick, u64 lineage, const LineageInfo& L, const BlockTag& child,
                          const u8* code, u32 len, u32 via) {
    GateEvent e = {};
    e.tick = tick;
    e.genotype = child.genotype;
    e.lineage = lineage;
    e.depth = child.depth;
    e.alive = L.alive;
    e.len = len;
    e.via = via;
    e.kind = kind;
    e.codon_off = stash(code, len);
    gates_.push(e);
}

void Lineages::link(const Soup& s, u32 p, u32 k, u64 tick, u32 via, bool gpu) {
    const Block& P = s.block(p);
    const u8* m = s.mem();
    ++links_;
    const usize need = static_cast<usize>(max_of(p, k)) + 1;
    if (tags_.size() < need)
        tags_.resize(need);
    if (tags_[p].lineage == 0) // the parent founds a lineage
        join(p, m + P.start, P.len, tick, found(s, P.body, m + P.start, P.len, tick), 0, 0);
    adopt(s, tags_[p], P.body, k, tick, via, gpu);
}

void Lineages::adopt(const Soup& s, BlockTag parent, u32 parent_body, u32 k, u64 tick, u32 via, bool gpu) {
    const Block& D = s.block(k);
    const u8* m = s.mem();
    if (tags_.size() <= k)
        tags_.resize(static_cast<usize>(k) + 1);
    LineageInfo* L = table_.find(parent.lineage);
    if (!L)
        return;
    ++L->divide_links;
    if (via == kViaParasite)
        ++L->parasite_links;
    if (gpu) {
        ++gpu_links_;
        ++L->gpu_links;
    }
    if (tags_[k].lineage == parent.lineage)
        return; // a member copied over again (re-infection): not a new member
    if (tags_[k].lineage != 0) // a member of another lineage, taken over
        leave(k, tick);
    join(k, m + D.start, D.len, tick, parent.lineage, parent.depth + 1, parent.genotype);

    L = table_.find(parent.lineage);
    if (!L)
        return;
    if (!L->detected && L->max_depth >= p_->gate_depth && L->alive >= p_->gate_alive) {
        L->detected = 1;
        ++detections_;
        push_event(kEventReplicator, tick, parent.lineage, *L, tags_[k], m + D.start, D.len, via);
    }
    // The first GPU link of a lineage that has met the gate (Phase 2).
    if (gpu && L->detected && !L->gpu_detected) {
        L->gpu_detected = 1;
        ++gpu_detections_;
        push_event(kEventGpuReplicator, tick, parent.lineage, *L, tags_[k], m + D.start, D.len, via);
    }
    // The first DIVIDE link by a GPU-born parent, in a lineage that has met
    // the gate (Phase 8): life that replicates on the GPU alone.
    if (via == kViaDivide && L->detected && !L->native_detected && parent_body != kNone &&
        s.body(parent_body).form == kFormGpu) {
        L->native_detected = 1;
        push_event(kEventGpuNative, tick, parent.lineage, *L, tags_[k], m + D.start, D.len, via);
    }
    // docs/bites-plan.md I5: the detect.parasite-th daughter its parasite
    // threads made from inside hosts, once gated, is the parasite event.
    if (via == kViaParasite && L->detected && L->parasite_divides < p_->parasite &&
        ++L->parasite_divides == p_->parasite) {
        ++parasite_detections_;
        push_event(kEventParasite, tick, parent.lineage, *L, tags_[k], m + D.start, D.len, via);
    }
}

// A DIVIDE by a parasite (docs/inject-plan.md M4): the daughter joins its
// lineage (the tag it carries on its body since it moved in, on_move_in) if
// she descends from its code, as a DIVIDE daughter from her parent; a
// parasite outside any lineage founds one then, as a parent does.
void Lineages::on_divide_parasite(const Soup& s, u32 cpu, u32 k, u64 tick) {
    const u32 body = s.cpu(cpu).body;
    if (body == kNone || !s.is_parasite(body))
        return;
    if (guest_tags_.size() <= body)
        guest_tags_.resize(static_cast<usize>(body) + 1);
    if (guest_tags_[body].lineage != 0 && !table_.find(guest_tags_[body].lineage))
        return; // (its lineage ended here meanwhile: not while it was a member)
    // M2: the parasite's code is its region in its host's first block,
    // clipped to what is left of that block.
    const u32 host = s.host_of(body);
    if (host == kNone || s.body(host).blocks == kNone || s.body(host).blocks == k)
        return;
    const Block& H = s.block(s.body(host).blocks);
    const u32 lo = max_of<u32>(s.guest_at(body), H.start);
    const u32 hi = min_of<u32>(s.guest_at(body) + s.guest_len(body), H.start + H.len);
    if (lo >= hi)
        return; // the host shrank past it
    u32 match = 0, by_gpu = 0;
    if (!descends_from(s, lo, hi - lo, k, &match, &by_gpu))
        return;
    ++links_;
    ++parasite_links_;
    const u8* m = s.mem();
    if (guest_tags_[body].lineage == 0) // the parasite founds a lineage
        join_tag(&guest_tags_[body], m + lo, hi - lo, tick, found(s, body, m + lo, hi - lo, tick), 0, 0);
    adopt(s, guest_tags_[body], kNone, k, tick, kViaParasite, false);
}

// B5: the detect.repairs-th open end a gated lineage's members closed with a
// NOP is the healer event.
void Lineages::on_repair(const Soup& s, u32 k, u64 tick) {
    if (k == kNone || k >= tags_.size() || tags_[k].lineage == 0)
        return;
    LineageInfo* L = table_.find(tags_[k].lineage);
    if (!L || !L->detected || L->repairs >= p_->repairs)
        return;
    if (++L->repairs < p_->repairs)
        return;
    ++healer_detections_;
    const Block& B = s.block(k);
    push_event(kEventHealer, tick, tags_[k].lineage, *L, tags_[k], s.mem() + B.start, B.len, kViaDivide);
}

void Lineages::survey(LineageSurvey* out) const {
    *out = LineageSurvey{};
    out->lineages = static_cast<u32>(table_.size());
    for (usize i = 0; i < table_.slot_count(); ++i) {
        if (!table_.slot_used(i))
            continue;
        const u64 id = table_.slot_key(i);
        const u32 alive = table_.slot_value(i).alive;
        if (alive > out->top_alive || (alive == out->top_alive && alive > 0 && id < out->top_lineage)) {
            out->top_alive = alive;
            out->top_lineage = id;
        }
    }
    Array<u64> genos;
    for (usize k = 0; k < tags_.size(); ++k) {
        const BlockTag& t = tags_[k];
        if (t.lineage == 0)
            continue;
        if (t.depth > out->max_depth)
            out->max_depth = t.depth;
        if (t.lineage == out->top_lineage)
            genos.push(t.genotype);
        if (lineage_node(t.lineage) != node_)
            ++out->colonists;
    }
    sort_u64(genos.data(), genos.size());
    for (usize i = 0; i < genos.size(); ++i)
        if (i == 0 || genos[i] != genos[i - 1])
            ++out->top_genotypes;
}

void Lineages::living_report(u64 tick, Array<LineageEnd>* out) const {
    out->clear();
    Array<u64> ids;
    for (usize i = 0; i < table_.slot_count(); ++i)
        if (table_.slot_used(i))
            ids.push(table_.slot_key(i));
    sort_u64(ids.data(), ids.size());
    for (usize i = 0; i < ids.size(); ++i) {
        LineageEnd e = {};
        e.info = *table_.find(ids[i]);
        e.ended = tick;
        e.id = ids[i];
        out->push(e);
    }
}

void append_lineage(TextBuf& out, u64 id) {
    out.append_u64(lineage_node(id));
    out.append(".");
    out.append_u64(id & 0xFFFFFFFFull);
}

CargoTag Lineages::cargo_tag(u32 k) const {
    CargoTag c = {};
    if (k < tags_.size() && tags_[k].lineage != 0) {
        const BlockTag& t = tags_[k];
        c.lineage = t.lineage;
        c.genotype = t.genotype;
        c.depth = t.depth;
        const GenotypeMeta* gm = bank_.meta(t.genotype);
        c.parent = gm ? gm->parent : 0;
        const LineageInfo* L = table_.find(t.lineage);
        c.gated = L && L->detected ? 1 : 0;
        c.origin = L ? static_cast<u8>(L->founder_origin) : kOriginUnknown;
    }
    return c;
}

const char* origin_name(u32 origin) {
    switch (origin) {
    case kOriginSpark:
        return "spark";
    case kOriginDaughter:
        return "daughter";
    case kOriginArrival:
        return "arrival";
    case kOriginWake:
        return "wake";
    default:
        return "unknown";
    }
}

// A dispersed daughter placed in block k. She keeps her lineage and depth:
// she is the same member, moved. If the lineage has no living member here
// yet, it gets an entry on this node (founded = arrival tick); the gate state
// travels with her (`gated`), so a gated lineage stays gated here and its
// later events (GPU, NPU) can fire on this node. The first arrival of a gated
// lineage on a node it wasn't founded on is a colonist event, once per
// lineage per node. A dormant program waking rejoins the same way (X12): it
// left its lineage when it went dormant, so it is counted once. (If its block
// is a member already -- taken over by an INJECT while it rested -- it stays
// as it is, rather than being counted twice.)
void Lineages::on_arrive(const Soup& s, u32 k, const CargoTag& tag, u64 tick) {
    if (k >= tags_.size())
        tags_.resize(static_cast<usize>(k) + 1);
    if (tag.lineage == 0 || tags_[k].lineage != 0)
        return;
    const Block& B = s.block(k);
    const u8* code = s.mem() + B.start;
    LineageInfo* L = table_.find(tag.lineage);
    const bool fresh = (L == nullptr);
    if (fresh) {
        LineageInfo& n = table_.get_or_insert(tag.lineage);
        n = LineageInfo{};
        n.founded = tick;
        n.founder_len = B.len;
        n.founder_genotype = tag.genotype;
        n.founder_origin = tag.origin; // the founder's, from its founding node (W7)
        n.detected = tag.gated;
        L = &n;
    } else if (tag.gated && !L->detected) {
        L->detected = 1;
    }
    join(k, code, B.len, tick, tag.lineage, tag.depth, tag.parent);
    L = table_.find(tag.lineage);
    if (tag.gated && lineage_node(tag.lineage) != node_ && !colonised_.contains(tag.lineage)) {
        colonised_.get_or_insert(tag.lineage) = 1;
        ++colonisations_;
        push_event(kEventColonist, tick, tag.lineage, *L, tags_[k], code, B.len, kViaDivide);
    }
}

} // namespace nova
