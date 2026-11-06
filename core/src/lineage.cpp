#include "nova/lineage.h"

#include "nova/soup.h"

namespace nova {

bool detect_params_from_config(Config& cfg, DetectParams* p) {
    p->enabled = cfg.get_bool("detect.enabled", true);
    double sim = cfg.get_f64("detect.similarity", 0.75);
    if (!(sim > 0.0 && sim <= 1.0)) {
        cfg.fail("detect.similarity", "must be above 0 and at most 1");
        sim = 0.75;
    }
    p->similarity = sim;
    u64 t = cfg.get_u64("detect.genebank_threshold", 2);
    if (t < 1 || t > 1000000) {
        cfg.fail("detect.genebank_threshold", "must be between 1 and 1000000");
        t = 2;
    }
    p->genebank_threshold = static_cast<u32>(t);
    p->inject = cfg.get_bool("detect.inject", true);
    p->written = cfg.get_bool("detect.written", true);
    u64 gd = cfg.get_u64("detect.gate_depth", 2), ga = cfg.get_u64("detect.gate_alive", 2);
    if (gd < 1 || gd > 1000000) {
        cfg.fail("detect.gate_depth", "must be between 1 and 1000000");
        gd = 2;
    }
    if (ga < 1 || ga > 1000000) {
        cfg.fail("detect.gate_alive", "must be between 1 and 1000000");
        ga = 2;
    }
    p->gate_depth = static_cast<u32>(gd);
    p->gate_alive = static_cast<u32>(ga);
    u64 ss = cfg.get_u64("signal.sustained", 100);
    if (ss < 1 || ss > 1000000000) {
        cfg.fail("signal.sustained", "must be between 1 and 1000000000");
        ss = 100;
    }
    p->signal_sustained = static_cast<u32>(ss);
    u64 df = cfg.get_u64("detect.defended", 3);
    if (df < 1 || df > 1000000000) {
        cfg.fail("detect.defended", "must be between 1 and 1000000000");
        df = 3;
    }
    p->defended = static_cast<u32>(df);
    return cfg.ok();
}

void Lineages::init(const DetectParams* p, u32 node) {
    p_ = p;
    node_ = node;
    next_id_ = 1;
    links_ = inject_links_ = gpu_links_ = founded_ = detections_ = gpu_detections_ = 0;
    npu_detections_ = colonisations_ = disk_detections_ = signal_detections_ = defended_detections_ = 0;
    max_depth_ever_ = 0;
    tags_.clear();
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
    const u64 g = genotype_hash(code, len);
    tags_[k] = BlockTag{g, tick, lineage, depth, 0};
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

void Lineages::leave(u32 k, u64 tick) {
    BlockTag& t = tags_[k];
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

void Lineages::on_divide(const Soup& s, u32 p, u32 k, u64 tick) {
    // A CPU running inside the block it splits off is not a parent copying
    // itself: the daughter would trivially match.
    if (p == kNone || p == k)
        return;
    const Block& P = s.block(p);
    const Block& D = s.block(k);
    const u32 n = min_of(P.len, D.len);
    const u8* m = s.mem();
    // Only codons the parent's body wrote into the daughter since her ALLOC
    // count (detect.written): budding onto a relative's remains is not copying.
    u32 match = 0, by_gpu = 0;
    for (u32 i = 0; i < n; ++i) {
        const u32 a = D.start + i;
        if (m[P.start + i] == m[a] && (!p_->written || is_written(a))) {
            ++match;
            by_gpu += is_gpu_written(a);
        }
    }
    if (similar(match, P.len, D.len))
        link(s, p, k, tick, kViaDivide, 2 * by_gpu > match);
}

void Lineages::on_inject(const Soup& s, u32 p, u32 v, u32 match, u64 tick) {
    if (!p_->inject || p == kNone || p == v)
        return;
    if (similar(match, s.block(p).len, s.block(v).len))
        link(s, p, v, tick, kViaInject, false);
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
    const Block& D = s.block(k);
    const u8* m = s.mem();
    ++links_;
    if (via == kViaInject)
        ++inject_links_;
    const usize need = static_cast<usize>(max_of(p, k)) + 1;
    if (tags_.size() < need)
        tags_.resize(need);
    if (tags_[p].lineage == 0) { // the parent founds a lineage
        const u64 id = (static_cast<u64>(node_) << 32) | next_id_++;
        ++founded_;
        LineageInfo& L = table_.get_or_insert(id);
        L = LineageInfo{};
        L.founded = tick;
        L.founder_len = P.len;
        L.founder_genotype = genotype_hash(m + P.start, P.len);
        join(p, m + P.start, P.len, tick, id, 0, 0);
    }
    const BlockTag parent = tags_[p];
    LineageInfo* L = table_.find(parent.lineage);
    if (via == kViaInject)
        ++L->inject_links;
    else
        ++L->divide_links;
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
    if (via == kViaDivide && L->detected && !L->native_detected && P.body != kNone &&
        s.body(P.body).form == kFormGpu) {
        L->native_detected = 1;
        push_event(kEventGpuNative, tick, parent.lineage, *L, tags_[k], m + D.start, D.len, via);
    }
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
    }
    return c;
}

// A dispersed daughter placed in block k. She keeps her lineage and depth:
// she is the same member, moved. If the lineage has no living member here
// yet, it gets an entry on this node (founded = arrival tick); the gate state
// travels with her (`gated`), so a gated lineage stays gated here and its
// later events (GPU, NPU) can fire on this node. The first arrival of a gated
// lineage on a node it wasn't founded on is a colonist event, once per
// lineage per node.
void Lineages::on_arrive(const Soup& s, u32 k, const CargoTag& tag, u64 tick) {
    if (k >= tags_.size())
        tags_.resize(static_cast<usize>(k) + 1);
    tags_[k] = BlockTag{};
    if (tag.lineage == 0)
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
