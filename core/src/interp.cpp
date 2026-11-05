// The interpreter: what each Op does (design.md 6.2 and 6.5).
//
// Exec runs one CPU on a local copy of its state and writes it back at the
// end. A copy, not a reference, because DIVIDE appends to the CPU and body
// arrays and may move them.
//
// Every instruction charges at least 1 against the CPU's quota (ENGULF and
// INJECT charge more) and sets the error flag to whether it failed; K
// consecutive failures halt the CPU (6.4 #1).
#include <cmath> // log

#include "nova/soup.h"

namespace nova {

struct Exec {
    Soup& s;
    const SoupParams& p;
    Cpu c;
    float errm; // the body's hazard multiplier this tick

    // --- addresses -----------------------------------------------------------

    bool own(u32 a) const {
        u32 k = s.owner_[a];
        return k != kNone && s.blocks_[k].body == c.body;
    }
    // Private code: read your own body or waste. Open membrane: anything.
    bool readable(u32 a) const {
        if (p.open_membrane)
            return true;
        u32 k = s.owner_[a];
        return k == kNone || s.blocks_[k].body == c.body;
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
    // The address after an instruction at a and its n-codon template.
    u32 skip(u32 a, u32 n) const {
        for (u32 i = 0; i <= n; ++i)
            a = next_ip(a);
        return a;
    }

    // --- templates -------------------------------------------------------------

    // The run of NOPs starting at `from` (at most max_template), as bits.
    u32 read_template(u32 from, u8* bits) const {
        u32 n = 0;
        u32 a = from;
        while (n < p.max_template) {
            if (!p.open_membrane && !own(a))
                break;
            Op op = p.table.op[s.mem_[a]];
            if (!is_nop(op))
                break;
            bits[n++] = op == Op::nop1;
            a = next_ip(a);
        }
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
            fmax = min_of<i64>(fmax, hi - n - static_cast<i64>(a) - n);
            bmax = min_of<i64>(bmax, static_cast<i64>(a) - n + 1 - lo);
        }
        const i64 dmax = max_of<i64>(dir >= 0 ? fmax : 0, dir <= 0 ? bmax : 0);
        for (i64 d = 1; d <= dmax; ++d) {
            if (dir >= 0 && d <= fmax) {
                i64 st = static_cast<i64>(a) + n + d;
                if (p.open_membrane)
                    st %= size;
                if (st + n <= size || p.open_membrane) {
                    if (complement_at(static_cast<u32>(st), bits, n))
                        return next_addr(static_cast<u32>((st + n - 1) % size));
                }
            }
            if (dir <= 0 && d <= bmax) {
                i64 st = static_cast<i64>(a) - n - d + 1;
                if (p.open_membrane)
                    st = ((st % size) + size) % size;
                if (st >= 0) {
                    if (complement_at(static_cast<u32>(st), bits, n))
                        return next_addr(static_cast<u32>((st + n - 1) % size));
                }
            }
        }
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

    void note_write(u32 a) {
        if (p.heat_on)
            ++s.page_write_[a >> kPageBits];
    }

    // A copied codon, with the heat-scaled chance of a one-bit copy error.
    u8 copied(u8 v) {
        s.copy_left_ -= errm;
        if (s.copy_left_ <= 0) {
            v ^= static_cast<u8>(1u << s.rng_.below(6));
            ++s.tally_.copy_errors;
            s.copy_left_ = -log(1.0 - s.rng_.unit()) / p.copy_rate;
        }
        return v;
    }

    bool copy() {
        u32 from = c.rh, to = c.wh;
        if (!readable(from) || !writable(to) || !page_accepts(to))
            return false;
        s.mem_[to] = copied(s.mem_[from]);
        note_write(to);
        c.rh = next_addr(from);
        c.wh = next_addr(to);
        return true;
    }

    // --- resources -----------------------------------------------------------

    // Claims len codons of waste as a new block of this body: the nearest free
    // run outward from the block the CPU runs in (forward on a tie).
    u32 alloc_memory(u32 len) {
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
    bool divide(u32 share) {
        u32 parent = c.body;
        u32 k = s.bodies_[parent].latest;
        if (k == kNone)
            return false;
        u32 d = s.new_body(); // may move bodies_
        unlink_block(parent, k);
        Body& P = s.bodies_[parent];
        Body& D = s.bodies_[d];
        P.latest = kNone;
        P.mem -= s.blocks_[k].owned;
        s.blocks_[k].body = d;
        D.blocks = k;
        D.mem = s.blocks_[k].owned;
        u32 give = static_cast<u32>(static_cast<u64>(P.units) * share / 64);
        s.new_cpu(d, s.blocks_[k].start);
        s.move_units(parent, d, give);
        forget(k);
        ++s.tally_.births;
        return true;
    }

    bool alloc() {
        i32 amount = static_cast<i32>(c.cx);
        switch (c.bx) {
        case 0: // memory
            return amount <= 0 || alloc_memory(static_cast<u32>(amount)) != kNone;
        case 1: // CPU, capped by what the pool holds
            return amount <= 0 || s.claim_units(c.body, static_cast<u32>(amount)) > 0;
        default: // 2 GPU, 3 disk: off in Phase 1a
            return false;
        }
    }

    bool release() {
        i32 amount = static_cast<i32>(c.cx);
        switch (c.bx) {
        case 0: { // memory: the end of the latest block
            u32 k = s.bodies_[c.body].latest;
            if (k == kNone)
                return false;
            if (amount <= 0)
                return true;
            Block& bl = s.blocks_[k];
            if (static_cast<u32>(amount) >= bl.len) {
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
            if (s.bodies_[c.body].units == 0)
                return false;
            s.release_units(c.body, static_cast<u32>(amount));
            return true;
        }
        default:
            return false;
        }
    }

    bool sense() {
        const Body& B = s.bodies_[c.body];
        switch (c.bx) {
        case 0: // my size: the block I run in
            c.cx = c.block == kNone ? 0 : s.blocks_[c.block].len;
            return true;
        case 1:
            c.cx = B.mem;
            return true;
        case 2:
            c.cx = B.units;
            return true;
        case 3: { // mean heat of my CPU units
            double sum = 0;
            for (u32 u = B.unit_head; u != kNone; u = s.unit_next_[u])
                sum += s.unit_heat_[u];
            c.cx = B.units ? static_cast<u32>(sum / B.units) : 0;
            return true;
        }
        case 4: // heat of the memory page I run in
            c.cx = static_cast<u32>(s.page_heat_[c.ip >> kPageBits]);
            return true;
        case 5:
            c.cx = static_cast<u32>(s.pool_.size());
            return true;
        case 6:
            c.cx = s.waste_;
            return true;
        case 7:
            c.cx = B.ncpu;
            return true;
        default:
            return false;
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
    // with probability matches / max(n, m), where 0/0 counts as 1: a surface
    // shields against blind attacks but can be aimed at. Returns the target body
    // on success, kNone on failure.
    u32 contest(u32 ip, const u8* bits, u32 n) {
        u32 best = kNone, best_match = 0, best_len = 0, best_dist = 0;
        u8 sb[64];
        for (usize i = 0; i < s.order_.size(); ++i) {
            u32 t = s.order_[i];
            if (t == c.body || !s.bodies_[t].alive || s.bodies_[t].blocks == kNone)
                continue;
            u32 start = s.blocks_[s.bodies_[t].blocks].start;
            u32 dist = start > ip ? start - ip : ip - start;
            if (dist > p.contact_range)
                continue;
            u32 m = surface(t, sb);
            u32 len = n > m ? n : m;
            u32 match = 0;
            for (u32 j = 0; j < n && j < m; ++j)
                match += bits[j] != sb[j];
            // Score match/len, with 0/0 = 1/1.
            u64 num = len ? match : 1, den = len ? len : 1;
            u64 bnum = best_len ? best_match : 1, bden = best_len ? best_len : 1;
            bool better = best == kNone || num * bden > bnum * den ||
                          (num * bden == bnum * den && dist < best_dist);
            if (better) {
                best = t;
                best_match = match;
                best_len = len;
                best_dist = dist;
            }
        }
        if (best == kNone)
            return kNone;
        bool win = best_len == 0 || s.rng_.below(best_len) < best_match;
        return win ? best : kNone;
    }

    void engulf_body(u32 prey) {
        u32 eater = c.body;
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
            u32 t = E.cpus;
            while (s.cpus_[t].next != kNone)
                t = s.cpus_[t].next;
            s.cpus_[t].next = P.cpus;
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
        }
        P.blocks = P.cpus = P.latest = kNone;
        P.unit_head = P.unit_tail = kNone;
        P.units = P.mem = P.ncpu = 0;
        P.alive = 0; // its slot is recycled by the death sweep
        ++s.tally_.engulfs;
    }

    // Overwrites (never inserts, 6.4 #3) up to CX codons of the victim's first
    // block, starting AX codons in, from this CPU's read head.
    u32 inject_into(u32 victim) {
        u32 k = s.bodies_[victim].blocks;
        const Block& bl = s.blocks_[k];
        u32 count = c.cx < bl.len ? c.cx : bl.len;
        u32 dest = bl.start + c.ax % bl.len;
        u32 written = 0;
        for (u32 i = 0; i < count; ++i) {
            u32 from = c.rh;
            if (!readable(from))
                break;
            if (s.owner_[dest] == k && page_accepts(dest)) {
                s.mem_[dest] = copied(s.mem_[from]);
                note_write(dest);
                ++written;
            }
            c.rh = next_addr(from);
            dest = dest + 1 == bl.start + bl.len ? bl.start : dest + 1;
        }
        ++s.tally_.injects;
        return written;
    }

    // Returns the instructions charged; *ok says whether it succeeded.
    u32 contact(u32 ip, bool engulf, u32* next, bool* ok) {
        u8 bits[64];
        u32 n = read_template(next_ip(ip), bits);
        *next = skip(ip, n);
        if (!engulf && static_cast<i32>(c.cx) <= 0) {
            *ok = false; // nothing to inject: zero means nothing
            return 1;
        }
        if (engulf)
            ++s.tally_.engulf_tries;
        else
            ++s.tally_.inject_tries;
        u32 cost = 1 + p.contact_base;
        u32 target = contest(ip, bits, n);
        if (target == kNone) {
            *ok = false;
            return cost; // a failed contest still costs the attempt (6.4 #7)
        }
        u32 moved = engulf ? s.bodies_[target].mem : 0;
        if (engulf)
            engulf_body(target);
        else
            moved = inject_into(target);
        cost += static_cast<u32>(p.contact_per_codon * moved);
        *ok = true;
        return cost;
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
        s.flaw_left_ = -log(1.0 - s.rng_.unit()) / p.flaw_rate;
    }

    u32 step() {
        const u32 ip = c.ip;
        ++c.executed;
        ++s.tally_.instructions;
        if (p.heat_on)
            ++s.page_fetch_[ip >> kPageBits];
        if (!p.open_membrane) {
            if (!own(ip)) { // not my code: private code can't be executed
                fault();
                c.ip = next_ip(ip);
                return 1;
            }
            c.block = s.owner_[ip];
        }

        u32 next = next_ip(ip);
        u32 cost = 1;
        bool ok = true;
        const Op op = p.table.op[s.mem_[ip]];
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
            // BX is the destination: 0 = here. Dispersal (k = neighbour edge k)
            // arrives in Phase 3; until then any other value fails, as a node
            // without that edge will.
            i32 share = static_cast<i32>(c.cx);
            share = share < 0 ? 0 : (share > 64 ? 64 : share);
            ok = c.bx == 0 && divide(static_cast<u32>(share));
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
            break;
        }
        case Op::ret: {
            u32 v;
            ok = pop(&v);
            if (ok)
                next = v % p.size;
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
        case Op::seth:
            if (c.ax >= p.size || c.bx > 1)
                ok = false;
            else if (c.bx == 0)
                c.rh = c.ax;
            else
                c.wh = c.ax;
            break;
        case Op::geth:
            if (c.bx > 1)
                ok = false;
            else
                c.ax = c.bx == 0 ? c.rh : c.wh;
            break;
        case Op::read:
            ok = readable(c.rh);
            if (ok) {
                c.cx = s.mem_[c.rh];
                c.rh = next_addr(c.rh);
            }
            break;
        case Op::write:
            ok = writable(c.wh) && page_accepts(c.wh);
            if (ok) {
                s.mem_[c.wh] = static_cast<u8>(c.cx & 63);
                note_write(c.wh);
                c.wh = next_addr(c.wh);
            }
            break;
        case Op::engulf:
            cost = contact(ip, true, &next, &ok);
            break;
        case Op::inject:
            cost = contact(ip, false, &next, &ok);
            break;
        case Op::emit:   // signals: off until Phase 4
        case Op::listen: // signals: off until Phase 4
        case Op::disk:   // disk: off until Phase 4
        case Op::gpu:    // GPU: off until Phase 2
            ok = false;
            break;
        case Op::yield:
            c.yielded = 1;
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
            break;
        }
        case Op::t_ret: {
            u32 v;
            ok = pop(&v);
            if (ok)
                next = v % p.size;
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
                s.mem_[to] = copied(s.mem_[from]);
                note_write(to);
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
            u32 at = amount > 0 ? alloc_memory(static_cast<u32>(amount)) : kNone;
            ok = at != kNone;
            if (ok)
                c.ax = at;
            break;
        }
        case Op::t_divide: // no amount operand: the share is a config value
            ok = divide(p.tierra_share);
            break;
        case Op::count:
            ok = false;
            break;
        }

        s.flaw_left_ -= errm;
        if (s.flaw_left_ <= 0)
            flaw();
        if (ok) {
            c.err = 0;
            c.errs = 0;
        } else {
            fault();
        }
        c.ip = next;
        return cost;
    }
};

i64 Soup::run_cpu(u32 ci, i64 quota) {
    Exec e{*this, *p_, cpus_[ci], bodies_[cpus_[ci].body].err};
    i64 used = 0;
    while (used < quota && !e.c.halted && !e.c.yielded)
        used += e.step();
    // ENGULF may have appended the prey's CPUs after this one: keep the link.
    u32 next = cpus_[ci].next;
    cpus_[ci] = e.c;
    cpus_[ci].next = next;
    return used;
}

} // namespace nova
