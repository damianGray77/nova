// The interpreter: what each Op does (design.md 6.2 and 6.5).
//
// Exec runs one CPU on a local copy of its state and writes it back at the
// end. A copy, not a reference, because DIVIDE appends to the CPU and body
// arrays and may move them.
//
// Every instruction charges at least 1 against the CPU's quota (ENGULF and
// INJECT charge more) and sets the error flag to whether it failed; K
// consecutive failures halt the CPU (6.4 #1).
//
// Phase 9 (design.md 6.15): with a timed cost table the charge is world time
// in picoseconds instead: the instruction's own part (cost.h) plus what it
// does as it runs (t_ps: template codons read, codons searched, a jump taken,
// a kernel call for a world change, codons moved, the disk). ENGULF, INJECT
// and any instruction longer than a tick become actions in progress: started
// here, the CPU then waits, and they land or fail at their end (run_cpu).
#include <cmath>   // log
#include <cstring> // memcpy

#include "nova/lineage.h"
#include "nova/profile.h" // behaviour profiles (A3): a followed thread's instructions, observe-only
#include "nova/soup.h"

namespace nova {

struct Exec {
    Soup& s;
    const SoupParams& p;
    Cpu c;
    float errm; // the body's hazard multiplier this tick
    u32 ci;     // this CPU's slot (kNone for a warp's host instruction)
    bool warp = false; // Phase 8: a host instruction for a warp (c is its lowest lane)
    // Phase 9, timed cost tables: this instruction's picoseconds beyond its
    // own part; the action it finishes now (run_cpu), or whether it started one.
    mutable u64 t_ps = 0; // (searches and template reads are const)
    const Action* resolving = nullptr;
    bool deferred = false;
    u32 wslot = kNone; // Phase 9b: the warp, for its host instruction
    // Gestation (docs/lifehistory-plan.md G1): a DIVIDE before the daughter is
    // ripe waits at itself, idle; ripe_at is the tick she will be.
    bool stalled = false;
    u64 ripe_at = 0;
    // Phase 9c: the costs of the kind of core it runs on (nullptr: a P-core's, the table itself).
    const CostTable* ct = nullptr;
    // --- behaviour profiles (docs/analysis-plan.md A3; profile.h) ------------
    // The sample following this thread's body, or nullptr; step<true> records
    // each instruction into it. self_load: the last DISK load wrote into the
    // block the thread runs in. Read only by the sample: nothing in the world
    // depends on either.
    ProfileSample* prof = nullptr;
    bool self_load = false;
    // --- end behaviour profiles ------------------------------------------------
    const CostTable& costs() const { return ct ? *ct : p.cost; }
    // Whether an instruction starts an action rather than happening at once.
    bool acts() const { return p.cost.timed && !resolving && (warp ? wslot != kNone : ci != kNone); }
    // Who acts: a CPU's slot, or a warp's (Phase 9b).
    void actor_of(Action* a) const {
        a->cpu = warp ? wslot : ci;
        a->warp = warp ? 1u : 0u;
    }
    // A daughter's lanes (docs/lanes-plan.md L2): the dividing warp's; 0 for
    // a CPU's (a GPU-born daughter of a CPU fits her code).
    u32 divider_lanes() const { return warp && wslot != kNone ? s.warps_[wslot].lanes : 0u; }

    // --- memory tiers (docs/world-plan.md W3) ---------------------------------
    // With a timed table and fast memory on the node (SoupParams::fast_end),
    // every load, store, template scan and bulk move costs its codon's tier's
    // price: the fast tier the table's own (the nearest cache's) / the node's
    // fast-memory speed, the slow tier the table's x load_far / load (one
    // cache level out) / its slow-memory speed. Those picoseconds gather in
    // t_mem and are rounded once an instruction. Without tiers (or classic)
    // every price goes into t_ps as before. tiers_setup() runs once the core's
    // table (ct) is known.
    bool tiered = false;
    double tf[2] = {1.0, 1.0}; // per tier (0 fast, 1 slow): the factor on a table price
    mutable double t_mem = 0;  // this instruction's tiered picoseconds (searches are const)
    u32 moved_fast = 0;        // fast codons among the last bulk move (ENGULF, bites, INJECT)
    void tiers_setup() {
        tiered = p.cost.timed && p.fast_end > 0;
        if (!tiered)
            return;
        const CostTable& t = costs();
        tf[0] = 1.0 / p.mem_fast_speed;
        tf[1] = static_cast<double>(t.load_far) / static_cast<double>(t.load) / p.mem_slow_speed;
    }
    bool fast(u32 a) const { return a < p.fast_end; }
    // n codons of a table operation ps at address a's tier.
    void charge(u64 ps, u32 a, u64 n = 1) const {
        if (!tiered) {
            t_ps += n * ps;
            return;
        }
        t_mem += static_cast<double>(n * ps) * tf[fast(a) ? 0 : 1];
    }
    // nf fast and ns slow codons of ps.
    void charge2(u64 ps, u64 nf, u64 ns) const {
        if (!tiered) {
            t_ps += (nf + ns) * ps;
            return;
        }
        t_mem += static_cast<double>(nf * ps) * tf[0] + static_cast<double>(ns * ps) * tf[1];
    }
    // The fast codons of [start, start + len) (a block's range: no wrap).
    u32 fast_in(u32 start, u32 len) const {
        if (start >= p.fast_end)
            return 0;
        return min_of<u32>(len, p.fast_end - start);
    }
    void charge_range(u64 ps, u32 start, u32 len) const {
        const u32 nf = tiered ? fast_in(start, len) : 0;
        charge2(ps, nf, len - nf);
    }
    // Block k's (a body's) codons still owned, in fast memory.
    u32 block_fast_owned(u32 k) const {
        const Block& bl = s.blocks_[k];
        if (bl.start >= p.fast_end)
            return 0;
        if (bl.start + bl.len <= p.fast_end)
            return bl.owned;
        u32 n = 0;
        for (u32 a = bl.start; a < p.fast_end; ++a)
            n += s.owner_[a] == k ? 1u : 0u;
        return n;
    }
    u32 fast_owned(u32 body) const {
        u32 n = 0;
        for (u32 k = s.bodies_[body].blocks; k != kNone; k = s.blocks_[k].next)
            n += block_fast_owned(k);
        return n;
    }
    // This instruction's picoseconds beyond its own part: t_ps and t_mem, rounded.
    u64 extra_ps() const { return t_ps + (tiered ? static_cast<u64>(t_mem + 0.5) : 0); }
    // An instruction's own part. Those that read or write memory through a
    // head (COPY, READ, WRITE, MOVII) at their codons' tiers, from the
    // registers before they run, into t_mem (0 then stands for the part).
    u64 own_part(Op op) const {
        const CostTable& t = costs();
        if (!tiered)
            return t.op[static_cast<u32>(op)];
        switch (op) {
        case Op::copy:
            charge(t.load, c.rh);
            charge(t.store, c.wh);
            return 0;
        case Op::read:
            charge(t.load, c.rh);
            return 0;
        case Op::write:
            charge(t.store, c.wh);
            return 0;
        case Op::t_movii:
            charge(t.load, c.bx % p.size);
            charge(t.store, c.ax % p.size);
            return 0;
        default:
            return t.op[static_cast<u32>(op)];
        }
    }
    // --- end memory tiers -------------------------------------------------------

    // --- addresses -----------------------------------------------------------

    // Yours: your body's, or, for a parasite, its host's too (docs/inject-plan.md M2).
    bool own(u32 a) const { return s.mine(c.body, a); }
    // Private code: read your own body or waste. Open membrane: anything.
    bool readable(u32 a) const {
        if (p.open_membrane)
            return true;
        return s.owner_[a] == kNone || s.mine(c.body, a);
    }
    // Writes go only into your own body (its daughter blocks included).
    bool writable(u32 a) const { return own(a); }

    // The address after a: wraps at the end of the block a belongs to (or, for
    // a lost codon, the block given as fallback), else at the soup's end. Open
    // membrane mode is Tierra's circular soup: no block wrap.
    u32 next_addr(u32 a, u32 fallback = kNone) const {
        if (!p.open_membrane) {
            u32 k = s.owner_[a];
            if (k == kNone)
                k = fallback;
            if (k != kNone) {
                const Block& b = s.blocks_[k];
                if (a >= b.start && a + 1 == b.start + b.len)
                    return b.start;
            }
        }
        return a + 1 == p.size ? 0 : a + 1;
    }
    u32 next_ip(u32 a) const { return next_addr(a, c.block); }
    // The address before a (a reversed copy's read head): the mirror of
    // next_addr, wrapping to the end of a's block, else of the soup.
    u32 prev_addr(u32 a) const {
        if (!p.open_membrane) {
            const u32 k = s.owner_[a];
            if (k != kNone) {
                const Block& b = s.blocks_[k];
                if (a == b.start && b.len > 0)
                    return b.start + b.len - 1;
            }
        }
        return a == 0 ? p.size - 1 : a - 1;
    }
    // The address after an instruction at a and its n-codon template.
    u32 skip(u32 a, u32 n) const {
        for (u32 i = 0; i <= n; ++i)
            a = next_ip(a);
        return a;
    }

    // --- templates -------------------------------------------------------------

    // The run of NOPs starting at `from` (at most max_template), as bits.
    u32 read_template(u32 from, u8* bits) const {
        u32 n = 0, nf = 0; // (nf: of them in fast memory, W3)
        u32 a = from;
        while (n < p.max_template) {
            if (!p.open_membrane && !own(a))
                break;
            Op op = p.table.op[s.mem_[a]];
            if (!is_nop(op))
                break;
            bits[n++] = op == Op::nop1;
            nf += fast(a) ? 1u : 0u;
            a = next_ip(a);
        }
        charge2(costs().load, nf, n - nf); // Phase 9: the template's codons, read (W3: each at its tier)
        return n;
    }

    bool complement_at(u32 start, const u8* bits, u32 n) const {
        for (u32 i = 0; i < n; ++i) {
            u32 a = start + i;
            if (a >= p.size)
                a -= p.size;
            if (!p.open_membrane && !own(a))
                return false;
            Op op = p.table.op[s.mem_[a]];
            if (!is_nop(op) || static_cast<u8>(op == Op::nop1) == bits[i])
                return false;
        }
        return true;
    }

    // Searches for the complement of an n-codon template that follows the
    // instruction at a. dir: 0 outward (forward first at each distance), 1
    // forward only, -1 backward only. Candidate windows start right after the
    // template (forward) or end right before the instruction (backward). In
    // private mode a match must lie in the searcher's own body. Returns the
    // address after the match, or kNone.
    u32 search(u32 a, const u8* bits, u32 n, int dir) const {
        const i64 size = p.size;
        // Under private code a match lies inside the searcher's own blocks, so
        // the scan stops where the body's span ends: same result, far fewer
        // windows for the small bodies of a random soup.
        i64 fmax = p.search_limit, bmax = p.search_limit;
        if (!p.open_membrane) {
            i64 lo = size, hi = 0;
            for (u32 k = s.bodies_[c.body].blocks; k != kNone; k = s.blocks_[k].next) {
                lo = min_of<i64>(lo, s.blocks_[k].start);
                hi = max_of<i64>(hi, static_cast<i64>(s.blocks_[k].start) + s.blocks_[k].len);
            }
            if (s.is_parasite(c.body)) // (and its host's, whose codons are its too: M2)
                for (u32 k = s.bodies_[s.host_of(c.body)].blocks; k != kNone; k = s.blocks_[k].next) {
                    lo = min_of<i64>(lo, s.blocks_[k].start);
                    hi = max_of<i64>(hi, static_cast<i64>(s.blocks_[k].start) + s.blocks_[k].len);
                }
            fmax = min_of<i64>(fmax, hi - n - static_cast<i64>(a) - n);
            bmax = min_of<i64>(bmax, static_cast<i64>(a) - n + 1 - lo);
        }
        const i64 dmax = max_of<i64>(dir >= 0 ? fmax : 0, dir <= 0 ? bmax : 0);
        u64 seen = 0, seen_f = 0; // Phase 9: places looked at, each a codon searched (W3: of them fast)
        for (i64 d = 1; d <= dmax; ++d) {
            if (dir >= 0 && d <= fmax) {
                i64 st = static_cast<i64>(a) + n + d;
                if (p.open_membrane)
                    st %= size;
                if (st + n <= size || p.open_membrane) {
                    ++seen;
                    seen_f += fast(static_cast<u32>(st)) ? 1 : 0;
                    if (complement_at(static_cast<u32>(st), bits, n)) {
                        charge2(costs().scan, seen_f, seen - seen_f);
                        return next_addr(static_cast<u32>((st + n - 1) % size));
                    }
                }
            }
            if (dir <= 0 && d <= bmax) {
                i64 st = static_cast<i64>(a) - n - d + 1;
                if (p.open_membrane)
                    st = ((st % size) + size) % size;
                if (st >= 0) {
                    ++seen;
                    seen_f += fast(static_cast<u32>(st)) ? 1 : 0;
                    if (complement_at(static_cast<u32>(st), bits, n)) {
                        charge2(costs().scan, seen_f, seen - seen_f);
                        return next_addr(static_cast<u32>((st + n - 1) % size));
                    }
                }
            }
        }
        charge2(costs().scan, seen_f, seen - seen_f);
        return kNone;
    }

    // A template jump: on success *next is the landing address; on failure it
    // skips the template and returns false.
    bool jump(u32 ip, int dir, u32* next) {
        u8 bits[64];
        u32 n = read_template(next_ip(ip), bits);
        u32 after = skip(ip, n);
        if (n == 0) {
            *next = after;
            return false; // no template, no target: zero means nothing
        }
        u32 to = search(ip, bits, n, dir);
        *next = to == kNone ? after : to;
        if (to != kNone)
            t_ps += costs().jump; // Phase 9: a jump taken
        return to != kNone;
    }

    // --- stack ---------------------------------------------------------------

    bool push(u32 v) {
        if (c.sp >= p.stack_depth)
            return false;
        c.stack[c.sp++] = v;
        return true;
    }
    bool pop(u32* v) {
        if (c.sp == 0)
            return false;
        *v = c.stack[--c.sp];
        return true;
    }

    // --- memory traffic ------------------------------------------------------

    // A write into a page past its `hot` onset goes through only with the
    // page's speed factor as probability: memory throttles by refusing work.
    bool page_accepts(u32 a) {
        if (!p.heat_on)
            return true;
        float h = s.page_heat_[a >> kPageBits];
        if (h <= p.mem.hot)
            return true;
        return s.rng_.unit() < p.mem.speed(h);
    }

    // Codon a was written (it held `was`).
    void note_write(u32 a, u8 was) {
        if (p.heat_on)
            ++s.page_write_[a >> kPageBits];
        if (!s.stamp_.empty()) // written: now the writer's own code
            s.stamp_[a] = 0;
        if (p.endurance > 0) // the node's wear (Phase 4 lifecycle)
            ++s.writes_;
        // docs/bites-plan.md B5: a NOP written over the outermost codon of an
        // open end of a body's code closes it: a repair.
        if (!is_nop(p.table.op[was]) && is_nop(p.table.op[s.mem_[a]])) {
            const u32 k = s.owner_[a];
            const u32 b = k == kNone ? kNone : s.blocks_[k].body;
            if (b != kNone && s.bodies_[b].blocks == k && (a == s.outer_codon(b, false) || a == s.outer_codon(b, true))) {
                ++s.tally_.repairs;
                if (s.obs_)
                    s.obs_->on_repair(s, k, s.tick_);
            }
        }
    }

    // --- remnants and salvage (Phase 4, Damian) ------------------------------
    //
    // A codon taken through a head carries the stamp of the program whose code
    // it was (0: nobody's). One stamped C > K x my genome length r comes
    // through intact with chance K x r / C, and otherwise as a random codon:
    // a much smaller program gets pieces of a bigger one's code, never all of
    // it. Execution and template searches are not salvage.
    u32 genome() const { return s.genome_len(c.body); }
    u8 salvaged(u8 v, u32 st) {
        if (st == 0)
            return v;
        const double lim = p.salvage_ratio * genome();
        if (st <= lim || s.rng_.unit() < lim / st)
            return v;
        ++s.tally_.garbled;
        return static_cast<u8>(s.rng_.next() & 63);
    }
    u8 take(u32 a) { return s.stamp_.empty() ? s.mem_[a] : salvaged(s.mem_[a], s.stamp_[a]); }
    u8 take_disk(u32 a) {
        return s.disk_.stamp.empty() ? s.disk_.mem[a] : salvaged(s.disk_.mem[a], s.disk_.stamp[a]);
    }

    // A copied codon, with the heat-scaled chance of a one-bit copy error (the
    // multiplier is the body's CPU heat's, or its GPU heat's for GPU copies).
    u8 copied(u8 v) { return copied_with(v, errm); }
    u8 copied_with(u8 v, float mult) {
        s.copy_left_ -= mult;
        if (s.copy_left_ <= 0) {
            v ^= static_cast<u8>(1u << s.rng_.below(6));
            ++s.tally_.copy_errors;
            s.copy_left_ = -log(1.0 - s.rng_.unit()) / (p.copy_rate * s.hazard_mult_);
        }
        return v;
    }

    bool copy() {
        u32 from = c.rh, to = c.wh;
        if (!readable(from) || !writable(to) || !page_accepts(to))
            return false;
        const u8 was_ = s.mem_[to];
        s.mem_[to] = copied(take(from));
        note_write(to, was_);
        if (s.obs_)
            s.obs_->on_write(to);
        c.rh = next_addr(from);
        c.wh = next_addr(to);
        return true;
    }

    // --- resources -----------------------------------------------------------

    // Claims len codons of waste as a new block of this body: the nearest free
    // run outward from the block the CPU runs in (forward on a tie).
    u32 alloc_memory(u32 len) {
        if (s.is_swallowed(c.body)) // docs/inject-plan.md S3: nothing grows inside an eater
            return kNone;
        if (s.is_parasite(c.body)) // M4: a parasite's memory comes out of its host
            return alloc_in_host(len);
        if (s.known_full(len))
            return kNone;
        u32 lo = c.ip, hi = c.ip + 1;
        if (c.block != kNone) {
            lo = s.blocks_[c.block].start;
            hi = lo + s.blocks_[c.block].len;
        }
        u32 f = s.find_free_fwd(hi, len);
        u32 b = s.find_free_bwd(lo, len);
        u32 at = kNone;
        if (f != kNone && b != kNone)
            at = (f - hi) <= (lo - (b + len)) ? f : b;
        else
            at = f != kNone ? f : b;
        if (at == kNone) {
            s.note_full(len); // forward and backward together cover the soup
            return kNone;
        }
        u32 k = s.new_block(at, len, c.body);
        s.bodies_[c.body].latest = k;
        c.wh = at;
        charge_range(costs().move, at, len); // Phase 9: its codons claimed (W3: at their tiers)
        return at;
    }

    // docs/inject-plan.md M4 (Damian: "it still has to copy itself inside of
    // the host program... eventually killing the host"): a parasite's ALLOC
    // carves its block out of its host's first block, from the end away from
    // its own code, never into it; the write head goes to its start, as an
    // ALLOC's does. Fails when the host hasn't that many codons of its own
    // there (outside the parasite's code, none lost).
    u32 alloc_in_host(u32 len) {
        const u32 h = s.host_of(c.body);
        const u32 k = h == kNone ? kNone : s.bodies_[h].blocks;
        if (len == 0 || k == kNone)
            return kNone;
        u32 lo = 0, hi = 0;
        s.parasite_span(c.body, &lo, &hi);
        const u32 S = s.blocks_[k].start, E = S + s.blocks_[k].len;
        if (hi <= lo) // (none of its code left in the host: it may take from either end; the low)
            lo = hi = E;
        const bool from_low = lo - S >= E - hi; // its code nearer the high end: carve the low end
        u32 at = kNone;
        if (from_low && S + len <= lo)
            at = S;
        else if (!from_low && E - hi >= len)
            at = E - len;
        if (at == kNone || len >= s.blocks_[k].len)
            return kNone;
        for (u32 x = at; x < at + len; ++x)
            if (s.owner_[x] != k)
                return kNone;
        const u32 k2 = from_low ? carve_block(k, at + len, false) : carve_block(k, at, true);
        give_block(k2, c.body);
        if (s.obs_) // (to the detector a fresh block: nothing written into it yet, as an ALLOC's)
            s.obs_->on_new_block(k2, at, len);
        if (p.gestation > 0)
            s.blocks_[k2].born = static_cast<u32>(s.tick_);
        c.wh = at;
        charge_range(costs().move, at, len);
        return at;
    }

    // Memory tiers (docs/world-plan.md W3): ALLOC's resource 7 claims fast
    // memory only: the nearest free run inside [0, fast_end), outward from
    // the block the CPU runs in, as alloc_memory finds one anywhere. kNone
    // when there is no room of that size there (which proves nothing about
    // slow memory, so it notes nothing).
    u32 alloc_fast(u32 len) {
        if (s.host_of(c.body) != kNone) // (a guest's memory is its host's: inject-plan M4, S3)
            return alloc_memory(len);
        const u32 fe = p.fast_end;
        if (len > fe || s.known_full(len))
            return kNone;
        u32 lo = c.ip, hi = c.ip + 1;
        if (c.block != kNone) {
            lo = s.blocks_[c.block].start;
            hi = lo + s.blocks_[c.block].len;
        }
        const u32 f = hi < fe ? free_run_fwd(s.used_.data(), fe, hi, len) : kNone;
        const u32 b = s.find_free_bwd(lo < fe ? lo : fe, len);
        u32 at = kNone;
        if (f != kNone && b != kNone)
            at = (f - hi) <= (lo - (b + len)) ? f : b;
        else
            at = f != kNone ? f : b;
        if (at == kNone)
            return kNone;
        u32 k = s.new_block(at, len, c.body);
        s.bodies_[c.body].latest = k;
        c.wh = at;
        charge_range(costs().move, at, len);
        return at;
    }

    void unlink_block(u32 body, u32 k) {
        Body& B = s.bodies_[body];
        if (B.blocks == k) {
            B.blocks = s.blocks_[k].next;
        } else {
            u32 t = B.blocks;
            while (s.blocks_[t].next != k)
                t = s.blocks_[t].next;
            s.blocks_[t].next = s.blocks_[k].next;
        }
        s.blocks_[k].next = kNone;
    }

    void forget(u32 k) {
        s.forget_block(c.body, k);
        if (c.block == k)
            c.block = kNone;
    }

    // DIVIDE (6.1 "DIVIDE mechanics only"): the most recently allocated block
    // becomes the daughter's body, with one CPU at its start, and she gets
    // share/64 of each non-memory holding (rounded down: zero gives nothing).
    // Gestation (G1, G2): whether the body's latest block (the daughter DIVIDE
    // would hand over) is ripe: life.gestation + life.gestation_per_codon x
    // her length ticks since it was claimed (Block.born). With no daughter it
    // is "ripe": DIVIDE then fails as before. Not ripe sets ripe_at.
    bool ripe() {
        const u32 k = s.bodies_[c.body].latest;
        if (k == kNone)
            return true;
        const Block& b = s.blocks_[k];
        const double need = static_cast<double>(p.gestation) + p.gestation_per_codon * static_cast<double>(b.len);
        const double age = static_cast<double>(static_cast<u32>(s.tick_) - b.born);
        if (age >= need)
            return true;
        ripe_at = s.tick_ + static_cast<u64>(ceil(need - age));
        return false;
    }

    // A daughter in block k is divided off (at home or to a neighbour): the
    // detector's link, and a parasite's count (docs/inject-plan.md M4).
    void note_divide(u32 k) {
        const bool parasite = !warp && s.is_parasite(c.body);
        if (parasite)
            ++s.tally_.parasite_divides;
        if (!s.obs_)
            return;
        if (parasite && ci != kNone)
            s.obs_->on_divide_parasite(s, ci, k, s.tick_);
        else
            s.obs_->on_divide(s, c.block, k, s.tick_);
    }

    bool divide(u32 share) {
        u32 parent = c.body;
        u32 k = s.bodies_[parent].latest;
        t_ps += costs().call; // Phase 9: a world change
        if (k == kNone || s.is_swallowed(parent)) // (docs/inject-plan.md S3: nothing is born inside an eater)
            return false;
        // M4 (Damian): a parasite's daughter takes her share of its host's
        // cores, GPU and NPU units, not of what the parasite brought.
        const u32 giver = s.is_parasite(parent) ? s.host_of(parent) : parent;
        charge_range(costs().move, s.blocks_[k].start, s.blocks_[k].len); // her codons handed over (W3: tiers)
        // The detector compares the daughter with the block this CPU runs in
        // (read-only; see lineage.h); a parasite thread's, with its payload,
        // for the injector's lineage (docs/bites-plan.md I5).
        note_divide(k);
        u32 d = s.new_body(); // may move bodies_
        s.origin_[d] = kOriginDaughter; // W7 (detector data)
        unlink_block(parent, k);
        Body& P = s.bodies_[parent];
        Body& D = s.bodies_[d];
        P.latest = kNone;
        P.mem -= s.blocks_[k].owned;
        s.blocks_[k].body = d;
        D.blocks = k;
        D.mem = s.blocks_[k].owned;
        // Phase 9c: a share of each kind of millicore (all of them P without E millicores).
        const Body& G = s.bodies_[giver];
        const u32 pe = s.e_units(giver);
        u32 give = static_cast<u32>(static_cast<u64>(G.units - pe) * share / 64);
        const u32 egive = static_cast<u32>(static_cast<u64>(pe) * share / 64);
        if (!p.warps)
            s.new_cpu(d, s.blocks_[k].start);
        s.move_kind(giver, d, give, false);
        if (egive)
            s.move_kind(giver, d, egive, true);
        if (p.gpus) // GPU and NPU units too: every non-memory holding (Phase 2, 2b)
            s.gpu_.move(giver, d, static_cast<u32>(static_cast<u64>(s.gpu_.count[giver]) * share / 64));
        if (p.npus)
            s.npu_.move(giver, d, static_cast<u32>(static_cast<u64>(s.npu_.count[giver]) * share / 64));
        if (s.disk_on() && !s.disk_.tiered()) // and the newest share of the disk, contents and all (Phase 4)
            s.disk_.move_tail(parent, d, static_cast<u32>(static_cast<u64>(s.disk_.total[parent]) * share / 64));
        if (s.disk_.tiered()) { // W4: the newest share of each tier, each keeping its tier
            const u32 ha = s.disk_.held_in(parent, true), hf = s.disk_.total[parent] - ha;
            s.disk_.move_tiers(parent, d, static_cast<u32>(static_cast<u64>(hf) * share / 64),
                               static_cast<u32>(static_cast<u64>(ha) * share / 64));
        }
        // Phase 8: she is born on what divided her (a warp's daughter as a
        // warp), once she holds her shares; a warp's daughter with its lanes
        // (docs/lanes-plan.md L2).
        if (p.warps)
            s.birth(d, s.blocks_[k].start, warp ? kFormGpu : kFormCpu, divider_lanes());
        // Phase 9c: her thread prefers the kind of core the thread that divided her does.
        if (s.kinds_on() && s.bodies_[d].cpus != kNone)
            s.cpu_kind_[s.bodies_[d].cpus] = warp ? kKindP : s.cpu_kind(ci);
        forget(k);
        ++s.tally_.births;
        return true;
    }

    // DIVIDE to neighbour `dest` (1..degree, nearest first; Phase 3, design.md
    // 6.9): the exchange rule (4.4a E). Her codons become cargo and her block
    // becomes waste, keeping its contents; her share of every non-memory
    // holding goes back into this node's pools (the parent pays at home), and
    // she draws the same amounts at the destination. Fails, changing nothing,
    // past the degree or when the edge already carries its capacity.
    bool disperse(u32 share, u32 dest) {
        const DispersalPorts& P = s.ports_;
        if (dest == 0 || dest > P.degree || s.host_of(c.body) != kNone) // (a guest can't send: inject-plan M8, S3)
            return false;
        const u32 parent = c.body;
        const u32 k = s.bodies_[parent].latest;
        t_ps += costs().call; // Phase 9: a world change
        if (k == kNone)
            return false;
        charge_range(costs().move, s.blocks_[k].start, s.blocks_[k].len); // her codons, packed as cargo (W3)
        const u32 nbr = dest - 1;
        if (P.down && P.down[P.edges[nbr]]) // a failed link (Phase 4)
            return false;
        u32 queued = P.in_flight[P.edges[nbr]];
        for (usize i = 0; i < s.departures_.size(); ++i) // (dormant programs drift outside the capacity)
            queued += s.departures_[i].nbr == nbr && !s.departures_[i].dormant;
        if (queued >= P.capacity)
            return false;
        // The detector sees her birth (and adopts her into the lineage) before
        // she leaves, so her tag can travel with her.
        Departure d = {};
        note_divide(k);
        if (s.obs_)
            d.tag = s.obs_->cargo_tag(k);
        const Block& B = s.blocks_[k];
        d.nbr = nbr;
        d.len = B.len;
        d.form = p.warps && warp ? kFormGpu : kFormCpu; // Phase 8: she travels as what divided her
        d.lanes = divider_lanes(); // ... with its lanes (docs/lanes-plan.md L2)
        d.codon_off = static_cast<u32>(s.departure_codons_.size());
        s.departure_codons_.append(s.mem_.data() + B.start, B.len);
        Body& Pb = s.bodies_[parent];
        const u32 pe = s.e_units(parent); // Phase 9c: each kind's share (P only without E millicores)
        d.millicores = static_cast<u32>(static_cast<u64>(Pb.units - pe) * share / 64);
        d.emillicores = static_cast<u32>(static_cast<u64>(pe) * share / 64);
        s.release_kind(parent, d.millicores, false);
        if (d.emillicores)
            s.release_kind(parent, d.emillicores, true);
        d.kind = warp ? kKindP : s.cpu_kind(ci); // her thread prefers her divider's kind
        if (p.gpus) {
            d.gpus = static_cast<u32>(static_cast<u64>(s.gpu_.count[parent]) * share / 64);
            s.gpu_.release(parent, d.gpus);
        }
        if (p.npus) {
            d.npus = static_cast<u32>(static_cast<u64>(s.npu_.count[parent]) * share / 64);
            s.npu_.release(parent, d.npus);
        }
        if (s.disk_on() && !s.disk_.tiered()) { // released at home, contents left as fossils (Phase 4)
            d.disk = static_cast<u32>(static_cast<u64>(s.disk_.total[parent]) * share / 64);
            s.disk_.release_tail(parent, d.disk, genome());
        }
        if (s.disk_.tiered()) { // W4: each tier's share, released at home and claimed there in its tier
            const u32 ha = s.disk_.held_in(parent, true), hf = s.disk_.total[parent] - ha;
            d.disk = static_cast<u32>(static_cast<u64>(hf) * share / 64);
            d.archive = static_cast<u32>(static_cast<u64>(ha) * share / 64);
            s.disk_.release_tiers(parent, d.disk, d.archive, genome());
        }
        s.stamp_block(k, B.len); // what she leaves behind is her code (Phase 4)
        unlink_block(parent, k);
        Pb.latest = kNone;
        Pb.mem -= s.blocks_[k].owned;
        forget(k);
        s.free_block(k);
        s.departures_.push(d);
        ++s.tally_.sent;
        return true;
    }

    // Disk tiers (docs/world-plan.md W4): a DISK op's cost from `access` (its
    // slowest tier's) and `per` codon (each codon's own tier's) over nf fast
    // and na archive codons -- the archive archive.slow times the fast
    // disk's, each tier's divided by its substrate speed (the node's fast-disk
    // and slow-disk levels, set by World::derive_node: docs/world-plan.md W1).
    double disk_scale(double access, double per, u32 nf, u32 na) const {
        const double ff = 1.0 / p.disk_speed;
        const double fa = p.archive_slow / p.archive_speed;
        const double slowest = na == 0 ? ff : (nf > 0 && ff > fa ? ff : fa);
        return access * slowest + per * (static_cast<double>(nf) * ff + static_cast<double>(na) * fa);
    }

    // DISK (Phase 4, design.md 6.10): BX through the DISK map (0 compare,
    // 1 store, 2 load), AX = offset in my disk, CX = count; the memory side is
    // the read head (compare, store) or the write head (load), advancing as
    // COPY's do, private code applying. All-or-nothing. Costs disk.cost_base +
    // disk.cost_per_codon x CX. Transfers are exact: the disk's hazard is rot.
    // W4: the body's disk is all its extents, whatever their tier; the op
    // costs the slowest tier it touches for access, plus each codon at its
    // own tier's price (disk_scale).
    bool disk_op(u32* cost) {
        if (!s.disk_on() || c.bx >= 3)
            return false;
        const u32 op = p.sel.disk[c.bx];
        const i32 n = static_cast<i32>(c.cx);
        if (n <= 0)
            return false;
        Array<u32>& addr = s.disk_addr_;
        if (!s.disk_.addresses(c.body, c.ax, static_cast<u32>(n), &addr))
            return false;
        u32 a = op == 2 ? c.wh : c.rh;
        for (i32 i = 0; i < n; ++i) { // the memory side, checked before anything moves
            if (op == 2 ? !writable(a) : !readable(a))
                return false;
            a = next_addr(a);
        }
        u32 na = 0; // W4: codons on the archive
        for (i32 i = 0; s.disk_.tiered() && i < n; ++i)
            na += addr[i] >= s.disk_.fast ? 1u : 0u;
        if (!s.disk_.tiered() && p.disk_speed == 1.0) // (exactly as before)
            *cost = max_of<u32>(1, p.disk_cost_base + static_cast<u32>(p.disk_cost_per_codon * n));
        else
            *cost = max_of<u32>(1, static_cast<u32>(min_of(disk_scale(p.disk_cost_base, p.disk_cost_per_codon,
                                                                      static_cast<u32>(n) - na, na),
                                                           4e9)));
        u8* disk = s.disk_.mem.data();
        switch (op) {
        case 0: { // compare: CX <- mismatches
            u32 diff = 0;
            for (i32 i = 0; i < n; ++i) {
                diff += take(c.rh) != take_disk(addr[i]);
                c.rh = next_addr(c.rh);
            }
            c.cx = diff;
            ++s.tally_.disk_compares;
            return true;
        }
        case 1: // store: disk <- memory
            for (i32 i = 0; i < n; ++i) {
                disk[addr[i]] = take(c.rh);
                if (!s.disk_.stamp.empty()) // my own data now
                    s.disk_.stamp[addr[i]] = 0;
                c.rh = next_addr(c.rh);
            }
            if (p.endurance > 0) // disk writes wear the node too
                s.writes_ += static_cast<u32>(n);
            ++s.tally_.disk_stores;
            s.tally_.archive_stores += na ? 1 : 0; // W4
            return true;
        default: { // load: memory <- disk
            bool into_self = false;
            for (i32 i = 0; i < n; ++i) {
                const u32 w = c.wh;
                const u8 was_ = s.mem_[w];
                s.mem_[w] = take_disk(addr[i]); // fossils are salvage
                note_write(w, was_);
                into_self = into_self || (c.block != kNone && s.owner_[w] == c.block);
                if (s.obs_)
                    s.obs_->on_write(w);
                c.wh = next_addr(w);
            }
            ++s.tally_.disk_loads;
            s.tally_.archive_loads += na ? 1 : 0; // W4
            self_load = into_self; // behaviour profiles (A3): a repair, for a followed thread's sample
            if (s.obs_ && into_self)
                s.obs_->on_disk_load(s, c.block, s.tick_);
            return true;
        }
        }
    }

    // ALLOC and RELEASE share one selector map (BX -> resource, D6): 0 memory,
    // 1 CPU, 2 GPU, 3 disk, 4 NPU when the node has one, and 5 E millicores
    // with them (Phase 9c; 1 is then P millicores); 6 the archive with one
    // (docs/world-plan.md W4).
    u32 resource() const { return c.bx < p.sel.res_n ? p.sel.res[c.bx] : 99u; }

    // docs/lifehistory-plan.md V1: YIELD with BX picking 1 (rest here) or 2
    // (drift) through the same map, and CX = N > 0, asks for a sleep of N
    // ticks (capped at dormant.sleep_max); the body goes at the death sweep.
    // Its first such ask this tick holds.
    void ask_sleep() {
        const u32 v = resource();
        if ((v != 1 && v != 2) || c.cx == 0 || c.body == kNone)
            return;
        Array<u32>& q = s.sleep_req_;
        while (q.size() <= c.body)
            q.push(0);
        if (q[c.body] != 0)
            return;
        const u32 n = c.cx < p.sleep_max ? c.cx : p.sleep_max;
        q[c.body] = n | (v == 2 ? Soup::kSleepDrift : 0u);
    }

    // The unit ledger for resource r (2 GPU, 4 NPU), or nullptr if absent.
    UnitLedger* units_of(u32 r) const {
        if (r == 2 && p.gpus)
            return &s.gpu_;
        if (r == 4 && p.npus)
            return &s.npu_;
        return nullptr;
    }

    bool alloc() {
        i32 amount = static_cast<i32>(c.cx);
        const u32 r = resource();
        t_ps += costs().call; // Phase 9: a world change (memory's codons: alloc_memory)
        switch (r) {
        case 0: // memory
            return amount <= 0 || alloc_memory(static_cast<u32>(amount)) != kNone;
        case 1: // CPU, capped by what the pool holds
            return amount <= 0 || s.claim_units(c.body, static_cast<u32>(amount)) > 0;
        case 5: // E millicores (Phase 9c), the same way from theirs
            if (!s.ek())
                return false;
            return amount <= 0 || s.claim_e_units(c.body, static_cast<u32>(amount)) > 0;
        case 2:   // GPU units (Phase 2) and
        case 4: { // NPU units (Phase 2b), capped by their pool; none: fails
            UnitLedger* L = units_of(r);
            if (!L)
                return false;
            return amount <= 0 || L->claim(s.rng_, c.body, static_cast<u32>(amount)) > 0;
        }
        case 3:   // disk (Phase 4): one run of CX codons; AX <- its offset in my disk
        case 6: { // W4: the archive, the same way (its offset in my whole disk)
            if (r == 3 ? !p.disk : !p.archive)
                return false;
            if (amount <= 0)
                return true;
            const u32 off = s.disk_.alloc_run(s.rng_, c.body, static_cast<u32>(amount), r == 6);
            if (off == kNone)
                return false;
            c.ax = off;
            return true;
        }
        case 7: { // memory tiers (W3): fast memory only, one run of CX codons there
            if (amount <= 0)
                return true;
            const bool got = alloc_fast(static_cast<u32>(amount)) != kNone;
            ++(got ? s.tally_.fast_allocs : s.tally_.fast_alloc_fails);
            return got;
        }
        default:
            return false;
        }
    }

    bool release() {
        i32 amount = static_cast<i32>(c.cx);
        t_ps += costs().call; // Phase 9: a world change
        switch (resource()) {
        case 0:   // memory: the end of the latest block
        case 7: { // memory tiers (W3): fast memory is memory, one address space: the same
            u32 k = s.bodies_[c.body].latest;
            if (k == kNone)
                return false;
            if (amount <= 0)
                return true;
            Block& bl = s.blocks_[k];
            { // its codons given up (W3: at their tiers)
                const u32 m = amount < static_cast<i32>(bl.len) ? static_cast<u32>(amount) : bl.len;
                charge_range(costs().move, bl.start + bl.len - m, m);
            }
            if (static_cast<u32>(amount) >= bl.len) {
                s.stamp_block(k, genome()); // left behind as my remnants (Phase 4)
                s.bodies_[c.body].mem -= bl.owned;
                s.bodies_[c.body].latest = kNone;
                unlink_block(c.body, k);
                forget(k);
                s.free_block(k);
                return true;
            }
            u32 end = bl.start + bl.len;
            ++s.free_epoch_;
            for (u32 a = end - static_cast<u32>(amount); a < end; ++a) {
                if (s.owner_[a] != k)
                    continue;
                if (!s.stamp_.empty() && s.stamp_[a] == 0)
                    s.stamp_[a] = genome();
                s.owner_[a] = kNone;
                s.used_[a >> 6] &= ~(1ull << (a & 63));
                ++s.waste_;
                --bl.owned;
                --s.bodies_[c.body].mem;
            }
            bl.len -= static_cast<u32>(amount);
            return true;
        }
        case 1: { // CPU units, oldest first; they keep their heat
            if (amount <= 0)
                return true;
            if (s.bodies_[c.body].units - s.e_units(c.body) == 0)
                return false;
            s.release_kind(c.body, static_cast<u32>(amount), false);
            return true;
        }
        case 5: // E millicores (Phase 9c), the same way
            if (!s.ek())
                return false;
            if (amount <= 0)
                return true;
            if (s.e_units(c.body) == 0)
                return false;
            s.release_kind(c.body, static_cast<u32>(amount), true);
            return true;
        case 2:   // GPU and
        case 4: { // NPU units, the same way (none: fails)
            UnitLedger* L = units_of(resource());
            if (!L)
                return false;
            if (amount <= 0)
                return true;
            if (L->count[c.body] == 0)
                return false;
            L->release(c.body, static_cast<u32>(amount));
            return true;
        }
        case 3: // disk (Phase 4): the newest CX codons; their contents stay
            if (!p.disk)
                return false;
            if (amount <= 0)
                return true;
            if (s.disk_.tiered()) { // W4: the newest CX codons of my fast disk
                if (s.disk_.held_in(c.body, false) == 0)
                    return false;
                s.disk_.release_tiers(c.body, static_cast<u32>(amount), 0, genome());
                return true;
            }
            if (s.disk_.total[c.body] == 0)
                return false;
            s.disk_.release_tail(c.body, static_cast<u32>(amount), genome());
            return true;
        case 6: // W4: the archive, the newest CX codons of mine there
            if (!p.archive)
                return false;
            if (amount <= 0)
                return true;
            if (s.disk_.held_in(c.body, true) == 0)
                return false;
            s.disk_.release_tiers(c.body, 0, static_cast<u32>(amount), genome());
            return true;
        default:
            return false;
        }
    }

    // SENSE: BX through the selector map (D6) to a reading. The readings
    // that depend on where the CPU runs are here; the body's and the node's
    // are Soup::sense_reading, which warps share (Phase 8).
    bool sense() {
        if (c.bx >= p.sel.sense_n) // 8 readings, 11 with a GPU, 14 with an NPU ...
            return false;
        switch (p.sel.sense[c.bx]) {
        case 0: // my size: the block I run in
            c.cx = c.block == kNone ? 0 : s.blocks_[c.block].len;
            if (s.is_parasite(c.body) && c.block != kNone && c.block == s.bodies_[s.host_of(c.body)].blocks) {
                u32 lo = 0, hi = 0; // (in its host: its own code's size, inject-plan M2)
                s.parasite_span(c.body, &lo, &hi);
                c.cx = hi - lo;
            }
            return true;
        case 4: // heat of the memory page I run in
            c.cx = static_cast<u32>(s.page_heat_[c.ip >> kPageBits]);
            return true;
        case 20: // Phase 8: my lane (a CPU is lane 0 of 1)
            c.cx = 0;
            return true;
        case 21: // lanes in my warp
            c.cx = 1;
            return true;
        default: {
            u32 v = 0;
            if (!s.sense_reading(c.body, p.sel.sense[c.bx], ci, &v))
                return false;
            c.cx = v;
            return true;
        }
        }
    }

    // --- contact: ENGULF and INJECT (6.1, 6.4 #3, #6-9) ----------------------

    // Reads a body's surface: the leading NOPs of its first block.
    u32 surface(u32 body, u8* bits) const {
        u32 k = s.bodies_[body].blocks;
        if (k == kNone)
            return 0;
        const Block& bl = s.blocks_[k];
        u32 n = 0;
        for (u32 a = bl.start; a < bl.start + bl.len && n < p.max_template; ++a) {
            if (s.owner_[a] != k)
                break;
            Op op = p.table.op[s.mem_[a]];
            if (!is_nop(op))
                break;
            bits[n++] = op == Op::nop1;
        }
        return n;
    }

    // The graded surface-match contest. Picks the best-matching surface within
    // contact range (then the nearest, then the oldest) and rolls for success
    // with probability matches / max(n, m). For ENGULF 0/0 counts as 1: a
    // surface shields against blind attacks but can be aimed at. For INJECT
    // with contact.inject_key, 0/0 counts as 0: an injector needs a key (the
    // template after it) and a host with a surface it complements, so hosts
    // differ in which injectors they are weak against, and a surfaceless body is
    // immune. Returns the target body on success, kNone on failure.
    u32 contest(u32 ip, const u8* bits, u32 n, bool blind_wins) {
        u32 best_match = 0, best_len = 0;
        const u32 best = contest_pick(ip, bits, n, blind_wins, &best_match, &best_len);
        if (best == kNone)
            return kNone;
        bool win = best_len == 0 ? blind_wins : s.rng_.below(best_len) < best_match;
        return win ? best : kNone;
    }

    // The contest's aim: the best-matching surface in range (then the
    // nearest, then the oldest), its matching bits and length; kNone if none.
    u32 contest_pick(u32 ip, const u8* bits, u32 n, bool blind_wins, u32* pmatch, u32* plen) {
        const u64 blind = blind_wins ? 1 : 0;
        u32 best = kNone, best_match = 0, best_len = 0, best_dist = 0;
        u8 sb[64];
        u64 seen = 0, seen_f = 0; // Phase 9: surfaces looked at (W3: of them in fast memory)
        for (usize i = 0; i < s.order_.size(); ++i) {
            u32 t = s.order_[i];
            if (t == c.body || !s.bodies_[t].alive || s.bodies_[t].blocks == kNone || s.host_of(t) != kNone)
                continue;
            u32 start = s.blocks_[s.bodies_[t].blocks].start;
            u32 dist = start > ip ? start - ip : ip - start;
            if (dist > p.contact_range)
                continue;
            ++seen;
            seen_f += fast(start) ? 1 : 0;
            u32 m = s.dormant(t) ? 0 : surface(t, sb); // a resting dormant program can't shield itself
            u32 len = n > m ? n : m;
            u32 match = 0;
            for (u32 j = 0; j < n && j < m; ++j)
                match += bits[j] != sb[j];
            // Score match/len, with 0/0 = 1/1 (or 0/1 for a keyed INJECT).
            u64 num = len ? match : blind, den = len ? len : 1;
            u64 bnum = best_len ? best_match : blind, bden = best_len ? best_len : 1;
            bool better = best == kNone || num * bden > bnum * den ||
                          (num * bden == bnum * den && dist < best_dist);
            if (better) {
                best = t;
                best_match = match;
                best_len = len;
                best_dist = dist;
            }
        }
        charge2(costs().scan, seen_f, seen - seen_f);
        *pmatch = best_match;
        *plen = best_len;
        return best;
    }

    void engulf_body(u32 prey) {
        u32 eater = c.body;
        moved_fast = tiered ? fast_owned(prey) : 0; // W3: the prey's codons in fast memory, its moves' tier
        // The prey's code stays the prey's (Phase 4 salvage): stamped with its
        // genome length before its blocks become the eater's.
        if (!s.stamp_.empty()) {
            const u32 g = s.genome_len(prey);
            for (u32 k = s.bodies_[prey].blocks; k != kNone; k = s.blocks_[k].next)
                s.stamp_block(k, g);
        }
        Body& P = s.bodies_[prey];
        Body& E = s.bodies_[eater];
        // Blocks: appended after the eater's own, now owned by the eater.
        u32 tail = E.blocks;
        while (s.blocks_[tail].next != kNone)
            tail = s.blocks_[tail].next;
        s.blocks_[tail].next = P.blocks;
        for (u32 k = P.blocks; k != kNone; k = s.blocks_[k].next)
            s.blocks_[k].body = eater;
        E.mem += P.mem;
        // CPUs keep running, now as the eater's (engulf rule 1).
        if (P.cpus != kNone) {
            for (u32 x = P.cpus; x != kNone; x = s.cpus_[x].next)
                s.cpus_[x].body = eater;
            if (E.cpus == kNone) { // Phase 8: an eater born on a GPU has none of its own
                E.cpus = P.cpus;
            } else {
                u32 t = E.cpus;
                while (s.cpus_[t].next != kNone)
                    t = s.cpus_[t].next;
                s.cpus_[t].next = P.cpus;
            }
            E.ncpu += P.ncpu;
        }
        // Holdings: every millicore, with its heat.
        if (P.unit_head != kNone) {
            for (u32 u = P.unit_head; u != kNone; u = s.unit_next_[u])
                s.unit_owner_[u] = eater;
            if (E.unit_tail != kNone) {
                s.unit_next_[E.unit_tail] = P.unit_head;
                s.unit_prev_[P.unit_head] = E.unit_tail;
            } else {
                E.unit_head = P.unit_head;
            }
            E.unit_tail = P.unit_tail;
            E.units += P.units;
            if (s.ek()) { // Phase 9c: E millicores among them
                s.eunits_[eater] += s.eunits_[prey];
                s.eunits_[prey] = 0;
            }
        }
        if (p.gpus) // and every GPU and NPU unit, with its heat
            s.gpu_.move(prey, eater, s.gpu_.count[prey]);
        if (p.npus)
            s.npu_.move(prey, eater, s.npu_.count[prey]);
        if (s.disk_on()) // and all of its disk (Phase 4; W4: both tiers, each extent keeping its tier)
            s.disk_.move_all(prey, eater);
        if (p.warps) // and its warps, up to warp.max (Phase 8)
            s.merge_warps(prey, eater);
        s.rehost_guests(prey, eater); // and its guests (docs/inject-plan.md)
        P.blocks = P.cpus = P.latest = kNone;
        P.unit_head = P.unit_tail = kNone;
        P.units = P.mem = P.ncpu = 0;
        P.alive = 0; // its slot is recycled by the death sweep
        ++s.tally_.engulfs;
        if (s.dormant(prey)) { // easy food: a resting dormant program swallowed whole
            s.dorm_[prey].on = 0;
            ++s.tally_.dormant_eaten;
        }
    }

    // --- INJECT moves the program (docs/inject-plan.md M, S4) -----------------
    //
    // Damian, 2026-11-09: "injecting should insert the program not a copy. It
    // should bring everything over, including machinery." A landed INJECT
    // moves the injecting body into its host: its genome overwrites the
    // host's first block just inside the end it came in by (I2's place), its
    // threads carry on in it, it keeps every holding, and the memory it leaves
    // is cleared to codon 0: free space, not information.

    // Its aim (M, S4): a swallowed body's is its eater, no wound needed, at
    // the end of the eater's code nearer its own; anyone else's the nearest
    // (or best-matching) wound in reach (I1). kNone: none.
    u32 inject_aim(u32 ip, const u8* bits, u32 n, bool* high, u32* pmatch, u32* plen) {
        *pmatch = *plen = 0;
        if (s.is_swallowed(c.body)) {
            const u32 e = s.host_of(c.body);
            const u32 ek = s.bodies_[e].blocks, mk = s.bodies_[c.body].blocks;
            if (ek == kNone || mk == kNone)
                return kNone;
            const Block& E = s.blocks_[ek];
            const u32 me = s.blocks_[mk].start;
            const u32 dlo = me > E.start ? me - E.start : E.start - me;
            const u32 hiend = E.start + E.len - 1;
            const u32 dhi = me > hiend ? me - hiend : hiend - me;
            *high = dhi < dlo;
            return e;
        }
        return wound_pick(ip, bits, n, high, pmatch, plen);
    }

    // Whether this body may move into `host` now: it has a genome of its own
    // (a parasite moves no further: M8) and no guests in it (Claude's call:
    // its code is theirs too, so an infected program can't leave it), the
    // host owns at least contact.inject_ratio x its memory (M6), and the
    // host's code has room for its genome x contact.inject_room.
    bool inject_allowed(u32 host) const {
        const Body& I = s.bodies_[c.body];
        const u32 hk = s.bodies_[host].blocks;
        if (I.blocks == kNone || s.is_parasite(c.body) || s.has_guests(c.body) || hk == kNone)
            return false;
        if (p.inject_ratio > 0 &&
            static_cast<double>(s.bodies_[host].mem) < p.inject_ratio * static_cast<double>(I.mem))
            return false;
        const u32 g = s.blocks_[I.blocks].len;
        return !(p.inject_room > 0 && static_cast<double>(g) * p.inject_room > static_cast<double>(s.blocks_[hk].len));
    }

    // The codons a move into `host` writes: the genome, at most the host's code.
    u32 inject_size(u32 host) const {
        const u32 g = s.blocks_[s.bodies_[c.body].blocks].len, h = s.blocks_[s.bodies_[host].blocks].len;
        return g < h ? g : h;
    }

    // The move itself (inject_allowed holds). Returns the codons written;
    // *next (the INJECT's next instruction) moves with its thread.
    u32 move_in(u32 host, bool high, u32* next) {
        const u32 me = c.body;
        Body& I = s.bodies_[me];
        const u32 G = I.blocks;
        const u32 gs = s.blocks_[G].start, gl = s.blocks_[G].len;
        const u32 hk = s.bodies_[host].blocks;
        const Block& H = s.blocks_[hk];
        const u32 count = gl < H.len ? gl : H.len;
        const u32 dest = high ? H.start + H.len - count : H.start;
        // Its genome over the host's codons (those the host still owns there).
        moved_fast = 0;
        for (u32 i = 0; i < count; ++i) {
            const u32 a = dest + i;
            if (s.owner_[a] != hk)
                continue;
            const u8 was_ = s.mem_[a];
            s.mem_[a] = s.mem_[gs + i];
            if (!s.stamp_.empty())
                s.stamp_[a] = 0;
            note_write(a, was_);
            moved_fast += fast(a) ? 1u : 0u;
        }
        // Its threads and warps carry on in it: an address in its genome moves
        // with it, one elsewhere in its own memory goes to its start.
        auto mine_before = [&](u32 a) { return a < p.size && s.owner_[a] != kNone && s.blocks_[s.owner_[a]].body == me; };
        auto map = [&](u32 a) -> u32 {
            if (a >= gs && a < gs + gl)
                return a - gs < count ? dest + (a - gs) : dest;
            return mine_before(a) ? dest : a;
        };
        auto move_cpu = [&](Cpu& x) {
            x.ip = map(x.ip);
            x.rh = map(x.rh);
            x.wh = map(x.wh);
            for (u32 j = 0; j < x.sp && j < kMaxStack; ++j)
                x.stack[j] = map(x.stack[j]);
            x.block = hk;
            x.parasite = 1;
        };
        for (u32 x = I.cpus; x != kNone; x = s.cpus_[x].next)
            if (x != ci) // (this one is c, below)
                move_cpu(s.cpus_[x]);
        move_cpu(c); // (never a warp's: contact_may)
        *next = map(*next);
        // (Claude's call: a parasite runs as CPU threads; its warps end, its
        // GPU units stay with it. A warp's code would need its host's memory
        // in its record.)
        if (p.warps)
            s.kill_warps(me);
        // Its other actions end (it is somewhere else now).
        for (usize j = 0; j < s.actions_.size();) {
            if (s.actions_[j].actor == me && s.actions_[j].actor_serial == I.serial) {
                for (usize m = j; m + 1 < s.actions_.size(); ++m)
                    s.actions_[m] = s.actions_[m + 1];
                s.actions_.pop();
            } else {
                ++j;
            }
        }
        // The memory it leaves: zeros (Damian: "free space, but not information").
        if (s.obs_)
            s.obs_->on_move_in(me, G);
        for (u32 k = I.blocks; k != kNone;) {
            const u32 nk = s.blocks_[k].next;
            const Block& B = s.blocks_[k];
            for (u32 a = B.start; a < B.start + B.len; ++a)
                if (s.owner_[a] == k) {
                    s.mem_[a] = 0;
                    if (!s.stamp_.empty())
                        s.stamp_[a] = 0;
                }
            s.free_block(k);
            k = nk;
        }
        I.blocks = I.latest = kNone;
        I.mem = 0;
        if (s.is_swallowed(me))
            ++s.tally_.escapes; // S4: out of digestion, into its eater's code
        s.set_guest(me, host, kGuestParasite, dest, count);
        ++s.tally_.injects;
        ++s.tally_.parasite_threads;
        return count;
    }

    // --- ENGULF as a bite (Phase 10, design.md 6.16; engulf.bite) -------------
    //
    // A bite is CX codons, at most the eater's own size: nothing bigger than
    // itself, though it can take a piece of something bigger. An aimed bite
    // (a template after ENGULF) goes to the best-matching surface, as the
    // contest always did; a blind one takes the nearest material in range: a
    // waste codon or a surfaceless body (a surface shields a body from blind
    // bites). Raw material has no surface and no grip roll. A body no bigger
    // than the bite is swallowed whole, as ENGULF did; a bigger one loses the
    // codons at its end nearest the eater (the point of contact) and the same
    // share of its millicores (at random) and GPU and NPU units, and limps on
    // if it can. Bitten material becomes the eater's latest block, as ALLOC's.

    // docs/bites-plan.md B2: at most engulf.bite_share of the biter's own
    // size, rounded up (so a 1-codon program can still eat another).
    u32 bite_size() const {
        const i32 cx = static_cast<i32>(c.cx);
        if (cx <= 0)
            return 0;
        const u32 own = s.bodies_[c.body].mem;
        const double want = p.bite_share * static_cast<double>(own);
        u32 cap = static_cast<u32>(want);
        if (static_cast<double>(cap) < want - 1e-9)
            ++cap;
        cap = cap < 1 ? 1 : (cap > own ? own : cap);
        return static_cast<u32>(cx) < cap ? static_cast<u32>(cx) : cap;
    }

    // B1: a blind bite's body: the nearest other body in contact range, NOP
    // surface or not (a surface no longer shields); kNone if none.
    u32 nearest_body(u32 ip, u32* pdist) {
        u32 best = kNone, best_dist = 0;
        u64 seen = 0, seen_f = 0;
        for (usize i = 0; i < s.order_.size(); ++i) {
            const u32 t = s.order_[i];
            if (t == c.body || !s.bodies_[t].alive || s.bodies_[t].blocks == kNone || s.host_of(t) != kNone)
                continue;
            const u32 start = s.blocks_[s.bodies_[t].blocks].start;
            const u32 dist = start > ip ? start - ip : ip - start;
            if (dist > p.contact_range)
                continue;
            ++seen;
            seen_f += fast(start) ? 1 : 0;
            if (best == kNone || dist < best_dist) {
                best = t;
                best_dist = dist;
            }
        }
        charge2(costs().scan, seen_f, seen - seen_f);
        *pdist = best_dist;
        return best;
    }

    // I1: INJECT's aim: a body with an open end (a wound) whose outermost codon
    // is within contact range of ip. Blind: the nearest open end; aimed: the
    // best-matching surface among bodies with one, then the nearest. *high:
    // which end. A NOP blocks INJECT, and so does unbroken code: only a wound
    // lets it in. kNone if none.
    u32 wound_pick(u32 ip, const u8* bits, u32 n, bool* high, u32* pmatch, u32* plen) {
        u32 best = kNone, best_match = 0, best_len = 0, best_dist = 0;
        bool best_high = false;
        u8 sb[64];
        u64 seen = 0, seen_f = 0;
        for (usize i = 0; i < s.order_.size(); ++i) {
            const u32 t = s.order_[i];
            if (t == c.body || !s.bodies_[t].alive || s.bodies_[t].blocks == kNone || s.host_of(t) != kNone)
                continue;
            u32 m = kNone; // its surface's length, read once it matters
            for (u32 side = 0; side < 2; ++side) {
                const u32 a = s.outer_codon(t, side == 1);
                if (a == kNone)
                    continue;
                const u32 dist = a > ip ? a - ip : ip - a;
                if (dist > p.contact_range)
                    continue;
                ++seen;
                seen_f += fast(a) ? 1 : 0;
                if (is_nop(p.table.op[s.mem_[a]]))
                    continue; // closed: a NOP there
                u32 match = 0, len = 0;
                if (n > 0) { // aimed: its surface against the template
                    if (m == kNone)
                        m = s.dormant(t) ? 0 : surface(t, sb);
                    len = n > m ? n : m;
                    for (u32 j = 0; j < n && j < m; ++j)
                        match += bits[j] != sb[j];
                }
                const u64 num = len ? match : 0, den = len ? len : 1;
                const u64 bnum = best_len ? best_match : 0, bden = best_len ? best_len : 1;
                if (best == kNone || num * bden > bnum * den || (num * bden == bnum * den && dist < best_dist)) {
                    best = t;
                    best_match = match;
                    best_len = len;
                    best_dist = dist;
                    best_high = side == 1;
                }
            }
        }
        charge2(costs().scan, seen_f, seen - seen_f);
        *high = best_high;
        *pmatch = best_match;
        *plen = best_len;
        return best;
    }

    // The nearest waste codon within contact range of ip (forward first at
    // each distance), or kNone; each place looked at is a scan.
    u32 nearest_waste(u32 ip) {
        u64 seen = 0, seen_f = 0; // (W3: of them in fast memory)
        for (u32 d = 0; d <= p.contact_range; ++d) {
            if (ip + d < p.size) {
                ++seen;
                seen_f += fast(ip + d) ? 1 : 0;
                if (s.owner_[ip + d] == kNone) {
                    charge2(costs().scan, seen_f, seen - seen_f);
                    return ip + d;
                }
            }
            if (d > 0 && ip >= d) {
                ++seen;
                seen_f += fast(ip - d) ? 1 : 0;
                if (s.owner_[ip - d] == kNone) {
                    charge2(costs().scan, seen_f, seen - seen_f);
                    return ip - d;
                }
            }
        }
        charge2(costs().scan, seen_f, seen - seen_f);
        return kNone;
    }

    // The run of waste through a, up to want codons (forward from a, then
    // back); *lo its start. 0 if a isn't waste.
    u32 waste_run(u32 a, u32 want, u32* lo) const {
        *lo = a;
        if (a >= p.size || s.owner_[a] != kNone)
            return 0;
        u32 len = 0;
        for (u32 x = a; x < p.size && len < want && s.owner_[x] == kNone; ++x)
            ++len;
        u32 l = a;
        while (l > 0 && len < want && s.owner_[l - 1] == kNone) {
            --l;
            ++len;
        }
        *lo = l;
        return len;
    }

    // The bite's aim: a body (its contest match and length), or a waste codon
    // (*waste; the target is then kNone). Both kNone: nothing in reach.
    // docs/bites-plan.md B1: no surface shields a body any more. An aimed
    // bite picks the best-matching surface (it needs some match, or a
    // surfaceless body); a blind one the nearest material, waste or body. There
    // is no grip roll: aiming chooses which body, tissue decides the harm.
    u32 bite_aim(u32 ip, const u8* bits, u32 n, u32* waste, u32* pmatch, u32* plen) {
        *waste = kNone;
        if (n > 0) { // aimed: living things only
            const u32 t = contest_pick(ip, bits, n, true, pmatch, plen);
            const bool body_ok = t != kNone && (*plen == 0 || *pmatch > 0);
            return body_ok ? t : kNone;
        }
        *pmatch = *plen = 0;
        u32 bd = 0;
        const u32 t = nearest_body(ip, &bd);
        const u32 w = nearest_waste(ip);
        if (w == kNone)
            return t;
        const u32 wd = w > ip ? w - ip : ip - w;
        if (t != kNone && bd < wd)
            return t;
        *waste = w;
        return kNone;
    }

    // The codons a bite would take now (its time).
    u32 bite_extent(u32 target, u32 waste, u32 want) const {
        if (target != kNone)
            return s.bodies_[target].mem < want ? s.bodies_[target].mem : want;
        u32 lo = 0;
        return waste_run(waste, want, &lo);
    }

    // Block k moves to body `to` whole (its codons, contents and stamps).
    void give_block(u32 k, u32 to) {
        const u32 from = s.blocks_[k].body;
        unlink_block(from, k);
        Body& F = s.bodies_[from];
        if (F.latest == k)
            F.latest = kNone;
        F.mem -= s.blocks_[k].owned;
        s.forget_block(from, k);
        s.blocks_[k].body = to;
        Body& T = s.bodies_[to];
        if (T.blocks == kNone) {
            T.blocks = k;
        } else {
            u32 t = T.blocks;
            while (s.blocks_[t].next != kNone)
                t = s.blocks_[t].next;
            s.blocks_[t].next = k;
        }
        T.mem += s.blocks_[k].owned;
        T.latest = k;
    }

    // Block k's codons from address a on (high) or before a (low) become a new
    // block of the same body, right after k in its list; k keeps the rest
    // (and its id, so a lineage member stays one). Returns the new block.
    u32 carve_block(u32 k, u32 a, bool high) {
        u32 k2;
        if (!s.free_blocks_.empty()) {
            k2 = s.free_blocks_.back();
            s.free_blocks_.pop();
        } else {
            k2 = static_cast<u32>(s.blocks_.size());
            s.blocks_.push(Block{});
        }
        const u32 start = s.blocks_[k].start, end = start + s.blocks_[k].len;
        const u32 lo = high ? a : start, hi = high ? end : a;
        u32 moved = 0;
        for (u32 x = lo; x < hi; ++x) {
            if (s.owner_[x] == k) {
                s.owner_[x] = k2;
                ++moved;
            }
        }
        Block& b2 = s.blocks_[k2];
        b2.start = lo;
        b2.len = hi - lo;
        b2.body = s.blocks_[k].body;
        b2.owned = moved;
        b2.born = s.blocks_[k].born; // (a bitten piece keeps its block's age)
        Block& b1 = s.blocks_[k];
        if (high) {
            b1.len = a - start;
        } else {
            b1.start = a;
            b1.len = end - a;
        }
        b1.owned -= moved;
        b2.next = b1.next;
        b1.next = k2;
        if (s.obs_)
            s.obs_->on_carve(k2);
        return k2;
    }

    // Cheaper corpses (Damian, 2026-11-08): a dead program's fresh remnant
    // in the waste run through a, up to want codons -- codons stamped by the
    // salvage rule (a dead program's code; vents, wipes and strikes leave none)
    // that haven't decayed to INERT. A bite charges each of them
    // engulf.corpse_cost of a raw codon: nothing is made, the same codons
    // move, but a meal of corpse costs less time than one of vent material.
    // (Counted before the claim, which clears the stamps. 0 without salvage.)
    u32 corpse_codons(u32 a, u32 want) const {
        if (s.stamp_.empty() || a == kNone)
            return 0;
        u32 lo = 0;
        const u32 len = waste_run(a, want, &lo);
        u32 k = 0;
        for (u32 x = lo; x < lo + len; ++x)
            k += s.stamp_[x] != 0 && p.table.op[s.mem_[x]] != Op::inert ? 1 : 0;
        return k;
    }
    // A bite's charged codons: raw ones whole, corpse ones at the discount.
    double bite_weight(u32 got, u32 corpse) const {
        const u32 k = corpse < got ? corpse : got;
        return static_cast<double>(got - k) + static_cast<double>(k) * p.corpse_cost;
    }

    // A bite of raw material: the waste run through a, up to want codons,
    // claimed as the eater's latest block, contents and all. Returns codons.
    u32 bite_waste(u32 a, u32 want) {
        u32 lo = 0;
        const u32 len = waste_run(a, want, &lo);
        if (len == 0)
            return 0;
        const u32 k = s.new_block(lo, len, c.body);
        s.bodies_[c.body].latest = k;
        ++s.tally_.bites_waste;
        moved_fast = tiered ? fast_in(lo, len) : 0; // W3
        return len;
    }

    // A bite of a bigger body: want codons from its end nearest ip, and the
    // same share of its millicores (at random) and GPU and NPU units. The
    // piece keeps its stamps (it was the prey's code). Returns codons taken.
    u32 bite_piece(u32 prey, u32 want, u32 ip) {
        const u32 eater = c.body;
        const u32 n0 = s.bodies_[prey].mem;
        const u32 g = s.genome_len(prey);
        // its blocks in address order
        Array<u32> order;
        for (u32 k = s.bodies_[prey].blocks; k != kNone; k = s.blocks_[k].next) {
            usize i = order.size();
            order.push(k);
            while (i > 0 && s.blocks_[order[i - 1]].start > s.blocks_[k].start) {
                order[i] = order[i - 1];
                --i;
            }
            order[i] = k;
        }
        if (order.empty() || n0 == 0)
            return 0;
        const u32 lo = s.blocks_[order[0]].start;
        const u32 hi = s.blocks_[order.back()].start + s.blocks_[order.back()].len;
        const bool from_low = ip < lo || (ip < hi && ip - lo <= hi - ip);
        // B4 (NOPs as tissue): its code, NOPs not counted, before and as taken;
        // B5: its open ends before the bite (a wound is one more after it).
        auto code_in = [&](u32 k) {
            u32 n = 0;
            const Block& B = s.blocks_[k];
            for (u32 a = B.start; a < B.start + B.len; ++a)
                n += s.owner_[a] == k && !is_nop(p.table.op[s.mem_[a]]);
            return n;
        };
        u32 code0 = 0, code_taken = 0;
        if (p.tissue)
            for (usize j = 0; j < order.size(); ++j)
                code0 += code_in(order[j]);
        const u32 open0 = static_cast<u32>(s.open_end(prey, false)) + static_cast<u32>(s.open_end(prey, true));
        u32 left = want, taken = 0;
        for (usize j = 0; j < order.size() && left > 0; ++j) {
            const u32 k = order[from_low ? j : order.size() - 1 - j];
            const u32 owned = s.blocks_[k].owned;
            if (owned == 0)
                continue;
            u32 piece = k;
            if (owned > left) { // part of it: the `left` owned codons on the near side
                const u32 st = s.blocks_[k].start, en = st + s.blocks_[k].len;
                u32 a = 0, cnt = 0;
                if (from_low) {
                    for (a = st; a < en && cnt < left; ++a)
                        cnt += s.owner_[a] == k;
                } else {
                    for (a = en; a > st && cnt < left; --a)
                        cnt += s.owner_[a - 1] == k;
                }
                piece = carve_block(k, a, !from_low);
            }
            const u32 got = s.blocks_[piece].owned;
            moved_fast += tiered ? block_fast_owned(piece) : 0; // W3
            if (p.tissue)
                code_taken += code_in(piece);
            s.stamp_block(piece, g);
            give_block(piece, eater);
            left -= got;
            taken += got;
        }
        if (s.bodies_[prey].blocks != kNone &&
            static_cast<u32>(s.open_end(prey, false)) + static_cast<u32>(s.open_end(prey, true)) > open0)
            ++s.tally_.wounds; // B5: the bite left an end of its code open
        // The same share of its holdings: millicores at random, GPU and NPU
        // units oldest first (as DIVIDE shares them). B4: with tissue, the share
        // of its code taken, NOPs not counted (a bite of padding takes no cores).
        const u64 share_n = p.tissue ? code_taken : taken, share_d = p.tissue ? (code0 ? code0 : 1) : n0;
        Body& P = s.bodies_[prey];
        const u32 units = static_cast<u32>((2 * static_cast<u64>(P.units) * share_n + share_d) / (2 * share_d));
        if (units > 0) {
            Array<u32> pool;
            for (u32 u = P.unit_head; u != kNone; u = s.unit_next_[u])
                pool.push(u);
            for (u32 i = 0; i < units && i < pool.size(); ++i) {
                const u32 j = i + static_cast<u32>(s.rng_.below(pool.size() - i));
                const u32 u = pool[j];
                pool[j] = pool[i];
                pool[i] = u;
                s.unlink_unit(u);
                s.append_unit(eater, u);
            }
        }
        if (p.gpus)
            s.gpu_.move(prey, eater,
                        static_cast<u32>((2 * static_cast<u64>(s.gpu_.count[prey]) * share_n + share_d) / (2 * share_d)));
        if (p.npus)
            s.npu_.move(prey, eater,
                        static_cast<u32>((2 * static_cast<u64>(s.npu_.count[prey]) * share_n + share_d) / (2 * share_d)));
        if (s.dormant(prey)) { // a resting dormant program loses the same share of its reserves
            Dormant& d = s.dorm_[prey];
            d.mem_reserve -= static_cast<u32>((2 * static_cast<u64>(d.mem_reserve) * share_n + share_d) / (2 * share_d));
            d.disk_reserve -=
                static_cast<u32>((2 * static_cast<u64>(d.disk_reserve) * share_n + share_d) / (2 * share_d));
            d.archive_reserve -= // W4 (0 without an archive)
                static_cast<u32>((2 * static_cast<u64>(d.archive_reserve) * share_n + share_d) / (2 * share_d));
        }
        ++s.tally_.bites_body;
        return taken;
    }

    // A bite lands on what it aimed at: raw material claimed, a body
    // swallowed whole, or a piece of a bigger one. Returns codons taken.
    u32 bite_land(u32 target, u32 waste, u32 want, u32 ip) {
        u32 got = 0;
        moved_fast = 0; // W3: set or summed by what lands
        if (target == kNone) {
            got = bite_waste(waste, want);
        } else if (s.bodies_[target].mem <= want) {
            got = s.bodies_[target].mem;
            // docs/inject-plan.md S1 (Damian): swallowed, to be digested, not
            // dead. (A resting dormant program can't respond: it is food at
            // once, as before. Claude's call.)
            if (p.digest > 0 && !s.dormant(target)) {
                s.set_guest(target, c.body, kGuestSwallowed);
                ++s.tally_.swallows;
                moved_fast = tiered ? fast_owned(target) : 0;
            } else {
                engulf_body(target);
            }
        } else {
            got = bite_piece(target, want, ip);
        }
        s.tally_.codons_bitten += got;
        return got;
    }

    // Returns the instructions charged; *ok says whether it succeeded.
    u32 contact(u32 ip, bool engulf, u32* next, bool* ok) {
        u8 bits[64];
        u32 n = read_template(next_ip(ip), bits);
        *next = skip(ip, n);
        if (engulf)
            ++s.tally_.engulf_tries;
        else
            ++s.tally_.inject_tries;
        t_ps += costs().call; // Phase 9 (a warp's host instruction): a world change
        u32 cost = 1 + p.contact_base;
        if (!contact_may(engulf)) {
            *ok = false;
            return cost;
        }
        if (engulf && p.bites) { // Phase 10: a bite, at once
            *ok = false;
            const u32 want = bite_size();
            if (want == 0)
                return cost;
            u32 waste = kNone, match = 0, len = 0;
            const u32 t = bite_aim(ip, bits, n, &waste, &match, &len);
            if (t == kNone && waste == kNone)
                return cost;
            // (docs/bites-plan.md B1: no grip roll; aiming only chooses the body)
            // (Cheaper corpses: a dead program's remnant, or a dormant program whole.)
            const bool dorm = t != kNone && s.dormant(t);
            const u32 corpse = t == kNone ? corpse_codons(waste, want) : 0;
            const u32 got = bite_land(t, waste, want, ip);
            if (got == 0)
                return cost;
            const u32 cheap = dorm ? got : (corpse < got ? corpse : got);
            s.tally_.corpse_codons += cheap;
            cost += static_cast<u32>(p.contact_per_codon * bite_weight(got, cheap));
            charge2(costs().move, moved_fast, got - moved_fast); // (W3: at their tiers)
            *ok = true;
            return cost;
        }
        bool high = false;
        u32 target = kNone;
        if (engulf) {
            target = contest(ip, bits, n, true);
        } else { // docs/bites-plan.md I1: only through an open end (or out of a swallower: inject-plan S4)
            u32 wm = 0, wl = 0;
            target = inject_aim(ip, bits, n, &high, &wm, &wl);
        }
        if (target == kNone) {
            *ok = false;
            return cost; // a failed contest still costs the attempt (6.4 #7)
        }
        if (!engulf) { // docs/inject-plan.md M: the program moves in, priced like a bite (M7)
            if (!inject_allowed(target)) {
                *ok = false; // too big for the host (M6) or its code (room): the host is unharmed
                return cost;
            }
            const u32 moved = move_in(target, high, next);
            cost += static_cast<u32>(p.contact_per_codon * static_cast<double>(moved));
            charge2(costs().move, moved_fast, moved - moved_fast); // (W3: at their tiers)
            *ok = true;
            return cost;
        }
        u32 moved = s.bodies_[target].mem;
        const bool dorm = s.dormant(target); // (cheaper corpses: a dormant program too)
        engulf_body(target);
        if (dorm)
            s.tally_.corpse_codons += moved;
        cost += static_cast<u32>(p.contact_per_codon * bite_weight(moved, dorm ? moved : 0));
        charge2(costs().move, moved_fast, moved - moved_fast); // (W3: at their tiers)
        *ok = true;
        return cost;
    }

    // Who may make contact now (docs/inject-plan.md M8, S3, S4): a parasite
    // neither bites nor moves on; a swallowed body only INJECTs (into its
    // eater); a warp doesn't INJECT (Claude's call: a parasite runs as CPU
    // threads, M3).
    bool contact_may(bool engulf) const {
        if (s.is_parasite(c.body))
            return false;
        if (s.is_swallowed(c.body) && engulf)
            return false;
        return engulf || !warp;
    }

    // --- contact as an action (Phase 9, design.md 6.15) ----------------------
    //
    // With a timed cost table a CPU's ENGULF or INJECT takes ticks. At its
    // start the contest picks its target, as above, but does not roll: if it
    // has any chance (a target, a surface match above 0 or a blind ENGULF, an
    // INJECT payload that fits), the action starts and its target is marked.
    // An ENGULF lasts the prey's codons at engulf.codons_per_tick, its CPU at
    // work meanwhile; an INJECT likewise lasts the codons it moves, priced
    // like a bite (docs/inject-plan.md M7). Either lasts at least a tick, so
    // every body gets a turn in between.
    //
    // At its end (contact_end) it lands only if the target is still there and
    // in range, rolling its chance against the target's surface as it is
    // then. So a target that sensed it (SENSE 22, 23) can rewrite its surface,
    // move away, shrink, or eat the attacker first: a failure whose chance
    // fell since the start is a defence (actions_defended, and the detector's
    // `defended` milestone).
    u32 contact_start(u32 ip, bool engulf, u32* next, bool* ok) {
        u8 bits[64];
        const u32 n = read_template(next_ip(ip), bits);
        *next = skip(ip, n);
        *ok = false;
        if (engulf)
            ++s.tally_.engulf_tries;
        else
            ++s.tally_.inject_tries;
        t_ps += costs().call;
        if (!contact_may(engulf))
            return 1; // (docs/inject-plan.md M8, S3: fails at once)
        if (engulf && p.bites) { // Phase 10: a bite takes its codons' time
            const u32 want = bite_size();
            if (want == 0)
                return 1;
            u32 waste = kNone, match = 0, len = 0;
            const u32 t = bite_aim(ip, bits, n, &waste, &match, &len);
            const u32 size = (t == kNone && waste == kNone) ? 0 : bite_extent(t, waste, want);
            if (size == 0)
                return 1; // nothing in reach: fails at once
            // Its time: its codons at engulf.codons_per_tick, a fresh corpse's
            // or a dormant program's at engulf.corpse_cost of that.
            const u32 cheap = t == kNone ? corpse_codons(waste, want) : (s.dormant(t) ? size : 0);
            const double weight = bite_weight(size, cheap);
            const double whole = ceil(weight / static_cast<double>(p.engulf_cpt));
            const u64 ticks = max_of<u64>(1, static_cast<u64>(whole));
            Action a = {};
            a.end = s.tick_ + ticks;
            a.actor_serial = s.bodies_[c.body].serial;
            a.target_serial = t != kNone ? s.bodies_[t].serial : 0;
            a.op = static_cast<u32>(Op::engulf);
            actor_of(&a);
            a.actor = c.body;
            a.target = t;
            a.n = n;
            a.match = match;
            a.len = len;
            a.bite = want;
            a.at = t == kNone ? waste : 0;
            memcpy(a.bits, bits, n);
            s.actions_.push(a);
            ++s.tally_.actions_started;
            deferred = true;
            return 1;
        }
        u32 match = 0, len = 0;
        bool high = false;
        u32 target = kNone;
        if (engulf) { // (Phase 9's whole swallow, without bites)
            target = contest_pick(ip, bits, n, true, &match, &len);
            if (target == kNone || (len != 0 && match == 0))
                return 1; // no chance at all: fails at once, as a lost contest does
        } else { // docs/bites-plan.md I1: only through an open end (or out of a swallower: inject-plan S4)
            target = inject_aim(ip, bits, n, &high, &match, &len);
            if (target == kNone)
                return 1; // no wound in reach: fails at once
            if (!inject_allowed(target))
                return 1; // too big for the host (M6) or its code: fails at once
        }
        const u64 size = engulf ? s.bodies_[target].mem : inject_size(target);
        u64 ticks = 1;
        if (engulf) {
            const u64 pace = p.engulf_cpt;
            ticks = max_of<u64>(1, (size + pace - 1) / pace);
            if (s.dormant(target)) // (cheaper corpses: a dormant program, swallowed whole)
                ticks = max_of<u64>(1, static_cast<u64>(ceil(bite_weight(static_cast<u32>(size), static_cast<u32>(size)) /
                                                             static_cast<double>(pace))));
        } else { // docs/inject-plan.md M7: the codons it moves at a bite's pace, its CPU busy meanwhile
            const u64 pace = p.engulf_cpt;
            ticks = max_of<u64>(1, (size + pace - 1) / pace);
        }
        Action a = {};
        a.end = s.tick_ + ticks;
        a.actor_serial = s.bodies_[c.body].serial;
        a.target_serial = s.bodies_[target].serial;
        a.op = static_cast<u32>(engulf ? Op::engulf : Op::inject);
        actor_of(&a);
        a.actor = c.body;
        a.target = target;
        a.n = n;
        a.match = match;
        a.len = len;
        a.at = high ? 1u : 0u; // INJECT: which end of the host its wound is
        memcpy(a.bits, bits, n);
        s.actions_.push(a);
        ++s.tally_.actions_started;
        deferred = true;
        return 1;
    }

    // The end of a contact action (see contact_start).
    u32 contact_end(u32 ip, bool engulf, u32* next, bool* ok) {
        const Action& a = *resolving;
        u8 cur[64];
        *next = skip(ip, read_template(next_ip(ip), cur)); // past the template as it is now
        *ok = false;
        if (engulf && p.bites && a.target == kNone) { // Phase 10: a bite of raw material
            const u32 d = a.at > ip ? a.at - ip : ip - a.at;
            const u32 corpse = d <= p.contact_range ? corpse_codons(a.at, a.bite) : 0;
            const u32 got = d <= p.contact_range ? bite_land(kNone, a.at, a.bite, ip) : 0;
            if (got == 0)
                return 1; // claimed by another meanwhile (run_cpu counts it failed)
            s.tally_.corpse_codons += corpse < got ? corpse : got;
            charge2(costs().move, moved_fast, got - moved_fast); // (W3: at their tiers)
            *ok = true;
            return 1;
        }
        const u32 t = a.target;
        const Body& T = s.bodies_[t];
        if (!T.alive || T.serial != a.target_serial || T.blocks == kNone)
            return 1; // gone: eaten by another, or dead (run_cpu counts it failed)
        // docs/inject-plan.md: the actor or its target may have become a guest
        // meanwhile. A swallowed body's INJECT lands only in its eater; a guest
        // target is out of reach (swallowed by another, or moved in).
        if (!contact_may(engulf))
            return 1;
        const bool escape = !engulf && s.is_swallowed(c.body);
        if (escape ? s.host_of(c.body) != t : s.host_of(t) != kNone)
            return 1;
        const u32 k = T.blocks;
        // A failure because the target changed in time is a defence (its
        // place, its surface, its size, a wound it closed).
        auto defended = [&]() {
            ++s.tally_.actions_defended;
            if (s.obs_)
                s.obs_->on_defended(s, k, s.tick_);
        };
        if (!engulf) { // docs/bites-plan.md I1: it lands if its wound is still open and in reach
            const bool high = a.at != 0;
            bool open = escape; // (S4: out of a swallower needs no wound)
            u32 wd = 0;
            if (!escape) {
                const u32 w = s.outer_codon(t, high);
                open = w != kNone && !is_nop(p.table.op[s.mem_[w]]);
                wd = w == kNone ? 0 : (w > ip ? w - ip : ip - w);
            }
            if (!open || wd > p.contact_range || !inject_allowed(t)) { // (run_cpu counts it failed)
                defended(); // the wound closed, the host moved off or shrank below it
                return 1;
            }
            const u32 moved = move_in(t, high, next);
            charge2(costs().move, moved_fast, moved - moved_fast); // (W3: at their tiers)
            *ok = true;
            return 1;
        }
        const u32 start = s.blocks_[k].start;
        const u32 dist = start > ip ? start - ip : ip - start;
        bool win = dist <= p.contact_range;
        bool changed = !win;
        if (!p.bites) { // Phase 9's whole swallow: its chance now against at the start (0/0 = 1)
            u8 sb[64];
            const u32 m = surface(t, sb);
            const u32 len = a.n > m ? a.n : m;
            u32 match = 0;
            for (u32 j = 0; j < a.n && j < m; ++j)
                match += a.bits[j] != sb[j];
            const u64 now_num = len ? match : 1, now_den = len ? len : 1;
            const u64 was_num = a.len ? a.match : 1, was_den = a.len ? a.len : 1;
            changed = changed || now_num * was_den < was_num * now_den;
            win = win && (len == 0 || s.rng_.below(len) < match);
        } // (B1: a bite has no grip roll; only moving out of reach saves its target)
        if (!win) { // (run_cpu counts it failed)
            if (changed)
                defended();
            return 1;
        }
        u32 moved = T.mem;
        const bool dorm = s.dormant(t); // (cheaper corpses: counted as they land)
        if (p.bites) // Phase 10: whole if it's no bigger than the bite, else a piece
            moved = bite_land(t, kNone, a.bite, ip);
        else
            engulf_body(t);
        if (dorm)
            s.tally_.corpse_codons += moved;
        charge2(costs().move, moved_fast, moved - moved_fast); // (W3: at their tiers)
        *ok = true;
        return 1;
    }

    // --- long instructions (Phase 9) ----------------------------------------

    // A DISK op's picoseconds: a kernel call, then a disk access and its
    // codons in sequence (just the call when it fails on its arguments).
    // W4: the slowest tier its range touches for the access, each codon at
    // its own tier's price (a range outside my disk, which will fail, at the
    // fast disk's, as before tiers).
    u64 disk_time() const {
        u64 t = costs().call;
        if (!s.disk_on() || c.bx >= 3 || static_cast<i32>(c.cx) <= 0)
            return t;
        const u64 n = c.cx;
        const bool store = p.sel.disk[c.bx] == 1;
        const u64 access = store ? costs().disk_write : costs().disk_read;
        const u64 per = store ? costs().disk_write_codon : costs().disk_read_codon;
        // Memory tiers (W3): its memory side, a bulk move per codon at each
        // codon's tier (the read head for compare and store, the write head
        // for load, advancing as COPY's), where the range lies in my disk.
        // Without tiers it costs nothing, as before.
        if (tiered && static_cast<u64>(c.ax) + n <= s.disk_held(c.body)) {
            u32 a = p.sel.disk[c.bx] == 2 ? c.wh : c.rh, nf = 0;
            for (u64 i = 0; i < n; ++i, a = next_addr(a))
                nf += fast(a) ? 1u : 0u;
            const double side = static_cast<double>(nf * costs().move) * tf[0] +
                                static_cast<double>((n - nf) * costs().move) * tf[1];
            t += static_cast<u64>(side + 0.5);
        }
        if (!s.disk_.tiered() && p.disk_speed == 1.0) // (exactly as before)
            return t + access + n * per;
        u32 na = 0;
        if (!s.disk_.archive_in(c.body, c.ax, c.cx, &na))
            na = 0;
        const double d = disk_scale(static_cast<double>(access), static_cast<double>(per), c.cx - na, na);
        return t + static_cast<u64>(d < 1.8e19 ? d : 1.8e19);
    }

    // An instruction longer than a tick of a whole core's time becomes an
    // action: the CPU waits for as many ticks as it takes, and it is done (run
    // through again, as the world is then) at its end.
    void start_long(Op op, u64 time) {
        Action a = {};
        const double ticks = static_cast<double>(time) / p.tick_units;
        const u64 whole = static_cast<u64>(ticks);
        a.end = s.tick_ + (static_cast<double>(whole) < ticks ? whole + 1 : whole);
        a.actor_serial = s.bodies_[c.body].serial;
        a.op = static_cast<u32>(op);
        actor_of(&a);
        a.actor = c.body;
        a.target = kNone;
        s.actions_.push(a);
        ++s.tally_.actions_started;
        deferred = true;
    }

    // --- the accelerators: GPU and NPU (Phase 2, 2b; design.md 6.7, 6.8) ----

    // The GPU instruction is the accelerator verb: BX 0-15 runs a GPU op and
    // BX 16-19 an NPU op, each through its selector map. Without that unit, or
    // with any other BX, it fails, charging 1.
    bool accel(u32* cost) {
        if (c.bx < kGpuOps) {
            if (!p.gpus || warp) // Phase 8: block ops fail in a warp (the warp is the GPU)
                return false;
            if (p.cost.timed && !resolving && device_time(false, 0))
                return true; // Phase 9b: longer than a tick: an action, done at its end
            return p.accel_async ? accel_issue(p.sel.gpu[c.bx], cost) : gpu_op(p.sel.gpu[c.bx], cost);
        }
        if (c.bx < kGpuOps + kNpuOps) {
            if (!p.npus)
                return false;
            const u32 op = p.sel.npu[c.bx - kGpuOps];
            if (p.cost.timed && !resolving && device_time(true, op))
                return true;
            return p.accel_async ? accel_issue(kAccDot + op, cost) : npu_op(op, cost);
        }
        return false;
    }

    // Phase 9b, timed tables: a block op's time. The first op a body sends
    // a device in a tick pays its start-up (a lone op's round trip); each
    // further op that tick only its share of a batch run in parallel. In
    // instant mode the CPU then waits for the work on the body's units
    // (elements at gpu_thr a lane, multiply-adds at npu_mac per thousandth of
    // the NPU); longer than a tick, the op becomes an action (true: started).
    // In async mode the CPU only sends it (a kernel call opens the batch) and
    // the result comes accel.latency ticks later, as before.
    bool device_time(bool npu, u32 op) {
        Array<u64>& batch = npu ? s.npu_batch_ : s.gpu_batch_;
        const bool open = s.batch_open(batch, c.body);
        const u64 start = npu ? costs().npu_start : costs().gpu_start;
        const u64 share = npu ? costs().npu_share : costs().gpu_share;
        if (p.accel_async) {
            t_ps += (open ? 0 : costs().call) + share;
            return false;
        }
        u64 t = open ? share : start;
        const i32 n = static_cast<i32>(c.cx);
        if (n > 0) {
            const u64 k = static_cast<u64>(n);
            // The work over the body's units, at the substrate's speed
            // (docs/world-plan.md W1; 1: exactly as before).
            const float speed = npu ? p.npu_speed : p.gpu_speed;
            auto over = [speed](u64 work, u64 units) {
                return speed == 1.0f ? work / units
                                     : static_cast<u64>(static_cast<double>(work) /
                                                        (static_cast<double>(units) * static_cast<double>(speed)));
            };
            if (npu) {
                const u64 macs = op == kMatvec ? k * k : (op == kMatmul ? k * k * k : k);
                const u64 units = s.npu_count(c.body) ? s.npu_count(c.body) : 1;
                t += over(macs * costs().npu_mac * 1000, units);
            } else {
                const u64 units = s.gpu_count(c.body) ? s.gpu_count(c.body) : 1;
                t += over(k * costs().gpu_thr, units);
            }
        }
        if (acts() && static_cast<double>(t) > p.tick_units) {
            start_long(Op::gpu, t);
            return true;
        }
        t_ps += t;
        return false;
    }

    // Launch (Phase 8, design.md 6.14): the GPU verb with BX 20 starts a warp
    // at the code after the template that follows it, found as JMP finds
    // one. Every lane starts with this CPU's (or lowest lane's) registers and
    // heads, lane L's heads L past. Fails without a template or a match,
    // without GPU units, or at warp.max. Costs gpu.setup. It asks for CX
    // lanes: the first power of two from warp.lanes_min that holds them, at
    // most the node's width (docs/lanes-plan.md L1, Damian).
    bool launch(u32 ip, u32* next, u32* cost) {
        u8 bits[64];
        const u32 n = read_template(next_ip(ip), bits);
        *next = skip(ip, n);
        if (n == 0)
            return false;
        const u32 to = search(ip, bits, n, 0);
        if (to == kNone || s.gpu_count(c.body) == 0 || s.warp_count_[c.body] >= p.warp_max ||
            s.is_parasite(c.body)) // (a parasite runs as CPU threads: docs/inject-plan.md M3)
            return false;
        const u32 w = s.new_warp(c.body, to, &c, s.fit_lanes(c.cx)); // CX lanes (docs/lanes-plan.md L1)
        *cost = max_of<u32>(1, p.gpu_setup);
        if (p.cost.timed) { // Phase 9b: a kernel launch is a call; the warp starts after the GPU's start-up
            t_ps += costs().call;
            const double ticks = static_cast<double>(costs().gpu_start) / p.tick_units;
            u64 whole = static_cast<u64>(ticks);
            if (static_cast<double>(whole) < ticks)
                ++whole;
            s.wuntil_[w] = s.tick_ + whole;
        }
        ++s.tally_.launches;
        return true;
    }

    // --- threads (Phase 9c, design.md 6.15) ----------------------------------

    // The GPU verb's BX 21 starts a thread at the code after the template
    // that follows it, found as JMP finds one, with this thread's registers
    // and heads (a fork with its arguments) and the kind of core it prefers.
    // It runs from the next tick. Fails without a template or a match, or
    // when the body holds cpu.max_threads. A kernel call.
    bool spawn(u32 ip, u32* next) {
        u8 bits[64];
        const u32 n = read_template(next_ip(ip), bits);
        *next = skip(ip, n);
        t_ps += costs().call;
        if (n == 0)
            return false;
        const u32 to = search(ip, bits, n, 0);
        if (to == kNone || s.bodies_[c.body].ncpu >= p.max_threads)
            return false;
        const u8 kind = warp ? kKindP : s.cpu_kind(ci);
        const u32 t = s.new_cpu(c.body, to);
        Cpu& T = s.cpus_[t];
        T.ax = c.ax;
        T.bx = c.bx;
        T.cx = c.cx;
        T.dx = c.dx;
        T.rh = c.rh;
        T.wh = c.wh;
        T.parasite = s.is_parasite(c.body) ? 1 : 0; // (a parasite's threads all are: inject-plan M9)
        s.cpu_kind_[t] = kind;
        ++s.tally_.threads_started;
        return true;
    }

    // BX 22: this thread prefers P cores (AX even) or E cores (AX odd). A
    // warp isn't a thread on a core: it fails there.
    bool prefer_kind() {
        if (warp || ci == kNone)
            return false;
        s.cpu_kind_[ci] = (c.ax & 1u) ? kKindE : kKindP;
        return true;
    }

    // --- asynchronous ops (Phase 5, design.md 6.11) ----------------------------

    // Walks n addresses from a like a head, each readable (or writable);
    // appends them to the soup's issue arena. False at the first that isn't.
    bool walk(u32 a, u32 n, bool write, u32* end) {
        for (u32 i = 0; i < n; ++i) {
            if (write ? !writable(a) : !readable(a))
                return false;
            s.issue_addr_.push(a);
            a = next_addr(a);
        }
        *end = a;
        return true;
    }

    // Issue: the op is checked and sized now (capacity, readable sources,
    // writable destinations), its heads advance as if it were done, its cost
    // is charged, and it is queued. Its inputs are read at the barrier and its
    // results written back accel.latency - 1 barriers later. Results that
    // were CX (compare, count, find, sum, dot, argmax) land in this CPU's
    // result slot (SENSE 18); CX is left at 0, or for elementwise ops at the
    // elements not issued, as in instant mode.
    bool accel_issue(u32 kind, u32* cost) {
        const i32 sn = static_cast<i32>(c.cx);
        if (sn <= 0)
            return false;
        const u32 n = static_cast<u32>(sn);
        const usize mark = s.issue_addr_.size();
        auto undo = [&]() {
            s.issue_addr_.resize(mark);
            return false;
        };
        AccelOp op = {};
        op.kind = kind;
        op.key = c.ax & 63;
        op.gerr = 1.0f;
        u32 rh = c.rh, wh = c.wh;
        u64 work = 0;
        if (!accel_is_npu(kind)) {
            const float cap = s.gpu_.cap[c.body];
            if (cap < 1.0f)
                return false;
            op.gerr = s.gpu_.err[c.body];
            if (kind <= kAccSum) { // reductions: all n now
                if (static_cast<float>(n) > cap || !walk(rh, n, false, &rh))
                    return undo();
                op.src_n = n;
                if (kind == kAccCompare) {
                    if (!walk(wh, n, false, &wh))
                        return undo();
                    op.dst_n = n;
                }
                op.n = n;
                c.cx = 0;
            } else { // elementwise: as many as capacity allows, to the first refusal
                const u32 todo = min_of<u32>(n, static_cast<u32>(cap));
                const bool reads_src = kind != kAccFill;
                const bool reads_dst = kind >= kAccAdd;
                Array<u32> src, dst;
                u32 done = 0;
                for (; done < todo; ++done) {
                    if (reads_src && !readable(rh))
                        break;
                    if (!writable(wh))
                        break;
                    if (reads_src)
                        src.push(rh);
                    dst.push(wh);
                    if (kind == kAccRcopy)
                        rh = prev_addr(rh);
                    else if (reads_src)
                        rh = next_addr(rh);
                    wh = next_addr(wh);
                }
                if (done == 0)
                    return false;
                s.issue_addr_.append(src.data(), src.size());
                if (reads_dst)
                    s.issue_addr_.append(dst.data(), dst.size()); // read ...
                s.issue_addr_.append(dst.data(), dst.size());     // ... and written
                op.src_n = static_cast<u32>(src.size());
                op.dst_n = reads_dst ? done : 0;
                op.n = done;
                c.cx = n - done;
            }
            work = op.n;
            s.gpu_.cap[c.body] -= static_cast<float>(work);
            s.gpu_.done[c.body] += static_cast<u32>(work);
            s.tally_.gpu_elements += work;
            *cost = max_of<u32>(1, p.gpu_setup + static_cast<u32>(static_cast<double>(work) * p.gpu_cpu_per_element));
        } else {
            const u32 k = n;
            if (kind == kAccMatvec)
                work = static_cast<u64>(k) * k;
            else if (kind == kAccMatmul)
                work = static_cast<u64>(k) * k * k;
            else
                work = k;
            if ((kind == kAccMatvec && k > p.npu_max_matvec) || (kind == kAccMatmul && k > p.npu_max_matmul))
                return false;
            if (static_cast<double>(work) > static_cast<double>(s.npu_.cap[c.body]))
                return false;
            if ((kind == kAccMatvec || kind == kAccMatmul) && c.ax >= p.size)
                return false;
            u32 m_end;
            switch (kind) {
            case kAccDot:
                if (!walk(rh, k, false, &rh) || !walk(wh, k, false, &wh))
                    return undo();
                op.src_n = op.dst_n = k;
                break;
            case kAccArgmax:
                if (!walk(rh, k, false, &rh))
                    return undo();
                op.src_n = k;
                break;
            case kAccMatvec:
                if (!walk(rh, k, false, &rh) || !walk(c.ax, k * k, false, &m_end) || !walk(wh, k, true, &wh))
                    return undo();
                op.src_n = k;
                op.mat_n = k * k;
                break;
            default: // matmul
                if (!walk(rh, k * k, false, &rh) || !walk(c.ax, k * k, false, &m_end) || !walk(wh, k * k, true, &wh))
                    return undo();
                op.src_n = op.mat_n = k * k;
                break;
            }
            op.n = k;
            c.cx = 0;
            s.npu_.cap[c.body] -= static_cast<float>(work);
            s.npu_.done[c.body] += static_cast<u32>(work);
            s.tally_.npu_macs += work;
            *cost = max_of<u32>(1, p.gpu_setup + static_cast<u32>(static_cast<double>(work) * p.npu_cpu_per_mac));
            if (s.obs_)
                s.obs_->on_npu(s, c.block, s.tick_);
        }
        c.rh = rh;
        c.wh = wh;
        op.out_n = accel_out_codons(kind, op.n);
        op.due = s.tick_ + (accel_is_npu(kind) ? p.npu_latency : p.gpu_latency) - 1; // (9c: each device's own)
        op.serial = s.accel_serial_++;
        op.body = c.body;
        op.body_serial = s.bodies_[c.body].serial;
        op.node = s.node_;
        op.cpu = ci;
        op.cpu_gen = ci < s.cpu_gen_.size() ? s.cpu_gen_[ci] : 0;
        op.genome = genome();
        if (ci < s.cpu_pending_.size())
            ++s.cpu_pending_[ci];
        s.issue_ref_.push(static_cast<u32>(mark));
        s.issued_.push(op);
        return true;
    }

    enum GpuOp : u32 {
        kCompare, kCount, kFind, kSum,                 // reductions: all-or-nothing
        kCopy, kFill, kNot, kRcopy,                    // elementwise: partial work
        kAdd, kSub, kXor, kAnd, kOr, kMin, kMax, kMul, // dest <- dest op source
    };

    void gpu_charge(u32 done, u32* cost) {
        s.gpu_.cap[c.body] -= static_cast<float>(done);
        s.gpu_.done[c.body] += done;
        s.tally_.gpu_elements += done;
        *cost = max_of<u32>(1, p.gpu_setup + static_cast<u32>(static_cast<double>(done) * p.gpu_cpu_per_element));
    }

    // GPU: one op across CX elements from the read head (source) to the write
    // head (destination), both advancing and wrapping like COPY's. Private
    // code and hot pages apply per element. Charges gpu.setup plus
    // gpu.cpu_per_element per element done.
    //  - Reductions (compare, count, find, sum) need capacity for all n and
    //    every element readable, else fail changing nothing; the result goes
    //    to CX. Find leaves the read head on the match and is charged only up
    //    to it.
    //  - Elementwise ops do as many elements as this tick's capacity allows,
    //    stopping at the first they may not do, and leave the rest in CX.
    bool gpu_op(u32 op, u32* cost) {
        const i32 n = static_cast<i32>(c.cx);
        const float cap = s.gpu_.cap[c.body];
        if (n <= 0 || cap < 1.0f)
            return false;
        const u8 key = static_cast<u8>(c.ax & 63);
        if (op <= kSum) {
            if (static_cast<float>(n) > cap)
                return false;
            u32 r = c.rh, w = c.wh, result = 0;
            for (u32 i = 0; i < static_cast<u32>(n); ++i) {
                if (!readable(r))
                    return false;
                const u8 v = take(r);
                if (op == kCompare) {
                    if (!readable(w))
                        return false;
                    result += v != take(w);
                    w = next_addr(w);
                } else if (op == kCount) {
                    result += v == key;
                } else if (op == kFind) {
                    if (v == key) { // the read head stops on the match
                        c.rh = r;
                        c.cx = i;
                        gpu_charge(i + 1, cost);
                        return true;
                    }
                } else {
                    result += v;
                }
                r = next_addr(r);
            }
            if (op == kFind)
                return false; // not there
            c.rh = r;
            if (op == kCompare)
                c.wh = w;
            c.cx = result;
            gpu_charge(static_cast<u32>(n), cost);
            return true;
        }
        const u32 todo = min_of<u32>(static_cast<u32>(n), static_cast<u32>(cap));
        const float gerr = s.gpu_.err[c.body];
        u32 done = 0;
        for (; done < todo; ++done) {
            const u32 from = c.rh, to = c.wh;
            if (op != kFill && !readable(from))
                break;
            if (!writable(to) || !page_accepts(to))
                break;
            const u32 src = op == kFill ? 0u : take(from), dst = take(to);
            u32 v;
            switch (op) {
            case kCopy:
            case kRcopy:
                v = copied_with(static_cast<u8>(src), gerr);
                break;
            case kFill:
                v = key;
                break;
            case kNot:
                v = 63 - src;
                break;
            case kAdd:
                v = dst + src;
                break;
            case kSub:
                v = dst - src;
                break;
            case kXor:
                v = dst ^ src;
                break;
            case kAnd:
                v = dst & src;
                break;
            case kOr:
                v = dst | src;
                break;
            case kMin:
                v = dst < src ? dst : src;
                break;
            case kMax:
                v = dst > src ? dst : src;
                break;
            default: // kMul
                v = dst * src;
                break;
            }
            const u8 was_ = s.mem_[to];
            s.mem_[to] = static_cast<u8>(v & 63);
            note_write(to, was_);
            if (s.obs_)
                s.obs_->on_write_gpu(to);
            if (op == kRcopy) // a reversed copy: the read head runs down
                c.rh = prev_addr(from);
            else if (op != kFill)
                c.rh = next_addr(from);
            c.wh = next_addr(to);
        }
        if (done == 0)
            return false;
        c.cx = static_cast<u32>(n) - done;
        gpu_charge(done, cost);
        return true;
    }

    enum NpuOp : u32 { kDot, kMatvec, kMatmul, kArgmax };

    // NPU values: a codon v is the signed fixed-point number (v - 32) / 32.
    static i64 sval(u8 v) { return static_cast<i64>(v & 63) - 32; }
    // A sum of products of such values, back into a codon: divide by 32
    // (arithmetic shift), saturate to -32..31, re-centre.
    static u8 sat_codon(i64 acc) {
        i64 q = acc >> 5;
        q = q < -32 ? -32 : (q > 31 ? 31 : q);
        return static_cast<u8>(q + 32);
    }
    // Reads n codons from a (advancing like a head) into out; false if any is
    // unreadable. *end is the address after the last.
    bool gather(u32 a, u32 n, u8* out, u32* end) {
        for (u32 i = 0; i < n; ++i) {
            if (!readable(a))
                return false;
            out[i] = take(a);
            a = next_addr(a);
        }
        *end = a;
        return true;
    }
    // Writes n codons from the write head on, all or nothing: every address
    // must be writable and accept the write (hot pages) before any is written.
    bool scatter(const u8* vals, u32 n) {
        u32 a = c.wh;
        for (u32 i = 0; i < n; ++i) {
            if (!writable(a) || !page_accepts(a))
                return false;
            a = next_addr(a);
        }
        a = c.wh;
        for (u32 i = 0; i < n; ++i) {
            const u8 was_ = s.mem_[a];
            s.mem_[a] = vals[i];
            note_write(a, was_);
            if (s.obs_)
                s.obs_->on_write(a);
            a = next_addr(a);
        }
        c.wh = a;
        return true;
    }

    // NPU: all-or-nothing ops on the body's NPU units, charged in multiply-
    // accumulates (npu.macs per unit per tick), plus gpu.setup and
    // npu.cpu_per_mac per MAC on the CPU. k = CX.
    //  - dot: CX <- sum of (source[i]-32)(dest[i]-32), signed; k MACs.
    //  - matvec: dest[j] <- sat(sum_i M[j][i] v[i] / 32), v at the read head,
    //    M (k x k, row-major) at AX; k <= 64; k^2 MACs.
    //  - matmul: dest <- sat(A B / 32), A at the read head, B at AX, both
    //    k x k; k <= 32; k^3 MACs.
    //  - argmax: CX <- offset of the largest source codon (the first of
    //    equals), read head left on it; k MACs.
    bool npu_op(u32 op, u32* cost) {
        const i32 n = static_cast<i32>(c.cx);
        if (n <= 0)
            return false;
        const u32 k = static_cast<u32>(n);
        u64 macs;
        if (op == kMatvec) {
            if (k > 64)
                return false;
            macs = static_cast<u64>(k) * k;
        } else if (op == kMatmul) {
            if (k > 32)
                return false;
            macs = static_cast<u64>(k) * k * k;
        } else {
            macs = k;
        }
        if (static_cast<double>(macs) > static_cast<double>(s.npu_.cap[c.body]))
            return false;
        if ((op == kMatvec || op == kMatmul) && c.ax >= p.size)
            return false;

        if (op == kDot) {
            u32 r = c.rh, w = c.wh;
            i64 acc = 0;
            for (u32 i = 0; i < k; ++i) {
                if (!readable(r) || !readable(w))
                    return false;
                acc += sval(take(r)) * sval(take(w));
                r = next_addr(r);
                w = next_addr(w);
            }
            c.rh = r;
            c.wh = w;
            c.cx = static_cast<u32>(static_cast<i32>(acc));
        } else if (op == kArgmax) {
            u32 r = c.rh, at = c.rh, best = 0;
            i32 top = -1;
            for (u32 i = 0; i < k; ++i) {
                if (!readable(r))
                    return false;
                const i32 v = take(r);
                if (v > top) {
                    top = v;
                    best = i;
                    at = r;
                }
                r = next_addr(r);
            }
            c.rh = at;
            c.cx = best;
        } else if (op == kMatvec) {
            u8 v[64], m[64 * 64], out[64];
            u32 v_end, m_end;
            if (!gather(c.rh, k, v, &v_end) || !gather(c.ax, k * k, m, &m_end))
                return false;
            for (u32 j = 0; j < k; ++j) {
                i64 acc = 0;
                for (u32 i = 0; i < k; ++i)
                    acc += sval(m[j * k + i]) * sval(v[i]);
                out[j] = sat_codon(acc);
            }
            if (!scatter(out, k))
                return false;
            c.rh = v_end;
        } else { // kMatmul
            u8 a[32 * 32], b[32 * 32], out[32 * 32];
            u32 a_end, b_end;
            if (!gather(c.rh, k * k, a, &a_end) || !gather(c.ax, k * k, b, &b_end))
                return false;
            for (u32 j = 0; j < k; ++j)
                for (u32 l = 0; l < k; ++l) {
                    i64 acc = 0;
                    for (u32 i = 0; i < k; ++i)
                        acc += sval(a[j * k + i]) * sval(b[i * k + l]);
                    out[j * k + l] = sat_codon(acc);
                }
            if (!scatter(out, k * k))
                return false;
            c.rh = a_end;
        }
        s.npu_.cap[c.body] -= static_cast<float>(macs);
        s.npu_.done[c.body] += static_cast<u32>(macs);
        s.tally_.npu_macs += macs;
        *cost = max_of<u32>(1, p.gpu_setup + static_cast<u32>(static_cast<double>(macs) * p.npu_cpu_per_mac));
        if (s.obs_)
            s.obs_->on_npu(s, c.block, s.tick_);
        return true;
    }

    // --- the step -------------------------------------------------------------

    void fault() {
        c.err = 1;
        if (c.errs < 255)
            ++c.errs;
        if (c.errs >= p.halt_after && !c.halted) {
            c.halted = 1;
            ++s.tally_.halts;
        }
    }

    // An execution flaw: one register comes out off by one (6.3).
    void flaw() {
        u32 delta = s.rng_.below(2) ? 1u : ~0u;
        switch (s.rng_.below(3)) {
        case 0:
            c.ax += delta;
            break;
        case 1:
            c.bx += delta;
            break;
        default:
            c.cx += delta;
            break;
        }
        ++s.tally_.flaws;
        s.flaw_left_ = -log(1.0 - s.rng_.unit()) / (p.flaw_rate * s.hazard_mult_);
    }

    // Disasters X10, an OS crash's instability: the instruction faults
    // spuriously (its error flag set, as IFERR sees it), and with the
    // weather's halt share the thread halts outright. Returns false.
    bool spurious() {
        const Weather& w = s.weather_;
        ++s.tally_.spurious_faults;
        s.fault_left_ = -log(1.0 - s.rng_.unit()) / w.faults;
        if (w.halts > 0 && s.rng_.chance(w.halts) && !c.halted) {
            c.halted = 1;
            ++s.tally_.halts;
        }
        return false;
    }

    // kProf (behaviour profiles, A3): the same step, also recording what the
    // instruction did into `prof`. The world sees exactly the same step.
    template <bool kProf = false> u32 step() {
        const u32 ip = c.ip;
        if (!resolving) { // (an action's end is the same instruction, counted once)
            ++c.executed;
            ++s.tally_.instructions;
            if (p.heat_on)
                ++s.page_fetch_[ip >> kPageBits];
        }
        if (!p.open_membrane) {
            if (!own(ip)) { // not my code: private code can't be executed
                if constexpr (kProf) // (A3)
                    if (!resolving)
                        profile_foreign(*prof);
                fault();
                c.ip = next_ip(ip);
                return p.cost.timed ? costs().alu : 1;
            }
            c.block = s.owner_[ip];
        }

        u32 next = next_ip(ip);
        u32 cost = 1;
        // At an action's end, its instruction (whatever has since been
        // written over it).
        const Op op = resolving ? static_cast<Op>(resolving->op) : p.table.op[s.mem_[ip]];
        t_ps = 0;
        t_mem = 0;                                         // memory tiers (W3)
        const u64 own = p.cost.timed ? own_part(op) : 0;   // (W3: with its heads as they are before it runs)
        [[maybe_unused]] ProfileProbe probe; // (A3: filled only by step<true>)
        if constexpr (kProf) {
            probe.before(s.tally_, c.bx, next, s.owner_[ip]);
            self_load = false;
        }
        bool ok = exec_op(op, ip, &next, &cost);
        if constexpr (kProf) // (A3) what it did, before anything below can change ok
            profile_step(*prof, probe, s.tally_, op, ip, ok, next, c.cx, resolving != nullptr, self_load);
        if (deferred) // Phase 9: it started an action; the CPU waits here, at it, until its end
            return 0;  // (an ENGULF or INJECT keeps it busy: the rest of its tick goes on it, run_cpu)
        if (stalled) { // gestation (G1): it waits at the DIVIDE, idle, for the rest of the tick
            stalled = false;
            c.yielded = 1;
            c.ip = ip;
            return p.cost.timed ? costs().alu : 1;
        }
        if (p.cost.timed) { // Phase 9: world time (the GPU verb adds its devices' own, 9b)
            const u64 t = own + extra_ps();
            cost = t > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(t);
        }

        s.flaw_left_ -= errm;
        if (s.flaw_left_ <= 0)
            flaw();
        if (s.faults_on_ && (s.fault_left_ -= 1.0) <= 0) // disasters X10: an OS crash brewing
            ok = spurious();
        if (ok) {
            c.err = 0;
            c.errs = 0;
        } else {
            fault();
        }
        c.ip = next;
        return cost;
    }

    // What one instruction does (the step's switch; a warp's host instruction
    // runs it too, Phase 8). *next is the address after the instruction on
    // entry and where the IP goes on exit; *cost is what it charges.
    bool exec_op(Op op, u32 ip, u32* pnext, u32* pcost) {
        u32 next = *pnext;
        u32 cost = *pcost;
        bool ok = true;
        switch (op) {
        case Op::nop0:
        case Op::nop1:
            break;

        // --- Nova core ---
        case Op::jmp:
            ok = jump(ip, 0, &next);
            break;
        case Op::ifz:
            if (c.cx != 0)
                next = next_ip(next);
            break;
        case Op::inc:
            ++c.cx;
            break;
        case Op::dec:
            --c.cx;
            break;
        case Op::zero:
            c.cx = 0;
            break;
        case Op::rot: { // AX -> BX -> CX -> AX
            u32 a = c.ax, b = c.bx, x = c.cx;
            c.bx = a;
            c.cx = b;
            c.ax = x;
            break;
        }
        case Op::push:
            ok = push(c.cx);
            break;
        case Op::pop:
            ok = pop(&c.cx);
            break;
        case Op::copy:
            ok = copy();
            break;
        case Op::find: {
            u8 bits[64];
            u32 n = read_template(next, bits);
            next = skip(ip, n);
            u32 at = n ? search(ip, bits, n, 0) : kNone;
            ok = at != kNone;
            if (ok)
                c.ax = at;
            break;
        }
        case Op::iflt:
            if (!(static_cast<i32>(c.ax) < static_cast<i32>(c.bx)))
                next = next_ip(next);
            break;
        case Op::sense:
            ok = sense();
            break;
        case Op::alloc:
            ok = alloc();
            break;
        case Op::divide: {
            // Gestation (G1): before she is ripe the DIVIDE waits, at itself,
            // costing nothing; it runs again when its thread next runs.
            if (p.gestation > 0 && !resolving && !ripe()) {
                next = ip;
                stalled = true;
                break;
            }
            // BX is the destination: 0 = here, k = neighbour k (nearest first).
            // BX 0-3 go through the selector map; BX >= 4 names neighbour BX
            // directly (Phase 3). Without dispersal anything but here fails.
            i32 share = static_cast<i32>(c.cx);
            share = share < 0 ? 0 : (share > 64 ? 64 : share);
            const u32 dest = c.bx < 4 ? p.sel.divide[c.bx] : c.bx;
            if (dest == 0)
                ok = divide(static_cast<u32>(share));
            else
                ok = p.dispersal && disperse(static_cast<u32>(share), dest);
            break;
        }

        // --- Nova rich ---
        case Op::call: {
            u8 bits[64];
            u32 n = read_template(next, bits);
            u32 ret = skip(ip, n);
            u32 to = n ? search(ip, bits, n, 0) : kNone;
            ok = to != kNone && push(ret);
            next = ok ? to : ret;
            if (ok)
                t_ps += costs().jump;
            break;
        }
        case Op::ret: {
            u32 v;
            ok = pop(&v);
            if (ok) {
                next = v % p.size;
                t_ps += costs().jump;
            }
            break;
        }
        case Op::djnz:
            --c.cx;
            if (c.cx != 0) {
                ok = jump(ip, 0, &next);
            } else {
                u8 bits[64];
                next = skip(ip, read_template(next, bits));
            }
            break;
        case Op::jz:
            if (c.cx == 0) {
                ok = jump(ip, 0, &next);
            } else {
                u8 bits[64];
                next = skip(ip, read_template(next, bits));
            }
            break;
        case Op::add:
            c.cx = c.ax + c.bx;
            break;
        case Op::sub:
            c.cx = c.ax - c.bx;
            break;
        case Op::mul:
            c.cx = c.ax * c.bx;
            break;
        case Op::div: {
            i32 a = static_cast<i32>(c.ax), b = static_cast<i32>(c.bx);
            if (b == 0)
                ok = false;
            else
                c.cx = (a == static_cast<i32>(0x80000000u) && b == -1) ? c.ax
                                                                       : static_cast<u32>(a / b);
            break;
        }
        case Op::and_:
            c.cx = c.ax & c.bx;
            break;
        case Op::or_:
            c.cx = c.ax | c.bx;
            break;
        case Op::xor_:
            c.cx = c.ax ^ c.bx;
            break;
        case Op::not_:
            c.cx = ~c.cx;
            break;
        case Op::shl:
            c.cx <<= 1;
            break;
        case Op::shr:
            c.cx >>= 1;
            break;
        case Op::neg:
            c.cx = 0u - c.cx;
            break;
        case Op::rand: // a random address-sized value
            c.cx = static_cast<u32>(s.rng_.below(p.size));
            break;
        case Op::ifne:
            if (c.ax == c.bx)
                next = next_ip(next);
            break;
        case Op::iferr: // tests the previous instruction's flag
            if (!c.err)
                next = next_ip(next);
            break;
        case Op::swap: {
            u32 t = c.ax;
            c.ax = c.bx;
            c.bx = t;
            break;
        }
        case Op::rotb: { // AX -> CX -> BX -> AX
            u32 a = c.ax, b = c.bx, x = c.cx;
            c.ax = b;
            c.bx = x;
            c.cx = a;
            break;
        }
        case Op::seth: // BX -> head through the selector map (D6)
            if (c.ax >= p.size || c.bx > 1)
                ok = false;
            else if (p.sel.head[c.bx] == 0)
                c.rh = c.ax;
            else
                c.wh = c.ax;
            break;
        case Op::geth:
            if (c.bx > 1)
                ok = false;
            else
                c.ax = p.sel.head[c.bx] == 0 ? c.rh : c.wh;
            break;
        case Op::read:
            ok = readable(c.rh);
            if (ok) {
                c.cx = take(c.rh);
                c.rh = next_addr(c.rh);
            }
            break;
        case Op::write:
            ok = writable(c.wh) && page_accepts(c.wh);
            if (ok) {
                const u8 was_ = s.mem_[c.wh];
                s.mem_[c.wh] = static_cast<u8>(c.cx & 63);
                note_write(c.wh, was_);
                if (s.obs_)
                    s.obs_->on_write(c.wh);
                c.wh = next_addr(c.wh);
            }
            break;
        case Op::engulf: // Phase 9: an action with a timed table (contact_start, contact_end)
            cost = resolving ? contact_end(ip, true, &next, &ok)
                             : (acts() ? contact_start(ip, true, &next, &ok) : contact(ip, true, &next, &ok));
            break;
        case Op::inject:
            cost = resolving ? contact_end(ip, false, &next, &ok)
                             : (acts() ? contact_start(ip, false, &next, &ok) : contact(ip, false, &next, &ok));
            break;
        case Op::emit: // the signal channel (Phase 4): adds CX to channel BX
            ok = c.bx < p.signal_channels;
            if (ok) {
                const i32 amp = static_cast<i32>(c.cx);
                const u64 mag = amp < 0 ? static_cast<u64>(-static_cast<i64>(amp)) : static_cast<u64>(amp);
                cost = max_of<u32>(1, p.signal_cost_base +
                                          static_cast<u32>(min_of(p.signal_cost_per_amp * mag, 4e9)));
                s.emit_[c.bx] += amp;
                ++s.emit_count_[c.bx];
                s.emit_amp_[c.bx] += mag;
                ++s.tally_.emits;
                if (s.obs_)
                    s.obs_->on_emit(s, c.block, s.tick_);
            }
            break;
        case Op::listen: // CX <- channel BX's field on my node this tick
            ok = c.bx < p.signal_channels;
            if (ok) {
                c.cx = static_cast<u32>(s.field_[c.bx]);
                ++s.tally_.listens;
            }
            break;
        case Op::disk: // the disk tier (Phase 4)
            if (p.cost.timed && !resolving) { // Phase 9: its time; longer than a tick, an action
                const u64 t = disk_time();
                if (acts() && static_cast<double>(t) > p.tick_units) {
                    start_long(op, t);
                    break;
                }
                t_ps += t;
            }
            ok = disk_op(&cost);
            break;
        case Op::gpu: // the accelerator verb: GPU ops, NPU ops (Phase 2, 2b), launch (Phase 8), threads (9c)
            if (p.threads && c.bx == kThreadOp)
                ok = spawn(ip, &next);
            else if (p.threads && c.bx == kKindOp)
                ok = prefer_kind();
            else if (p.warps && c.bx == kLaunchOp)
                ok = launch(ip, &next, &cost);
            else
                ok = accel(&cost);
            break;
        case Op::yield:
            c.yielded = 1;
            if (p.sleep) // docs/lifehistory-plan.md V1: a sleep, with BX 1 or 2 and CX > 0
                ask_sleep();
            break;
        case Op::release:
            ok = release();
            break;

        // --- Tierra instruction set 0 (Ray's V3+ documented semantics) ---
        case Op::t_not0:
            c.cx ^= 1;
            break;
        case Op::t_shl:
            c.cx <<= 1;
            break;
        case Op::t_zero:
            c.cx = 0;
            break;
        case Op::t_ifz:
            if (c.cx != 0)
                next = next_ip(next);
            break;
        case Op::t_subcab:
            c.cx = c.ax - c.bx;
            break;
        case Op::t_subaac:
            c.ax = c.ax - c.cx;
            break;
        case Op::t_inca:
            ++c.ax;
            break;
        case Op::t_incb:
            ++c.bx;
            break;
        case Op::t_decc:
            --c.cx;
            break;
        case Op::t_incc:
            ++c.cx;
            break;
        case Op::t_pushax:
            ok = push(c.ax);
            break;
        case Op::t_pushbx:
            ok = push(c.bx);
            break;
        case Op::t_pushcx:
            ok = push(c.cx);
            break;
        case Op::t_pushdx:
            ok = push(c.dx);
            break;
        case Op::t_popax:
            ok = pop(&c.ax);
            break;
        case Op::t_popbx:
            ok = pop(&c.bx);
            break;
        case Op::t_popcx:
            ok = pop(&c.cx);
            break;
        case Op::t_popdx:
            ok = pop(&c.dx);
            break;
        case Op::t_jmpo:
            ok = jump(ip, 0, &next);
            break;
        case Op::t_jmpb:
            ok = jump(ip, -1, &next);
            break;
        case Op::t_call: { // pushes the address after its template
            u8 bits[64];
            u32 n = read_template(next, bits);
            u32 ret = skip(ip, n);
            u32 to = n ? search(ip, bits, n, 0) : kNone;
            ok = to != kNone && push(ret);
            next = ok ? to : ret;
            if (ok)
                t_ps += costs().jump;
            break;
        }
        case Op::t_ret: {
            u32 v;
            ok = pop(&v);
            if (ok) {
                next = v % p.size;
                t_ps += costs().jump;
            }
            break;
        }
        case Op::t_movdc:
            c.dx = c.cx;
            break;
        case Op::t_movba:
            c.bx = c.ax;
            break;
        case Op::t_movii: { // [AX] <- [BX]
            u32 to = c.ax % p.size, from = c.bx % p.size;
            ok = readable(from) && writable(to) && page_accepts(to);
            if (ok) {
                const u8 was_ = s.mem_[to];
                s.mem_[to] = copied(take(from));
                note_write(to, was_);
                if (s.obs_)
                    s.obs_->on_write(to);
            }
            break;
        }
        case Op::t_adro:
        case Op::t_adrb:
        case Op::t_adrf: { // AX = address after the match, CX = template size
            u8 bits[64];
            u32 n = read_template(next, bits);
            next = skip(ip, n);
            int dir = op == Op::t_adro ? 0 : (op == Op::t_adrb ? -1 : 1);
            u32 at = n ? search(ip, bits, n, dir) : kNone;
            ok = at != kNone;
            if (ok) {
                c.ax = at;
                c.cx = n;
            }
            break;
        }
        case Op::t_mal: { // CX codons; AX = their address
            i32 amount = static_cast<i32>(c.cx);
            t_ps += costs().call; // Phase 9: a world change
            u32 at = amount > 0 ? alloc_memory(static_cast<u32>(amount)) : kNone;
            ok = at != kNone;
            if (ok)
                c.ax = at;
            break;
        }
        case Op::t_divide: // no amount operand: the share is a config value
            ok = divide(p.tierra_share);
            break;

        // --- Phase 1b plain-64 additions (design.md 6.6, D7) ---
        case Op::nopx: // inert, and not a template bit: it ends templates
            break;
        case Op::jmpf: // template jumps in one direction only
            ok = jump(ip, 1, &next);
            break;
        case Op::jmpb:
            ok = jump(ip, -1, &next);
            break;
        case Op::ifeq:
            if (c.ax != c.bx)
                next = next_ip(next);
            break;
        case Op::ifgt:
            if (!(static_cast<i32>(c.ax) > static_cast<i32>(c.bx)))
                next = next_ip(next);
            break;
        case Op::mod: { // CX = AX mod BX, signed like DIV
            i32 a = static_cast<i32>(c.ax), b = static_cast<i32>(c.bx);
            if (b == 0)
                ok = false;
            else
                c.cx = (a == static_cast<i32>(0x80000000u) && b == -1) ? 0u : static_cast<u32>(a % b);
            break;
        }
        case Op::min_:
            c.cx = static_cast<i32>(c.ax) < static_cast<i32>(c.bx) ? c.ax : c.bx;
            break;
        case Op::max_:
            c.cx = static_cast<i32>(c.ax) > static_cast<i32>(c.bx) ? c.ax : c.bx;
            break;
        case Op::dup: // push the top of the stack again
            ok = c.sp > 0 && push(c.stack[c.sp - 1]);
            break;
        case Op::drop: {
            u32 v;
            ok = pop(&v);
            break;
        }
        case Op::inca:
            ++c.ax;
            break;
        case Op::deca:
            --c.ax;
            break;
        case Op::incb:
            ++c.bx;
            break;
        case Op::decb:
            --c.bx;
            break;
        case Op::zeroa:
            c.ax = 0;
            break;
        case Op::zerob:
            c.bx = 0;
            break;
        case Op::inert: // Phase 10: dead matter, not an instruction (nova_v2's codon 0)
        case Op::count:
            ok = false;
            break;
        }
        *pnext = next;
        *pcost = cost;
        return ok;
    }
};

i64 Soup::run_cpu(u32 ci, i64 quota, u8 kind) {
    Exec e{*this, *p_, cpus_[ci], bodies_[cpus_[ci].body].err, ci};
    if (kind == kKindE) // Phase 9c: on an E-core, at its costs
        e.ct = &p_->ecost;
    e.tiers_setup(); // memory tiers (W3): this core's prices per tier
    // Behaviour profiles (A3): a followed body's thread runs through step<true>.
    e.prof = prof_ ? prof_->watching(*this, e.c.body) : nullptr;
    i64 used = 0;
    const u64 ran_before = e.c.executed; // Phase 10: a body that runs isn't frozen
    // Phase 9: an action under way. Before its end the CPU waits out the tick
    // (its time is spent: it heats, as work does); at its end it lands or
    // fails first, then the CPU runs on. A contact action whose target is
    // gone ends at once.
    if (!actions_.empty()) {
        const u32 ai = action_of(ci);
        if (ai != kNone) {
            const Action& a = actions_[ai];
            const bool gone = a.target != kNone &&
                              (!bodies_[a.target].alive || bodies_[a.target].serial != a.target_serial);
            if (a.end > tick_ && !gone) // an ENGULF is the CPU at work; a wait leaves it idle (9c, bites plan)
                return busy_action(a) ? quota : 0;
            const Action act = a;
            for (usize j = ai; j + 1 < actions_.size(); ++j) // in start order, still
                actions_[j] = actions_[j + 1];
            actions_.resize(actions_.size() - 1);
            e.resolving = &act;
            if (e.prof) // (A3)
                e.step<true>();
            else
                e.step();
            e.resolving = nullptr;
            if (e.c.err)
                ++tally_.actions_failed;
            else
                ++tally_.actions_landed;
        }
    }
    if (e.prof) // behaviour profiles (A3): the same loop, recording
        while (used < quota && !e.c.halted && !e.c.yielded && !e.deferred)
            used += e.step<true>();
    else
        while (used < quota && !e.c.halted && !e.c.yielded && !e.deferred)
            used += e.step();
    if (e.deferred && used < quota && busy_action(actions_.back()))
        used = quota; // the rest of the tick goes on the ENGULF; a wait is idle (9c, bites plan)
    if (e.c.executed != ran_before && e.c.body < last_ran_.size())
        last_ran_[e.c.body] = tick_;
    // ENGULF may have appended the prey's CPUs after this one: keep the link.
    u32 next = cpus_[ci].next;
    cpus_[ci] = e.c;
    cpus_[ci].next = next;
    return used;
}

// The body's and the node's SENSE readings (design.md 6.5): everything but
// where a CPU or lane runs (readings 0, 4, 20, 21). ci is the CPU slot for
// the asynchronous accelerator's readings (kNone for a warp: they read 0).
// SENSE 22 and 23 from the soonest action on a body (action_on; kNone: none).
u32 Soup::action_reading(u32 r, u32 i) const {
    if (i == kNone)
        return 0;
    const Action& a = actions_[i];
    if (r == 22) {
        const u64 left = a.end > tick_ ? a.end - tick_ : 1;
        return left > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(left);
    }
    return a.op == static_cast<u32>(Op::engulf) ? kActEngulf : kActInject;
}

bool Soup::sense_reading(u32 b, u32 r, u32 ci, u32* v) const {
    const Body& B = bodies_[b];
    switch (r) {
    case 1:
        *v = B.mem;
        return true;
    case 2:
        *v = B.units;
        return true;
    case 3: { // mean heat of my CPU units
        double sum = 0;
        for (u32 u = B.unit_head; u != kNone; u = unit_next_[u])
            sum += unit_heat_[u];
        *v = B.units ? static_cast<u32>(sum / B.units) : 0;
        return true;
    }
    case 5:
        *v = static_cast<u32>(pool_.size());
        return true;
    case 6:
        *v = waste_;
        return true;
    case 7:
        *v = B.ncpu;
        return true;
    // Phase 2 and 2b: 8-10 for the GPU, 11-13 for the NPU, each failing
    // when the node lacks that unit.
    case 8: // my units
    case 11: {
        const UnitLedger* L = r < 11 ? (p_->gpus ? &gpu_ : nullptr) : (p_->npus ? &npu_ : nullptr);
        if (!L)
            return false;
        *v = L->count[b];
        return true;
    }
    case 9: // mean heat of my units
    case 12: {
        const UnitLedger* L = r < 11 ? (p_->gpus ? &gpu_ : nullptr) : (p_->npus ? &npu_ : nullptr);
        if (!L)
            return false;
        const u32 n = L->count[b];
        double sum = 0;
        for (u32 u = L->head[b]; u != kNone; u = L->next[u])
            sum += L->heat[u];
        *v = n ? static_cast<u32>(sum / n) : 0;
        return true;
    }
    case 10: // free units on the node
    case 13: {
        const UnitLedger* L = r < 11 ? (p_->gpus ? &gpu_ : nullptr) : (p_->npus ? &npu_ : nullptr);
        if (!L)
            return false;
        *v = static_cast<u32>(L->pool.size());
        return true;
    }
    case 14: // Phase 3: my node's degree (neighbours I could disperse to)
        if (!p_->dispersal)
            return false;
        *v = ports_.degree;
        return true;
    case 15: // Phase 4: my disk codons (W4: both tiers, the range DISK addresses)
        if (!disk_on())
            return false;
        *v = disk_.total[b];
        return true;
    case 16: // free disk codons on the node (W4: the fast disk's; the archive has no reading)
        if (!disk_on())
            return false;
        *v = disk_.fast_free();
        return true;
    case 17: // node health 0-100 (the lifecycle): leaving a dying node can evolve
        if (!lifecycle())
            return false;
        *v = health();
        return true;
    case 18: // Phase 5: my last accelerator result (async ops)
        if (!p_->accel_async)
            return false;
        *v = accel_result(ci);
        return true;
    case 19: // my accelerator ops still pending
        if (!p_->accel_async)
            return false;
        *v = accel_pending(ci);
        return true;
    case 22: // Phase 9: ticks until the soonest action on me ends (0: none)
    case 23: // ... and what it is (1 ENGULF, 2 INJECT)
        if (!p_->cost.timed)
            return false;
        *v = action_reading(r, action_on(b));
        return true;
    case 24: { // Phase 9c: my threads still running
        if (!kinds_on())
            return false;
        u32 n = 0;
        for (u32 c = B.cpus; c != kNone; c = cpus_[c].next)
            n += cpus_[c].halted ? 0 : 1;
        *v = n;
        return true;
    }
    case 25: // the kind of core this thread prefers (0 P, 1 E; a warp reads 0)
        if (!kinds_on())
            return false;
        *v = ci == kNone ? kKindP : cpu_kind(ci);
        return true;
    case 26: // my E millicores (9c-b2; 0 where there are none)
        if (!kinds_on())
            return false;
        *v = e_units(b);
        return true;
    case 27: // free E millicores on the node
        if (!kinds_on())
            return false;
        *v = static_cast<u32>(epool_.size());
        return true;
    default:
        return false;
    }
}

// A warp's host instruction (Phase 8, design.md 6.14): the warp stopped in
// front of a world-changing instruction during the GPU phase; it is done here,
// once, with the registers of the lowest lane at it, as a CPU would do it.
// Every lane at the instruction gets the result: registers it changed take
// its new value, heads it moved move by as much (keeping each lane's offset),
// and the error flag. Its cost comes from the warp's capacity at the serial
// rate, as a debt carried into next tick.
//
// Phase 9b, timed tables: the warp then waits a GPU round trip and the host's
// time for the instruction (a GPU program asking the CPU for something). Its
// ENGULF, INJECT and long instructions are actions: begun here (the warp
// waits at the instruction until their end), finished by resolve_warp_actions.
bool Soup::host_step(u32 w, const Action* resolving) {
    const u32 L = warps_[w].lanes; // its own lanes (docs/lanes-plan.md L1)
    const u32 body = warps_[w].body;
    NOVA_ASSERT(wstate_[w] != kLanesDevice); // the world fetched them from the GPU
    u32 minip = kNone, lowest = kNone;
    for (u32 l = 0; l < L; ++l) {
        const Lane& x = lane(w, l);
        if (x.halted || x.yielded)
            continue;
        if (x.ip < minip) {
            minip = x.ip;
            lowest = l;
        }
    }
    if (minip == kNone)
        return false;
    const u32 o = owner_[minip];
    const u32 block = o != kNone && blocks_[o].body == body ? o : kNone;
    for (u32 l = 0; l < L; ++l) { // the step sets the block the lanes run in
        Lane& x = lane_mut(w, l);
        if (!x.halted && !x.yielded && x.ip == minip)
            x.block = block;
    }
    const Lane& lo = lane(w, lowest);
    Cpu t = {};
    t.ax = lo.ax;
    t.bx = lo.bx;
    t.cx = lo.cx;
    t.dx = lo.dx;
    for (u32 i = 0; i < kMaxStack; ++i)
        t.stack[i] = lo.stack[i];
    t.sp = lo.sp;
    t.ip = minip;
    t.rh = lo.rh;
    t.wh = lo.wh;
    t.body = body;
    t.block = block;
    t.next = kNone;
    t.err = static_cast<u8>(lo.err);
    if (p_->heat_on)
        ++page_fetch_[minip >> kPageBits];
    const float gerr = gpu_count(body) ? gpu_.err[body] : 1.0f;
    Exec e{*this, *p_, t, gerr, kNone, true};
    e.wslot = w;
    e.resolving = resolving;
    e.tiers_setup(); // memory tiers (W3): the host's (a P-core's) prices per tier
    u32 next = e.next_ip(minip);
    u32 cost = 1;
    const Op op = resolving ? static_cast<Op>(resolving->op) : p_->table.op[mem_[minip]];
    // Behaviour profiles (A3): a followed body's warp host instruction is
    // recorded as a thread's is (observe-only).
    ProfileSample* const prof = prof_ ? prof_->watching(*this, body) : nullptr;
    ProfileProbe probe = {};
    if (prof)
        probe.before(tally_, e.c.bx, next, block);
    const bool ok = e.exec_op(op, minip, &next, &cost);
    if (prof)
        profile_step(*prof, probe, tally_, op, minip, ok, next, e.c.cx, resolving != nullptr, e.self_load);
    if (e.stalled) { // gestation (G1, G4): the warp waits at its DIVIDE, lanes unchanged, until she is ripe
        if (p_->cost.timed)
            wuntil_[w] = e.ripe_at;
        host_stalled_ = true;
        warps_[w].wait = 0;
        ++tally_.warp_host;
        return true;
    }
    if (e.deferred) { // Phase 9b: an action began; the warp waits at the instruction until its end
        wuntil_[w] = actions_.back().end;
        warps_[w].wait = 0;
        ++tally_.warp_host;
        return true;
    }
    const i64 size = p_->size;
    auto moved = [size](u32 h, u32 before, u32 after) {
        const i64 d = static_cast<i32>(after - before);
        const i64 v = ((static_cast<i64>(h) + d) % size + size) % size;
        return static_cast<u32>(v);
    };
    // A launch may have grown the lanes (no references kept across it).
    for (u32 l = 0; l < L; ++l) {
        Lane& x = lane_mut(w, l);
        if (x.halted || x.yielded || x.ip != minip)
            continue;
        if (e.c.ax != t.ax)
            x.ax = e.c.ax;
        if (e.c.bx != t.bx)
            x.bx = e.c.bx;
        if (e.c.cx != t.cx)
            x.cx = e.c.cx;
        if (e.c.dx != t.dx)
            x.dx = e.c.dx;
        if (e.c.rh != t.rh)
            x.rh = moved(x.rh, t.rh, e.c.rh);
        if (e.c.wh != t.wh)
            x.wh = moved(x.wh, t.wh, e.c.wh);
        if (e.c.yielded) // V5: a YIELD run by the host (life.sleep) yields its lanes as a lane's does
            x.yielded = 1;
        if (ok) {
            x.err = 0;
            x.errs = 0;
        } else {
            x.err = 1;
            if (x.errs < 255)
                ++x.errs;
            if (x.errs >= p_->halt_after && !x.halted) {
                x.halted = 1;
                ++tally_.halts;
            }
        }
        x.ip = next;
    }
    const u32 now = warps_[w].body; // ENGULF can't move this warp, but be sure
    if (now != kNone) {
        if (p_->cost.timed) { // Phase 9b: the round trip and the host's time, as whole ticks
            if (resolving) {
                wuntil_[w] = tick_; // an action's end: its time was the wait
            } else {
                const u64 ps = p_->cost.gpu_start + p_->cost.op[static_cast<u32>(op)] + e.extra_ps(); // (W3: t_mem too)
                const double ticks = static_cast<double>(ps) / p_->tick_units;
                u64 whole = static_cast<u64>(ticks);
                if (static_cast<double>(whole) < ticks)
                    ++whole;
                wuntil_[w] = tick_ + (whole < 1 ? 1 : whole);
            }
        } else {
            const double serial = static_cast<double>(warp_step_price(warps_[w].lanes)) + p_->warp_lane_cost;
            warp_carry_[now] -= static_cast<float>(static_cast<double>(cost) * serial);
        }
        count_lanes(w);
        wstate_[w] = kLanesHost; // changed here: the GPU's copy is stale
    }
    warps_[w].wait = 0;
    ++tally_.warp_host;
    return ok;
}

} // namespace nova
