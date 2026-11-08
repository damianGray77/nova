// Behaviour profiles (docs/analysis-plan.md A3): the sampler, a pure observer
// (nova/profile.h explains what it records and why it changes nothing).
#include "nova/profile.h"

#include <cstring> // memset

#include "nova/genebank.h" // genotype_hash
#include "nova/keys.h"
#include "nova/lineage.h"
#include "nova/rng.h"

namespace nova {

namespace {

constexpr u64 kProfileSalt = 0x70726F66696C6573ull; // "profiles": the observer's own stream

const char* const kActionNames[kPaCount] = {
    "engulf_tries", "engulf_landed", "bites_waste",    "inject_tries",  "inject_landed", "divides",
    "dispersals",   "dormant",       "disk_compares",  "disk_stores",   "disk_loads",    "archive_stores",
    "archive_loads", "self_loads",   "emits",          "listens",       "heard_nonzero", "gpu_ops",
    "npu_ops",      "launches",      "threads"};
const char* const kHoldNames[kHoldCount] = {"hold_mem",  "hold_cpu",     "hold_ecpu",    "hold_gpu", "hold_npu",
                                            "hold_disk", "hold_archive", "hold_threads", "hold_warps"};
const char* const kRoleNames[kRoleCount] = {"predator", "parasite", "repairer", "signaller",
                                            "wanderer", "copier",   "idle"};

u32 popcount8(u8 v) {
    u32 n = 0;
    for (; v; v &= static_cast<u8>(v - 1))
        ++n;
    return n;
}

bool is_branch(Op op) {
    switch (op) {
    case Op::ifz:
    case Op::iflt:
    case Op::ifne:
    case Op::iferr:
    case Op::ifeq:
    case Op::ifgt:
    case Op::t_ifz:
    case Op::djnz:
    case Op::jz:
        return true;
    default:
        return false;
    }
}

// The lineage tag of body b's genome block (0 without detection).
BlockTag tag_of(const Soup& s, u32 b) {
    const Lineages* L = s.observer();
    const u32 k = s.body(b).blocks;
    return L && k != kNone ? L->tag(k) : BlockTag{};
}

u64 genome_hash(const Soup& s, u32 b) {
    const u32 k = s.body(b).blocks;
    if (k == kNone)
        return 0;
    const Block& B = s.block(k);
    return genotype_hash(s.mem() + B.start, B.len);
}

// The nearest other living body (resting dormant ones aside) on one side of
// [from, ...) within kProfileNeighbourScan codons, or kNone.
u32 neighbour(const Soup& s, u32 b, i64 from, int dir) {
    for (u32 i = 0; i < kProfileNeighbourScan; ++i) {
        const i64 a = from + dir * static_cast<i64>(i);
        if (a < 0 || a >= static_cast<i64>(s.size()))
            break;
        const u32 o = s.owner_body(static_cast<u32>(a));
        if (o == kNone || o == b || s.dormant(o))
            continue;
        return o;
    }
    return kNone;
}

void mark(ProfileSample& x, u32 block, u32 ip) {
    if (block != x.block || ip < x.gstart)
        return;
    const u32 off = ip - x.gstart;
    if (off < x.glen && off < kProfileGenomeMax)
        x.bits[off >> 3] = static_cast<u8>(x.bits[off >> 3] | (1u << (off & 7)));
}

} // namespace

bool profile_params_from_config(Config& cfg, ProfileParams* p) {
    *p = ProfileParams{};
    p->enabled = key_flag(cfg, "profile.enabled");
    p->every = key_u32(cfg, "profile.every");
    p->per_node = key_u32(cfg, "profile.per_node");
    p->ticks = key_u32(cfg, "profile.ticks");
    p->arrivals = key_flag(cfg, "profile.arrivals");
    if (p->ticks > p->every)
        cfg.fail("profile.ticks", "must not exceed profile.every (one round at a time)");
    return cfg.ok();
}

const char* profile_action_name(u32 a) { return a < kPaCount ? kActionNames[a] : ""; }
const char* profile_hold_name(u32 h) { return h < kHoldCount ? kHoldNames[h] : ""; }
const char* role_name(u32 r) { return r < kRoleCount ? kRoleNames[r] : ""; }

const char* profile_end_name(u32 why) {
    return why == kProfileDone ? "done" : (why == kProfileGone ? "gone" : "dormant");
}

u32 role_by_rules(const u32* act) {
    if (act[kPaEngulf])
        return kRolePredator;
    if (act[kPaInject])
        return kRoleParasite;
    if (act[kPaSelfLoad])
        return kRoleRepairer;
    if (act[kPaEmit])
        return kRoleSignaller;
    if (act[kPaDormant] || act[kPaDisperse] > act[kPaDivide])
        return kRoleWanderer;
    if (act[kPaDivide] || act[kPaDisperse])
        return kRoleCopier;
    return kRoleIdle;
}

u32 profile_working_length(const ProfileSample& s) {
    u32 n = 0;
    for (u32 i = 0; i < kProfileGenomeMax / 8; ++i)
        n += popcount8(s.bits[i]);
    return n;
}

u32 profile_distinct_ops(const ProfileSample& s) {
    u32 n = 0;
    for (u32 i = 0; i < kOpCount; ++i)
        n += s.ops[i] ? 1u : 0u;
    return n;
}

u32 action_kinds(const u32* act) {
    const u32 kinds[] = {act[kPaEngulfTry],    act[kPaBiteWaste], act[kPaInjectTry], act[kPaDivide],
                         act[kPaDisperse],     act[kPaDormant],   act[kPaDiskCompare], act[kPaDiskStore],
                         act[kPaDiskLoad],     act[kPaEmit],      act[kPaListen],    act[kPaGpu],
                         act[kPaNpu],          act[kPaLaunch],    act[kPaThread]};
    static_assert(sizeof kinds / sizeof kinds[0] == kActionKinds, "action kinds");
    u32 n = 0;
    for (u32 v : kinds)
        n += v ? 1u : 0u;
    return n;
}

void ProfileCensus::add(const ProfileSample& s) {
    if (round != s.start) {
        *this = ProfileCensus{};
        round = s.start;
    }
    ++samples;
    ++role[role_by_rules(s.act)];
    work_len += profile_working_length(s);
    kinds += action_kinds(s.act);
}

// --- recording an instruction ---------------------------------------------------

void ProfileProbe::before(const SoupTally& tl, u32 bx_, u32 dflt_, u32 block_) {
    // The order profile_step reads them in.
    t[0] = tl.engulf_tries;
    t[1] = tl.engulfs;
    t[2] = tl.bites_body;
    t[3] = tl.bites_waste;
    t[4] = tl.inject_tries;
    t[5] = tl.injects;
    t[6] = tl.births;
    t[7] = tl.sent;
    t[8] = tl.disk_compares;
    t[9] = tl.disk_stores;
    t[10] = tl.disk_loads;
    t[11] = tl.archive_stores;
    t[12] = tl.archive_loads;
    t[13] = tl.emits;
    t[14] = tl.listens;
    t[15] = tl.launches;
    t[16] = tl.threads_started;
    bx = bx_;
    dflt = dflt_;
    block = block_;
}

void profile_step(ProfileSample& s, const ProfileProbe& pr, const SoupTally& tl, Op op, u32 ip, bool ok, u32 next,
                  u32 cx, bool resolving, bool self_load) {
    const u64* t = pr.t;
    s.act[kPaEngulfTry] += static_cast<u32>(tl.engulf_tries - t[0]);
    s.act[kPaEngulf] += static_cast<u32>((tl.engulfs - t[1]) + (tl.bites_body - t[2]));
    s.act[kPaBiteWaste] += static_cast<u32>(tl.bites_waste - t[3]);
    s.act[kPaInjectTry] += static_cast<u32>(tl.inject_tries - t[4]);
    s.act[kPaInject] += static_cast<u32>(tl.injects - t[5]);
    s.act[kPaDivide] += static_cast<u32>(tl.births - t[6]);
    s.act[kPaDisperse] += static_cast<u32>(tl.sent - t[7]);
    s.act[kPaDiskCompare] += static_cast<u32>(tl.disk_compares - t[8]);
    s.act[kPaDiskStore] += static_cast<u32>(tl.disk_stores - t[9]);
    s.act[kPaDiskLoad] += static_cast<u32>(tl.disk_loads - t[10]);
    s.act[kPaArchiveStore] += static_cast<u32>(tl.archive_stores - t[11]);
    s.act[kPaArchiveLoad] += static_cast<u32>(tl.archive_loads - t[12]);
    s.act[kPaEmit] += static_cast<u32>(tl.emits - t[13]);
    s.act[kPaListen] += static_cast<u32>(tl.listens - t[14]);
    s.act[kPaLaunch] += static_cast<u32>(tl.launches - t[15]);
    s.act[kPaThread] += static_cast<u32>(tl.threads_started - t[16]);
    if (self_load)
        ++s.act[kPaSelfLoad];
    if (resolving) // an action's end: what it did is counted above; the instruction was counted when it began
        return;
    ++s.instr;
    ++s.ops[static_cast<u32>(op)];
    if (!ok)
        ++s.faults;
    mark(s, pr.block, ip);
    if (op == Op::gpu && ok && pr.bx < kLaunchOp)
        ++s.act[pr.bx < kGpuOps ? kPaGpu : kPaNpu];
    // Listening then acting: a LISTEN's heard value against the way the next
    // conditional branch goes.
    if (op == Op::listen && ok) {
        const i32 v = static_cast<i32>(cx);
        if (v != 0)
            ++s.act[kPaHeard];
        s.heard_abs += v < 0 ? static_cast<u64>(-static_cast<i64>(v)) : static_cast<u64>(v);
        s.lt_class = v == 0 ? 0u : (v > 0 ? 1u : 2u);
        s.lt_wait = 1;
        return;
    }
    if (!s.lt_wait)
        return;
    if (is_branch(op)) {
        u32 way;
        if (op == Op::djnz) // a template jump: it went the other way if it jumped
            way = ok && cx != 0;
        else if (op == Op::jz)
            way = ok && cx == 0;
        else // an IF: the other way is skipping the next instruction
            way = next != pr.dflt;
        ++s.lta[s.lt_class * 2 + way];
        s.lt_wait = 0;
    } else if (++s.lt_wait > kLtaWindow) {
        s.lt_wait = 0;
    }
}

void profile_foreign(ProfileSample& s) {
    ++s.instr;
    ++s.foreign;
    ++s.faults;
    if (s.lt_wait && ++s.lt_wait > kLtaWindow)
        s.lt_wait = 0;
}

// --- the sampler -------------------------------------------------------------------

void Profiles::init(const ProfileParams* p, u64 seed, u32 node) {
    p_ = p;
    seed_ = seed;
    node_ = node;
    live_.clear();
    used_.clear();
    slot_.clear();
    nactive_ = 0;
    done_.clear();
}

void Profiles::reset() {
    live_.clear();
    used_.clear();
    slot_.clear();
    nactive_ = 0;
}

void Profiles::holdings(const Soup& s, ProfileSample& x) const {
    const u32 b = x.body;
    const Body& B = s.body(b);
    const u32 e = s.e_units(b), arch = s.archive_held(b);
    x.hold[kHoldMem] = B.mem;
    x.hold[kHoldCpu] = B.units - e;
    x.hold[kHoldEcpu] = e;
    x.hold[kHoldGpu] = s.gpu_count(b);
    x.hold[kHoldNpu] = s.npu_count(b);
    x.hold[kHoldDisk] = s.disk_held(b) - arch;
    x.hold[kHoldArchive] = arch;
    x.hold[kHoldThreads] = B.ncpu;
    x.hold[kHoldWarps] = s.warp_count(b);
}

void Profiles::start(const Soup& s, u32 b, u64 tick, bool census) {
    u32 i = 0;
    while (i < used_.size() && used_[i])
        ++i;
    if (i == used_.size()) {
        live_.resize(i + 1);
        used_.push(0);
    }
    ProfileSample& x = live_[i];
    memset(&x, 0, sizeof x);
    const Body& B = s.body(b);
    const Block& K = s.block(B.blocks);
    x.serial = B.serial;
    x.start = x.end = tick;
    x.node = node_;
    x.body = b;
    x.block = B.blocks;
    x.gstart = K.start;
    x.glen = K.len;
    x.genotype = genotype_hash(s.mem() + K.start, K.len);
    const BlockTag t = tag_of(s, b);
    x.lineage = t.lineage;
    x.depth = t.depth;
    if (t.lineage) {
        const LineageInfo* L = s.observer()->lineage(t.lineage);
        x.founder_origin = L ? L->founder_origin : kOriginUnknown;
    }
    x.origin = s.origin(b);
    x.form = B.form;
    holdings(s, x);
    // Kin clustering: the node's census (this round's candidates), and the
    // nearest neighbour on each side in memory.
    if (census) {
        x.node_bodies = static_cast<u32>(cand_.size());
        for (usize j = 0; j < cand_.size(); ++j) {
            x.lin_alive += x.lineage && clin_[j] == x.lineage ? 1u : 0u;
            x.geno_alive += cgeno_[j] == x.genotype ? 1u : 0u;
        }
        const u32 nb[2] = {neighbour(s, b, static_cast<i64>(K.start) - 1, -1),
                           neighbour(s, b, static_cast<i64>(K.start) + K.len, 1)};
        for (u32 side = 0; side < 2; ++side) {
            u32 rel = 0;
            if (nb[side] != kNone) {
                rel = kNbFound;
                const u64 l = tag_of(s, nb[side]).lineage;
                if (x.lineage && l == x.lineage)
                    rel |= kNbLineage;
                if (genome_hash(s, nb[side]) == x.genotype)
                    rel |= kNbGenotype;
            }
            (side ? x.nb_right : x.nb_left) = rel;
        }
    }
    if (slot_.size() <= b) {
        const usize old = slot_.size();
        slot_.resize(static_cast<usize>(b) + 1);
        for (usize j = old; j < slot_.size(); ++j)
            slot_[j] = kNone;
    }
    slot_[b] = i;
    used_[i] = 1;
    ++nactive_;
}

void Profiles::finish(u32 i, u32 why, u64 end) {
    ProfileSample& x = live_[i];
    x.why = why;
    x.end = end;
    x.lt_wait = 0;
    slot_[x.body] = kNone;
    done_.push(x);
    used_[i] = 0;
    --nactive_;
}

void Profiles::on_tick_start(const Soup& s, u64 tick) {
    if (!p_ || !p_->enabled || tick % p_->every != 0)
        return;
    cand_.clear();
    for (usize k = 0; k < s.body_count(); ++k) {
        const u32 b = s.body_at(k);
        const Body& B = s.body(b);
        if (B.alive && !s.dormant(b) && B.blocks != kNone)
            cand_.push(b);
    }
    if (cand_.empty())
        return;
    const usize n = cand_.size();
    clin_.resize(n);
    cgeno_.resize(n);
    for (usize j = 0; j < n; ++j) {
        clin_[j] = tag_of(s, cand_[j]).lineage;
        cgeno_[j] = genome_hash(s, cand_[j]);
    }
    // The picks: a partial Fisher-Yates shuffle of the candidates' places,
    // from the observer's own stream.
    Rng r(seed_ ^ kProfileSalt, node_, tick);
    Array<u32> order;
    order.resize(n);
    for (usize j = 0; j < n; ++j)
        order[j] = static_cast<u32>(j);
    const usize m = p_->per_node < n ? p_->per_node : n;
    for (usize j = 0; j < m; ++j) {
        const usize k = j + static_cast<usize>(r.below(n - j));
        const u32 t = order[j];
        order[j] = order[k];
        order[k] = t;
        const u32 b = cand_[order[j]];
        if (!slot_of(b))
            start(s, b, tick, true);
    }
}

void Profiles::on_tick_end(const Soup& s, u64 tick) {
    if (nactive_ == 0)
        return;
    for (u32 i = 0; i < used_.size(); ++i) {
        if (!used_[i])
            continue;
        ProfileSample& x = live_[i];
        const Body& B = s.body(x.body);
        if (!B.alive || B.serial != x.serial) {
            finish(i, kProfileGone, tick);
            continue;
        }
        if (s.dormant(x.body)) { // (going dormant ends a sample at once: on_dormant)
            finish(i, kProfileDormant, tick);
            continue;
        }
        x.end = tick;
        holdings(s, x);
        if (x.lineage == 0) { // a spark that founded a lineage, an INJECT victim taken in ...
            const BlockTag t = tag_of(s, x.body);
            x.lineage = t.lineage;
            x.depth = t.depth;
            if (t.lineage) {
                const LineageInfo* L = s.observer()->lineage(t.lineage);
                x.founder_origin = L ? L->founder_origin : kOriginUnknown;
            }
        }
        // (Warps' lanes aren't marked in the bitmap: where they stand is only on
        // the host when they didn't run on the GPU, which warp.device = auto
        // decides by timing, so marking them made the file depend on the
        // machine and the threads. Their work is lane_instr.)
        if (tick + 1 >= x.start + (p_ ? p_->ticks : 1))
            finish(i, kProfileDone, tick);
    }
}

void Profiles::on_dormant(const Soup& s, u32 b) {
    ProfileSample* x = watching(s, b);
    if (!x)
        return;
    holdings(s, *x);
    ++x->act[kPaDormant];
    finish(slot_[b], kProfileDormant, s.current_tick());
}

bool Profiles::follow(const Soup& s, u32 b, u64 tick) {
    const Body& B = s.body(b);
    if (!B.alive || s.dormant(b) || B.blocks == kNone || slot_of(b))
        return false;
    cand_.clear();
    start(s, b, tick, false);
    return true;
}

void Profiles::finish_all(u64 tick) {
    for (u32 i = 0; i < used_.size(); ++i)
        if (used_[i])
            finish(i, kProfileDone, tick);
}

// --- profiles.csv and arrivals.csv ----------------------------------------------------

namespace {

// The table's distinct ops, in codon order.
u32 table_ops(const CodonTable& t, Op* out) {
    u32 n = 0;
    for (u32 c = 0; c < 64; ++c) {
        bool seen = false;
        for (u32 j = 0; j < n && !seen; ++j)
            seen = out[j] == t.op[c];
        if (!seen)
            out[n++] = t.op[c];
    }
    return n;
}

const char* form_name(u32 f) { return f == kFormGpu ? "gpu" : "cpu"; }

} // namespace

void profile_csv_header(const CodonTable& t, TextBuf& out) {
    out.append("tick,end_tick,ticks,node,body,serial,form,origin,lineage,depth,founder_origin,genotype,genome_len,"
               "end,role,instr,lane_instr,foreign,faults,work_len,distinct_ops,action_kinds");
    for (u32 a = 0; a < kPaCount; ++a) {
        out.append_char(',');
        out.append(kActionNames[a]);
    }
    out.append(",heard_abs,lta_zero_fell,lta_zero_other,lta_pos_fell,lta_pos_other,lta_neg_fell,lta_neg_other");
    for (u32 h = 0; h < kHoldCount; ++h) {
        out.append_char(',');
        out.append(kHoldNames[h]);
    }
    out.append(",node_bodies,lin_alive,geno_alive,nb_left,nb_right");
    Op ops[64];
    const u32 n = table_ops(t, ops);
    for (u32 i = 0; i < n; ++i) {
        out.append(",op_");
        out.append(op_name(ops[i]));
    }
    out.append(",codons\n");
}

void profile_csv_row(const CodonTable& t, const ProfileSample& s, TextBuf& out) {
    auto num = [&out](u64 v) {
        out.append_char(',');
        out.append_u64(v);
    };
    out.append_u64(s.start);
    num(s.end);
    num(s.end - s.start + 1);
    num(s.node);
    num(s.body);
    num(s.serial);
    out.append_char(',');
    out.append(form_name(s.form));
    out.append_char(',');
    out.append(origin_name(s.origin));
    out.append_char(',');
    append_lineage(out, s.lineage);
    num(s.depth);
    out.append_char(',');
    out.append(origin_name(s.founder_origin));
    out.append_char(',');
    out.append_hex64(s.genotype);
    num(s.glen);
    out.append_char(',');
    out.append(profile_end_name(s.why));
    out.append_char(',');
    out.append(role_name(role_by_rules(s.act)));
    num(s.instr);
    num(s.lane_instr);
    num(s.foreign);
    num(s.faults);
    num(profile_working_length(s));
    num(profile_distinct_ops(s));
    num(action_kinds(s.act));
    for (u32 a = 0; a < kPaCount; ++a)
        num(s.act[a]);
    num(s.heard_abs);
    for (u32 i = 0; i < 6; ++i)
        num(s.lta[i]);
    for (u32 h = 0; h < kHoldCount; ++h)
        num(s.hold[h]);
    num(s.node_bodies);
    num(s.lin_alive);
    num(s.geno_alive);
    num(s.nb_left);
    num(s.nb_right);
    Op ops[64];
    const u32 n = table_ops(t, ops);
    for (u32 i = 0; i < n; ++i)
        num(s.ops[static_cast<u32>(ops[i])]);
    // The bitmap as hex: 4 codons a digit, codon 4j + k in the digit's bit k.
    out.append_char(',');
    const u32 len = s.glen < kProfileGenomeMax ? s.glen : kProfileGenomeMax;
    for (u32 j = 0; j < len; j += 4) {
        u32 v = 0;
        for (u32 k = 0; k < 4 && j + k < len; ++k)
            v |= ((s.bits[(j + k) >> 3] >> ((j + k) & 7)) & 1u) << k;
        out.append_char("0123456789abcdef"[v]);
    }
    if (len == 0)
        out.append_char('-');
    out.append_char('\n');
}

void arrivals_csv_header(TextBuf& out) { out.append("tick,node,src,lineage,genotype,len,dormant\n"); }

void arrivals_csv_row(const ArrivalRecord& a, TextBuf& out) {
    out.append_u64(a.tick);
    out.append_char(',');
    out.append_u64(a.node);
    out.append_char(',');
    out.append_u64(a.src);
    out.append_char(',');
    append_lineage(out, a.lineage);
    out.append_char(',');
    out.append_hex64(a.genotype);
    out.append_char(',');
    out.append_u64(a.len);
    out.append_char(',');
    out.append_u64(a.dormant);
    out.append_char('\n');
}

} // namespace nova
