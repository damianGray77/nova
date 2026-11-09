// docs/bites-plan.md (revision 3, approved by Damian 2026-11-08): bleeding
// from open ends (B6), repairs (B5); docs/inject-plan.md (approved
// 2026-11-09): INJECT moving the program into its host as a parasite (M1-M5,
// M8), swallowed prey digested and escaping (S1-S5); and a busy world with
// all of it on, the same at 1, 4 and 20 threads and through a snapshot.
// (Bites without immunity, the third and NOPs as tissue are in suite
// ecology; INJECT's placement, size and room rules in suite rules.)
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

// docs/inject-plan.md M1-M5: an INJECT through a wound moves the whole
// injector into its host. It stays its own body (its serial, cores and
// threads), its genome now just inside the wound, its old memory zeros. It
// runs on its own cores only (M3). Its ALLOC carves the block out of its
// host's code, from the end away from its own, and its DIVIDE frees the
// daughter with her share of the host's cores (M4). When the host dies, what
// is left of its code becomes its own block again (M5).
void test_parasite() {
    PFix f;
    CHECK(f.init("life.bleed_every = 0\nworld.millicores = 2000\n"));
    // Its genome (11 codons): INJECT, then CX = 7, ALLOC 7 codons, DIVIDE (share 7/64).
    const u32 inj = plant(f.s, 100, "INJECT ZERO INC INC INC INC INC INC INC ALLOC DIVIDE", 500);
    TextBuf host_code;
    host_code.append("NOP1");
    for (int i = 0; i < 29; ++i)
        host_code.append(" INC");
    const u32 host = plant(f.s, 200, host_code.c_str(), 640); // [200, 230), its tail open
    CHECK(inj != kNone && host != kNone);
    u8 genome[11];
    for (u32 i = 0; i < 11; ++i)
        genome[i] = f.s.mem()[100 + i];
    const u32 pc = f.s.body(inj).cpus;
    const u64 serial = f.s.body(inj).serial;
    f.s.run_cpu(pc, 1);
    CHECK(f.s.tally().injects == 1 && f.s.tally().parasite_threads == 1 && f.s.body(host).ncpu == 1);
    CHECK(f.s.is_parasite(inj) && f.s.host_of(inj) == host && f.s.body(inj).serial == serial);
    CHECK(f.s.body(inj).blocks == kNone && f.s.body(inj).mem == 0 && f.s.body(inj).units == 500);
    CHECK(f.s.infected() == 1 && f.s.guests_now(kGuestParasite) == 1 && f.s.genome_len(inj) == 11);
    bool moved = true, zeroed = true;
    for (u32 i = 0; i < 11; ++i) {
        moved = moved && f.s.mem()[219 + i] == genome[i];
        zeroed = zeroed && f.s.mem()[100 + i] == 0 && f.s.owner_body(100 + i) == kNone;
    }
    CHECK(moved && zeroed && f.s.owner_body(219) == host); // (its codons stay its host's)
    CHECK(f.s.cpu(pc).ip == 220 && f.s.cpu(pc).parasite && f.s.cpu(pc).body == inj);
    CHECK(soup_ok(f.s));
    // It runs on: ZERO, INC x 7, ALLOC (carved from the host's low end,
    // [200, 207)), DIVIDE.
    const usize bodies0 = f.s.body_count();
    u64 divides = 0;
    for (u32 i = 0; i < 20 && divides == 0; ++i) {
        f.s.run_cpu(pc, 1);
        divides += f.s.tally().parasite_divides;
    }
    CHECK(divides == 1 && f.s.body_count() == bodies0 + 1 && f.s.tally().births == 1);
    const u32 d = f.s.owner_body(200);
    CHECK(d != kNone && d != host && d != inj && f.s.owner_body(206) == d && f.s.owner_body(207) == host);
    CHECK(f.s.host_of(d) == kNone && f.s.block(f.s.body(d).blocks).len == 7);
    CHECK(f.s.body(d).units == 70 && f.s.body(host).units == 570 && f.s.body(inj).units == 500); // 7/64 of the host's
    CHECK(f.s.block(f.s.body(host).blocks).start == 207 && f.s.body(host).mem == 23);
    CHECK(soup_ok(f.s));
    // The host dies: its parasite lives on, its code its own block again.
    f.s.force_kill(host);
    f.s.death_sweep(); // (its slot leaves the schedule)
    CHECK(f.s.body(inj).alive && !f.s.is_parasite(inj) && f.s.tally().freed == 1 && f.s.infected() == 0);
    CHECK(f.s.body(inj).blocks != kNone && f.s.block(f.s.body(inj).blocks).start == 219);
    CHECK(f.s.block(f.s.body(inj).blocks).len == 11 && f.s.body(inj).mem == 11);
    CHECK(!f.s.cpu(pc).parasite && f.s.cpu(pc).block == f.s.body(inj).blocks);
    CHECK(soup_ok(f.s));
}

// M8: a parasite can't INJECT again, bite or disperse; nothing can bite or
// INJECT into it (it has no surface of its own). A program with guests in
// it can't INJECT either (its code is theirs too).
void test_parasite_limits() {
    PFix f;
    CHECK(f.init("life.bleed_every = 0\nworld.millicores = 2000\nengulf.bite = true\n"));
    const u32 inj = plant(f.s, 100, "INJECT ENGULF INJECT", 300);
    TextBuf host_code;
    host_code.append("NOP1");
    for (int i = 0; i < 9; ++i)
        host_code.append(" INC");
    const u32 host = plant(f.s, 200, host_code.c_str(), 300); // [200, 210)
    CHECK(inj != kNone && host != kNone);
    const u32 pc = f.s.body(inj).cpus;
    f.s.run_cpu(pc, 1);
    CHECK(f.s.is_parasite(inj) && f.s.cpu(pc).ip == 208);
    f.s.cpu_mut(pc).cx = 3;
    f.s.run_cpu(pc, 1); // ENGULF: fails
    CHECK(f.s.cpu(pc).err == 1 && f.s.tally().bites_body == 0 && f.s.tally().bites_waste == 0);
    f.s.run_cpu(pc, 1); // INJECT: fails
    CHECK(f.s.cpu(pc).err == 1 && f.s.tally().injects == 1);
    // The host, infected, can't move into a bigger body either.
    TextBuf big;
    big.append("NOP1");
    for (int i = 0; i < 39; ++i)
        big.append(" INC");
    CHECK(plant(f.s, 300, big.c_str(), 300) != kNone); // [300, 340), its tail open
    f.s.mem_mut()[200] = static_cast<u8>(f.codon(Op::inject));
    Cpu& hc = f.s.cpu_mut(f.s.body(host).cpus);
    hc.ip = 200;
    f.s.run_cpu(f.s.body(host).cpus, 1);
    CHECK(hc.err == 1 && f.s.tally().injects == 1 && !f.s.is_parasite(host));
    CHECK(soup_ok(f.s));
}

// S1-S5 (Damian: "the victim shouldn't die immediately"): a bite that
// swallows its prey whole makes it a swallowed guest, digested
// engulf.digest (2) codons a tick. Its INJECT aims at its eater, needs no
// wound, and makes it a parasite there: digestion stops (an escape). If its
// eater dies first it is free again.
void test_swallowed() {
    PFix f;
    CHECK(f.init("life.bleed_every = 0\nworld.millicores = 2000\nengulf.bite = true\nengulf.digest = 2\n"));
    TextBuf eater_code; // (aimed: its NOP0 template finds the prey's NOP1 surface)
    eater_code.append("ENGULF NOP0");
    for (int i = 0; i < 27; ++i)
        eater_code.append(" INC");
    eater_code.append(" NOP0");
    const u32 e = plant(f.s, 100, eater_code.c_str(), 500); // [100, 130)
    const u32 prey = plant(f.s, 200, "NOP1 INJECT ALLOC INC INC", 300);
    CHECK(e != kNone && prey != kNone);
    u8 genome[5];
    for (u32 i = 0; i < 5; ++i)
        genome[i] = f.s.mem()[200 + i];
    f.s.cpu_mut(f.s.body(e).cpus).cx = 10; // a third of 30: 10, the whole prey
    f.s.run_cpu(f.s.body(e).cpus, 1);
    CHECK(f.s.is_swallowed(prey) && f.s.host_of(prey) == e && f.s.tally().swallows == 1 && f.s.body(prey).alive);
    CHECK(f.s.guests_now(kGuestSwallowed) == 1 && soup_ok(f.s));
    // While swallowed it can't ALLOC (S3).
    Cpu& pc = f.s.cpu_mut(f.s.body(prey).cpus);
    pc.ip = 202;
    pc.cx = 4;
    f.s.run_cpu(f.s.body(prey).cpus, 1);
    CHECK(pc.err == 1 && f.s.body(prey).mem == 5);
    f.s.digest();
    CHECK(f.s.body(prey).mem == 3 && f.s.body(e).mem == 32 && f.s.owner_body(204) == e && soup_ok(f.s));
    // Its INJECT: into its eater, at the end nearer it (the high end), no wound needed.
    pc.ip = 201;
    f.s.run_cpu(f.s.body(prey).cpus, 1);
    CHECK(pc.err == 0 && f.s.is_parasite(prey) && f.s.host_of(prey) == e && f.s.tally().escapes == 1);
    CHECK(f.s.mem()[127] == genome[0] && f.s.mem()[128] == genome[1] && f.s.mem()[129] == genome[2]);
    f.s.digest();
    CHECK(f.s.tally().digested == 2 && soup_ok(f.s)); // (no more: it got out)
    // Another, whose eater dies first: free again, its code its own.
    PFix g;
    CHECK(g.init("life.bleed_every = 0\nworld.millicores = 2000\nengulf.bite = true\nengulf.digest = 2\n"));
    const u32 e2 = plant(g.s, 100, eater_code.c_str(), 500);
    const u32 p2 = plant(g.s, 200, "NOP1 INC INC INC INC", 300);
    g.s.cpu_mut(g.s.body(e2).cpus).cx = 10;
    g.s.run_cpu(g.s.body(e2).cpus, 1);
    CHECK(g.s.is_swallowed(p2));
    g.s.digest();
    g.s.force_kill(e2);
    g.s.death_sweep();
    CHECK(g.s.body(p2).alive && !g.s.is_swallowed(p2) && g.s.tally().freed == 1 && g.s.body(p2).mem == 3);
    CHECK(soup_ok(g.s));
    // And one that dies inside: food at once.
    PFix h;
    CHECK(h.init("life.bleed_every = 0\nworld.millicores = 2000\nengulf.bite = true\nengulf.digest = 2\n"));
    const u32 e3 = plant(h.s, 100, eater_code.c_str(), 500);
    const u32 p3 = plant(h.s, 200, "NOP1 INC INC INC INC", 300);
    h.s.cpu_mut(h.s.body(e3).cpus).cx = 10;
    h.s.run_cpu(h.s.body(e3).cpus, 1);
    h.s.cpu_mut(h.s.body(p3).cpus).halted = 1;
    h.s.death_sweep();
    CHECK(!h.s.body(p3).alive && h.s.body(e3).mem == 35 && h.s.body(e3).units == 800 && h.s.tally().engulfs == 1);
    CHECK(soup_ok(h.s));
}

// docs/lanes-plan.md H1 (Damian: "fairer for both swallower and victim"): a
// swallowed body's cores go to its eater with its codons, the same share each
// tick (rounded to the nearest), so neither holds what the other has: a
// 10-codon prey with 300 millicores and 10 GPU units, digested 2 codons a
// tick, keeps 240 and 8, then 180 and 6, 120 and 4, 60 and 2; then it is gone
// and its eater has them all.
void test_handover() {
    PFix f;
    CHECK(f.init("life.bleed_every = 0\nworld.millicores = 2000\nworld.gpus = 64\nengulf.bite = true\n"
                 "engulf.digest = 2\n"));
    TextBuf eater_code;
    eater_code.append("ENGULF NOP0");
    for (int i = 0; i < 27; ++i)
        eater_code.append(" INC");
    eater_code.append(" NOP0");
    const u32 e = plant(f.s, 100, eater_code.c_str(), 500); // [100, 130)
    const u32 prey = plant(f.s, 200, "NOP1 INC INC INC INC INC INC INC INC INC", 300); // [200, 210)
    CHECK(e != kNone && prey != kNone && f.s.claim_gpus(prey, 10) == 10);
    f.s.cpu_mut(f.s.body(e).cpus).cx = 10; // a third of 30: the whole prey
    f.s.run_cpu(f.s.body(e).cpus, 1);
    CHECK(f.s.is_swallowed(prey) && f.s.body(e).units == 500 && f.s.body(prey).units == 300);
    const u32 units[4] = {240, 180, 120, 60}, gpus[4] = {8, 6, 4, 2};
    for (u32 i = 0; i < 4; ++i) {
        f.s.digest();
        CHECK(f.s.body(prey).alive && f.s.body(prey).mem == 8 - 2 * i);
        CHECK(f.s.body(prey).units == units[i] && f.s.body(e).units == 800 - units[i]);
        CHECK(f.s.gpu_count(prey) == gpus[i] && f.s.gpu_count(e) == 10 - gpus[i]);
        CHECK(soup_ok(f.s));
    }
    f.s.digest(); // its last 2 codons: food, the rest of it its eater's
    CHECK(!f.s.body(prey).alive && f.s.body(e).units == 800 && f.s.gpu_count(e) == 10 && f.s.tally().digested == 10);
    f.s.death_sweep(); // (its slot leaves the schedule, as at a tick's end)
    CHECK(soup_ok(f.s));
}

// A busy procedural world with bites on, digestion and fast bleeding:
// wounds, bleeding, repairs-or-not, programs moving into hosts, swallows and
// digestion all happen, and the world is the same at 1, 4 and 20 threads and
// through a snapshot (taken with guests in it).
void test_world() {
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append("engulf.bite = true\nengulf.digest = 2\nlife.bleed_every = 20\n");
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
    CHECK(sum.swallows > 0 && sum.digested > 0);
    CHECK(snap.size() > 0);
    if (snap.size() > 0) {
        Run b;
        TextBuf err;
        CHECK(b.init_from_snapshot(snap.data(), snap.size(), 4, &err));
        b.run_until(ticks);
        CHECK_EQ_U64(b.world().hash(), hashes[0]);
    }
    printf("  parasites world: %llu ticks, %llu body bites (%llu wounds), %llu codons bled, %llu repairs, %llu moved in, "
           "%llu parasite divides, %llu swallowed, %llu codons digested, %llu escapes, %llu freed; the same at 1, 4 and "
           "20 threads and through a snapshot\n",
           static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(sum.bites_body),
           static_cast<unsigned long long>(sum.wounds), static_cast<unsigned long long>(sum.bled),
           static_cast<unsigned long long>(sum.repairs), static_cast<unsigned long long>(sum.injects),
           static_cast<unsigned long long>(sum.parasite_divides), static_cast<unsigned long long>(sum.swallows),
           static_cast<unsigned long long>(sum.digested), static_cast<unsigned long long>(sum.escapes),
           static_cast<unsigned long long>(sum.freed));
}

} // namespace

void suite_parasites() {
    test_bleeding();
    test_repair();
    test_parasite();
    test_parasite_limits();
    test_swallowed();
    test_handover();
    test_world();
}
