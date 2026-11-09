// docs/bites-plan.md (revision 3, approved by Damian 2026-11-08): bleeding
// from open ends (B6), repairs (B5), INJECT only through an open end and
// just inside it (I1, I2), parasite threads with their host's rights (I3,
// I4), and a busy world with all of it on, the same at 1, 4 and 20 threads
// and through a snapshot. (Bites without immunity, the third and NOPs as
// tissue are in suite ecology; INJECT's placement and rules in suite rules.)
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/snapshot.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

struct PFix {
    SoupParams p;
    Soup s;
    bool init(const char* extra, u32 size = 1024) {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("cost.table = classic\n");
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), size))
            return false;
        s.init(&p, 37, 0);
        s.begin_tick(0);
        return true;
    }
    u8 codon(Op op) const { return static_cast<u8>(codon_of(p.table, op)); }
};

// B6: an open end loses its outermost codon once in life.bleed_every ticks
// (on the ticks where tick + 1 + serial is a multiple of it) until a NOP is
// outermost -- here internal ones, the new skin at each end. The codons go to
// waste, stamped as the body's remnant (food). A body sealed by NOPs keeps
// its code.
void test_bleeding() {
    PFix f;
    CHECK(f.init("life.bleed_every = 10\nsalvage.ratio = 4\n"));
    const u32 b = plant(f.s, 100, "INC INC NOP0 INC INC INC INC NOP1 INC INC", 100); // [100, 110)
    const u32 sealed = plant(f.s, 300, "NOP0 INC INC INC NOP1", 100);
    CHECK(b != kNone && sealed != kNone && f.s.open_end(b, false) && f.s.open_end(b, true));
    CHECK(!f.s.open_end(sealed, false) && !f.s.open_end(sealed, true) && f.s.open_ends() == 2);
    const u32 waste0 = f.s.waste();
    u64 bled = 0, first = 0;
    for (u64 t = 0; t < 40; ++t) {
        f.s.begin_tick(t);
        f.s.bleed();
        bled += f.s.tally().bled;
        if (f.s.tally().bled && !first)
            first = t;
    }
    // Its turns: ticks 9 and 19 (serial 0), two codons each; then it is sealed.
    CHECK(first == 9 && bled == 4 && f.s.body(b).mem == 6 && f.s.waste() == waste0 + 4);
    const Block& bl = f.s.block(f.s.body(b).blocks);
    CHECK(bl.start == 102 && bl.len == 6 && !f.s.open_end(b, false) && !f.s.open_end(b, true));
    // (stamped with its genome length then: 10 for the head, 9 for the tail just after)
    CHECK(f.s.owner_body(100) == kNone && f.s.owner_body(109) == kNone && f.s.stamp(100) == 10 && f.s.stamp(109) == 9);
    CHECK(f.s.body(sealed).mem == 5 && f.s.open_ends() == 0);
    CHECK(soup_ok(f.s));
    // life.bleed_every = 0: no bleeding.
    PFix g;
    CHECK(g.init("life.bleed_every = 0\n"));
    const u32 b2 = plant(g.s, 100, "INC INC INC INC", 100);
    for (u64 t = 0; t < 40; ++t) {
        g.s.tick(t);
        CHECK(g.s.tally().bled == 0);
    }
    CHECK(g.s.body(b2).mem == 4);
}

// B5: a program that writes a NOP over the outermost codon of an open end
// closes it: a repair, and the end stops bleeding.
void test_repair() {
    PFix f;
    CHECK(f.init("life.bleed_every = 0\n"));
    const u32 b = plant(f.s, 100, "WRITE INC INC INC", 100);
    CHECK(b != kNone && f.s.open_end(b, true));
    Cpu& c = f.s.cpu_mut(f.s.body(b).cpus);
    c.wh = 103;
    c.cx = f.codon(Op::nop0);
    f.s.run_cpu(f.s.body(b).cpus, 1);
    CHECK(c.err == 0 && f.s.mem()[103] == f.codon(Op::nop0) && f.s.tally().repairs == 1);
    CHECK(!f.s.open_end(b, true) && f.s.open_end(b, false)); // its head (WRITE) is still open
    // Writing a NOP inside, or over a NOP, isn't a repair.
    c.ip = 100;
    c.wh = 102;
    f.s.run_cpu(f.s.body(b).cpus, 1);
    c.ip = 100;
    c.wh = 103;
    f.s.run_cpu(f.s.body(b).cpus, 1);
    CHECK(f.s.tally().repairs == 1);
    CHECK(soup_ok(f.s));
}

// I3, I4: an INJECT through a wound starts a parasite thread at its payload,
// a thread of the host's that runs on the host's cores; a DIVIDE it runs
// divides off a daughter with the host's memory and cores (parasite_divides).
void test_parasite_thread() {
    PFix f;
    CHECK(f.init("life.bleed_every = 0\nworld.millicores = 2000\n"));
    // The payload: CX = 7, ALLOC 7 codons, DIVIDE (share 7/64).
    const u32 inj = plant(f.s, 100, "INJECT ZERO INC INC INC INC INC INC INC ALLOC DIVIDE", 500);
    TextBuf host_code;
    host_code.append("NOP1");
    for (int i = 0; i < 19; ++i)
        host_code.append(" INC");
    const u32 host = plant(f.s, 200, host_code.c_str(), 640); // [200, 220), its tail open
    CHECK(inj != kNone && host != kNone);
    Cpu& c = f.s.cpu_mut(f.s.body(inj).cpus);
    c.cx = 10;
    c.rh = 101; // ZERO ... DIVIDE
    f.s.run_cpu(f.s.body(inj).cpus, 1);
    CHECK(f.s.tally().injects == 1 && f.s.tally().parasite_threads == 1 && f.s.body(host).ncpu == 2);
    CHECK(f.s.infected() == 1);
    u32 par = kNone;
    for (u32 x = f.s.body(host).cpus; x != kNone; x = f.s.cpu(x).next)
        if (f.s.cpu(x).parasite)
            par = x;
    CHECK(par != kNone && f.s.cpu(par).ip == 210 && f.s.cpu(par).body == host);
    // The host's own thread loops on INC; the parasite's runs its payload.
    const usize bodies0 = f.s.body_count();
    u64 divides = 0;
    for (u32 i = 0; i < 5 && divides == 0; ++i) {
        f.s.begin_tick(1 + i);
        f.s.run_cpu(par, 100);
        divides += f.s.tally().parasite_divides;
    }
    // (Its payload sits at the host's tail: the thread wraps round through the
    // host's code and back into it, dividing once a pass.)
    CHECK(divides >= 1 && f.s.body_count() == bodies0 + divides && f.s.tally().births == divides);
    CHECK(f.s.body(host).units < 640); // the daughter's share came from the host
    CHECK(soup_ok(f.s));
}

// A busy procedural world with bites on and fast bleeding: wounds, bleeding,
// repairs-or-not, INJECT through wounds and parasite threads all happen, and
// the world is the same at 1, 4 and 20 threads and through a snapshot.
void test_world() {
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append("engulf.bite = true\nlife.bleed_every = 20\n");
    const u64 ticks = 300;
    u64 hashes[3] = {};
    const u32 threads[3] = {1, 4, 20};
    SoupTally sum = {};
    ByteWriter snap;
    for (u32 v = 0; v < 3; ++v) {
        Run run;
        CHECK(make_run(run, cfg.c_str(), threads[v]));
        for (u64 t = 1; t <= ticks; ++t) {
            run.run_until(t);
            if (v != 0)
                continue;
            const u64* s = reinterpret_cast<const u64*>(&run.world().last_soup_tick());
            u64* d = reinterpret_cast<u64*>(&sum);
            for (usize i = 0; i < sizeof(SoupTally) / sizeof(u64); ++i)
                d[i] += s[i];
            if (t == ticks / 2)
                run.snapshot(snap);
        }
        hashes[v] = run.world().hash();
        if (v == 0)
            for (u32 n = 0; n < run.world().node_count(); ++n)
                CHECK(soup_ok(run.world().node_soup(n)));
    }
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK(sum.bites_body > 0 && sum.wounds > 0 && sum.bled > 0 && sum.injects > 0 && sum.parasite_threads > 0);
    CHECK(snap.size() > 0);
    if (snap.size() > 0) {
        Run b;
        TextBuf err;
        CHECK(b.init_from_snapshot(snap.data(), snap.size(), 4, &err));
        b.run_until(ticks);
        CHECK_EQ_U64(b.world().hash(), hashes[0]);
    }
    printf("  parasites world: %llu ticks, %llu body bites (%llu wounds), %llu codons bled, %llu repairs, %llu injects, "
           "%llu parasite threads, %llu parasite divides; the same at 1, 4 and 20 threads and through a snapshot\n",
           static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(sum.bites_body),
           static_cast<unsigned long long>(sum.wounds), static_cast<unsigned long long>(sum.bled),
           static_cast<unsigned long long>(sum.repairs), static_cast<unsigned long long>(sum.injects),
           static_cast<unsigned long long>(sum.parasite_threads), static_cast<unsigned long long>(sum.parasite_divides));
}

} // namespace

void suite_parasites() {
    test_bleeding();
    test_repair();
    test_parasite_thread();
    test_world();
}
