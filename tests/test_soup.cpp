// The soup's world mechanics (design.md 6.1, 6.3, 6.4): the CPU ledger and
// scheduler, births and deaths, sparks, conservation, and Gate 2 -- the 6.2
// sketch replicator under Nova's rules.
#include "check.h"
#include "helpers.h"
#include "nova/snapshot.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

const char* const kSketch = "SENSE ALLOC NOP1 COPY DJNZ NOP0 INC DIVIDE";

struct Fix {
    SoupParams p;
    Soup s;
    bool init(const char* extra, u32 size = 4096, u64 seed = 3) {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), size))
            return false;
        s.init(&p, seed, 0);
        return true;
    }
};

bool is_sketch(const Soup& s, const Block& bl, const u8* code) {
    if (bl.len != 8)
        return false;
    for (u32 i = 0; i < 8; ++i)
        if (s.mem()[bl.start + i] != code[i])
            return false;
    return true;
}

// Budget = owned millicores x instr_per_core / 1000, fractions carried.
void scheduler() {
    Fix f;
    CHECK(f.init("world.millicores = 4000\ncpu.instr_per_core = 250\n"));
    u32 big = plant(f.s, 100, "INC", 1000);
    u32 small = plant(f.s, 200, "INC", 1);
    CHECK(big != kNone && small != kNone);
    for (u64 t = 0; t < 10; ++t)
        f.s.tick(t);
    CHECK(f.s.cpu(f.s.body(big).cpus).cx == 2500); // 250 a tick
    CHECK(f.s.cpu(f.s.body(small).cpus).cx == 2);  // 0.25 a tick, carried: ticks 3 and 7
    CHECK(f.s.tally().instructions == 250);         // the last tick
    CHECK(soup_ok(f.s));
}

// A body with no running CPU dies: holdings back to the pool, memory to waste
// (contents kept, for scavengers).
void death() {
    Fix f;
    CHECK(f.init("world.millicores = 1000\n"));
    u32 jammed = plant(f.s, 100, "JMP", 50);   // fails every time: halts after 8
    u32 broke = plant(f.s, 200, "INC INC", 0); // no millicore to run on
    u32 fine = plant(f.s, 300, "INC", 50);
    CHECK(jammed != kNone && broke != kNone && fine != kNone);
    u8 before = f.s.mem()[100];
    f.s.tick(0);
    CHECK(f.s.body_count() == 1 && f.s.body_at(0) == fine);
    CHECK(f.s.tally().deaths == 2 && f.s.tally().halts == 1);
    CHECK(f.s.owner_body(100) == kNone && f.s.mem()[100] == before);
    CHECK(f.s.free_units() == 950 && f.s.waste() == 4095);
    CHECK(soup_ok(f.s));
}

// Sparks turn random stretches of waste into bodies with a grant from the pool,
// and stop when the pool runs dry.
void sparks() {
    Fix f;
    CHECK(f.init("world.millicores = 1000\nspark.initial = 50\nspark.grant = 10\n"
                 "spark.min_len = 8\nspark.max_len = 16\n"));
    CHECK(f.s.body_count() == 50 && f.s.free_units() == 500);
    bool lengths = true;
    for (usize k = 0; k < f.s.body_count(); ++k) {
        const Body& b = f.s.body(f.s.body_at(k));
        lengths = lengths && b.mem >= 8 && b.mem <= 16 && b.units == 10 && b.ncpu == 1;
    }
    CHECK(lengths);
    CHECK(soup_ok(f.s));

    Fix g; // a rate of 2.5 per tick, a dry pool after 100 bodies
    CHECK(g.init("world.millicores = 1000\nspark.rate = 2.5\nspark.grant = 10\n"));
    u64 total = 0;
    for (u64 t = 0; t < 30; ++t) {
        g.s.tick(t);
        total += g.s.tally().sparks;
    }
    CHECK(total >= 50 && total <= 100); // ~75 expected
    Fix h;
    CHECK(h.init("world.millicores = 50\nspark.initial = 20\nspark.grant = 10\n"));
    CHECK(h.s.body_count() == 5 && h.s.free_units() == 0);
}

// Gate 2, interpreter level: the sketch copies itself exactly, its daughter
// does too, and so on down to the generation whose 1/64 share rounds to zero,
// which dies in the sweep. Its second replication needs heads that wrap at the
// end of their block, like the IP (design.md 6.5).
void sketch_generations() {
    Fix f;
    CHECK(f.init("world.millicores = 5000\n"));
    u32 parent = plant(f.s, 2000, kSketch, 4096);
    CHECK(parent != kNone);
    u8 code[8];
    for (u32 i = 0; i < 8; ++i)
        code[i] = f.s.mem()[2000 + i];
    f.s.begin_tick(0);

    auto replicate = [&](u32 body, u32 want_units) {
        u64 births = f.s.tally().births;
        steps(f.s, body, 21); // SENSE ALLOC NOP1 (COPY DJNZ) x8 INC DIVIDE
        CHECK(f.s.tally().births == births + 1);
        u32 d = f.s.body_at(f.s.body_count() - 1); // the newest body
        const Body& db = f.s.body(d);
        CHECK(is_sketch(f.s, f.s.block(db.blocks), code));
        CHECK(db.units == want_units);
        return d;
    };
    u32 d1 = replicate(parent, 64);  // 4096 / 64
    u32 d1b = replicate(parent, 63); // 4032 / 64: the second copy works too
    u32 d2 = replicate(d1, 1);       // 64 / 64
    u32 d3 = replicate(d2, 0);       // 1 / 64 rounds to zero
    (void)d1b;
    CHECK(f.s.cpu(f.s.body(d1).cpus).ip == f.s.block(f.s.body(d1).blocks).start);
    f.s.death_sweep();
    CHECK(f.s.body_count() == 4); // parent, d1, d1b, d2; d3 swept
    bool d3_gone = true;
    for (usize k = 0; k < f.s.body_count(); ++k)
        d3_gone = d3_gone && f.s.body_at(k) != d3;
    CHECK(d3_gone);
    CHECK(soup_ok(f.s));
}

// Gate 2, world level: seeded alone on a node and run by the scheduler, every
// body is an exact copy and the zero-share generation dies.
void sketch_world() {
    Run run;
    CHECK(make_run(run, "seed = 7\nworld.model = soup\nworld.nodes = 1\nworld.soup_size = 4096\n"
                        "world.millicores = 8192\ncpu.instr_per_core = 100\n"
                        "heat.enabled = false\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\n"
                        "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 INC DIVIDE\n"
                        "seed.at = 2000\nseed.millicores = 8192\n",
                   1));
    u64 births = 0, deaths = 0;
    for (u32 t = 0; t < 40; ++t) {
        run.step();
        births += run.world().last_soup_tick().births;
        deaths += run.world().last_soup_tick().deaths;
    }
    const Soup& s = run.world().node_soup(0);
    u8 code[8];
    for (u32 i = 0; i < 8; ++i)
        code[i] = s.mem()[2000 + i];
    bool exact = true, all_have_cpu = true;
    for (usize k = 0; k < s.body_count(); ++k) {
        const Body& b = s.body(s.body_at(k));
        exact = exact && is_sketch(s, s.block(b.blocks), code);
        all_have_cpu = all_have_cpu && b.units > 0;
    }
    CHECK(exact);
    CHECK(all_have_cpu);
    CHECK(births > 100 && deaths > 0);
    CHECK(soup_ok(s));
    printf("  soup: sketch alone for 40 ticks: %llu births, %llu zero-share deaths, %zu exact "
           "copies alive\n",
           static_cast<unsigned long long>(births), static_cast<unsigned long long>(deaths),
           s.body_count());
}

// With Ray's baseline hazards and heat on, the sketch still replicates and most
// copies stay exact.
void sketch_with_hazards() {
    Run run;
    CHECK(make_run(run, "seed = 11\nworld.model = soup\nworld.nodes = 1\nworld.soup_size = 8192\n"
                        "world.millicores = 8192\ncpu.instr_per_core = 100\n"
                        "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 INC DIVIDE\n"
                        "seed.at = 4000\nseed.millicores = 8192\n",
                   1));
    u64 births = 0, copy_errors = 0;
    for (u32 t = 0; t < 40; ++t) {
        run.step();
        births += run.world().last_soup_tick().births;
        copy_errors += run.world().last_soup_tick().copy_errors;
    }
    const Soup& s = run.world().node_soup(0);
    u8 code[8];
    for (u32 i = 0; i < 8; ++i)
        code[i] = s.mem()[4000 + i];
    usize exact = 0;
    for (usize k = 0; k < s.body_count(); ++k)
        exact += is_sketch(s, s.block(s.body(s.body_at(k)).blocks), code);
    CHECK(births > 100);
    CHECK(exact * 10 >= s.body_count() * 8); // at least 80% exact
    CHECK(soup_ok(s));
}

// Every invariant holds after every tick of a busy random soup: sparks, random
// code running every instruction, heat releases, hazards, contact.
void conservation() {
    Run run;
    CHECK(make_run(run, kSoupWorld, 4));
    bool ok = true;
    for (u32 t = 0; t < 60 && ok; ++t) {
        run.step();
        for (u32 n = 0; n < run.world().node_count() && ok; ++n)
            ok = soup_ok(run.world().node_soup(n));
    }
    CHECK(ok);
}

// Snapshot, resume at another thread count, with a knob change before it.
void snapshot_resume() {
    TextBuf err;
    Run straight;
    CHECK(make_run(straight, kSoupWorld, 1));
    CHECK(straight.schedule_knob(20, "spark.rate", "3", &err));
    straight.run_until(60);

    Run first;
    CHECK(make_run(first, kSoupWorld, 20));
    CHECK(first.schedule_knob(20, "spark.rate", "3", &err));
    first.run_until(30);
    ByteWriter bytes;
    first.snapshot(bytes);

    Run resumed;
    CHECK(resumed.init_from_snapshot(bytes.data(), bytes.size(), 4, &err));
    resumed.run_until(60);
    CHECK_EQ_U64(resumed.world().hash(), straight.world().hash());

    Run no_knob; // control: the knob really changed the run
    CHECK(make_run(no_knob, kSoupWorld, 4));
    no_knob.run_until(60);
    CHECK(no_knob.world().hash() != straight.world().hash());

    // Structural soup keys cannot change mid-run; unknown keys are refused.
    CHECK(!straight.schedule_knob(70, "hazard.copy", "0.1", &err));
    CHECK(!straight.schedule_knob(70, "demo.send_rate", "0.5", &err));

    // A corrupted soup section is rejected, not loaded.
    ByteWriter bad;
    first.snapshot(bad);
    Array<u8> copy;
    copy.assign(bad.data(), bad.size());
    copy[copy.size() / 2] ^= 0x40;
    Run broken;
    CHECK(!broken.init_from_snapshot(copy.data(), copy.size(), 1, &err));
}

// The word-at-a-time free-run searches agree with a naive codon-by-codon scan
// on fragmented soups (odd sizes, so the last word is partial).
void free_runs() {
    const u32 sizes[] = {1000u, 4096u, 4133u};
    for (u32 size : sizes) {
        Fix f;
        CHECK(f.init("world.millicores = 100000\n", size, 99));
        Rng r(5, size, 0);
        for (u32 i = 0; i < size / 6; ++i) {
            u32 at = static_cast<u32>(r.below(size));
            u32 len = 1 + static_cast<u32>(r.below(i % 3 == 0 ? 90 : 9));
            f.s.create_body(at, len, 0); // fails harmlessly where it overlaps
        }
        auto free_at = [&](u32 a) { return f.s.owner_block(a) == kNone; };
        bool same = true;
        for (u32 t = 0; t < 3000 && same; ++t) {
            u32 at = static_cast<u32>(r.below(size + 1));
            u32 len = 1 + static_cast<u32>(r.below(t % 2 ? 12 : 200));
            u32 want_f = kNone, want_b = kNone;
            for (u32 p = at; p + len <= size && want_f == kNone; ++p) {
                bool ok = true;
                for (u32 q = p; q < p + len && ok; ++q)
                    ok = free_at(q);
                if (ok)
                    want_f = p;
            }
            for (i64 p = static_cast<i64>(at) - len; p >= 0 && want_b == kNone; --p) {
                bool ok = true;
                for (u32 q = static_cast<u32>(p); q < p + len && ok; ++q)
                    ok = free_at(q);
                if (ok)
                    want_b = static_cast<u32>(p);
            }
            u32 got_f = at < size ? f.s.find_free_fwd(at, len) : kNone;
            if (at == size)
                want_f = kNone;
            u32 got_b = f.s.find_free_bwd(at, len);
            same = got_f == want_f && got_b == want_b;
            if (!same)
                fprintf(stderr, "  free_runs: size %u at %u len %u: fwd %u vs %u, bwd %u vs %u\n",
                        size, at, len, got_f, want_f, got_b, want_b);
        }
        CHECK(same);
    }
}

} // namespace

void suite_soup() {
    free_runs();
    scheduler();
    death();
    sparks();
    sketch_generations();
    sketch_world();
    sketch_with_hazards();
    conservation();
    snapshot_resume();
}
