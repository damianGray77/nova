// The replicator detector (design.md 6.6, Phase 1b D2): its four controls,
// hash identity with detection on and off, the genebank and lineage outputs,
// and gate events that replay exactly at 1 and 20 threads.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/lineage.h"

using namespace nova;

namespace {

// No sparks, no hazards, heat on, Phase 1b rules, and a seeded genome with a
// 10-millicore grant (a spark's), appended by each test. One node, or four.
#define NOVA_SEEDED_WORLD                                                                          \
    "seed = 9\nworld.model = soup\nworld.soup_size = 16384\n"                                      \
    "world.millicores = 4000\ncpu.instr_per_core = 25000\n"                                        \
    "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\n"                                           \
    "seed.at = 8000\n"
const char* const kSeeded = NOVA_SEEDED_WORLD "world.nodes = 1\n";
const char* const kSeeded4 = NOVA_SEEDED_WORLD "world.nodes = 4\n";

const char* const kShare = "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE\n"
                           "seed.millicores = 10\n";

struct Outcome {
    u64 links = 0, founded = 0, detections = 0;
    u32 max_depth = 0;
    u64 first_tick = 0;
    u64 hash = 0;
    bool ok = false;
};

Outcome run_for(const char* extra, u64 ticks, const char* base = kSeeded) {
    Outcome o;
    TextBuf cfg;
    cfg.append(base);
    cfg.append(extra);
    Run run;
    if (!make_run(run, cfg.c_str(), 1))
        return o;
    run.run_until(ticks);
    const Lineages* L = run.world().node_lineages(0);
    if (L) {
        o.links = L->links();
        o.founded = L->founded();
        o.detections = L->detections();
        o.max_depth = L->max_depth_ever();
    }
    o.first_tick = run.detection().tick;
    o.hash = run.world().hash();
    o.ok = true;
    return o;
}

// Control 1: the SENSE-share replicator fires. Its 1-millicore daughters run
// and copy themselves, so the lineage reaches depth 2 with several alive.
void share_fires() {
    Outcome o = run_for(kShare, 30);
    CHECK(o.ok && o.detections == 1 && o.founded >= 1 && o.max_depth >= 2);
    CHECK(o.first_tick < 10);
    printf("  detect: SENSE-share replicator at grant 10: gate met at tick %llu (%llu links)\n",
           static_cast<unsigned long long>(o.first_tick), static_cast<unsigned long long>(o.links));
}

// Control 2: the INC-share sketch does not. Its daughters get floor(10/64) = 0
// millicores and die at birth: links of depth 1 only.
void sketch_does_not_fire() {
    Outcome o = run_for("seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 INC DIVIDE\n"
                        "seed.millicores = 10\n",
                        200);
    CHECK(o.ok && o.detections == 0 && o.links > 0 && o.max_depth == 1);
}

// Control 3: ALLOC + DIVIDE without COPY buds off whatever waste was there: no
// inheritance, so no links at all (its daughters are funded and run, too).
void no_copy_no_link() {
    Outcome o = run_for("seed.genome = SENSE ALLOC SENSE DIVIDE\nseed.millicores = 1000\n", 200);
    CHECK(o.ok && o.detections == 0 && o.links == 0);
}

// Control 4: a sparks-only random soup (the bench node) buds constantly, and
// budding never makes a link. Random code does make short INJECT chains -- a
// spark with SENSE ... INJECT overwrites a neighbour with a copy of itself --
// which is why the gate's thresholds are knobs (design.md 6.6).
void random_budding_never_links() {
    const char* soup = "seed = 4\nworld.model = soup\nworld.nodes = 1\nworld.soup_size = 16384\n"
                       "world.millicores = 4000\ncpu.instr_per_core = 25000\n"
                       "spark.initial = 400\nspark.rate = 4\nspark.grant = 10\n"
                       "spark.min_len = 8\nspark.max_len = 64\n";
    for (int inject = 0; inject < 2; ++inject) {
        TextBuf cfg;
        cfg.append(soup);
        if (!inject)
            cfg.append("detect.inject = false\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 1));
        u64 births = 0;
        for (u32 t = 0; t < 300; ++t) {
            run.step();
            births += run.world().last_soup_tick().births;
        }
        const Lineages* L = run.world().node_lineages(0);
        CHECK(L != nullptr && births > 300);
        if (!L)
            return;
        CHECK(L->links() == L->inject_links()); // no DIVIDE link from budding
        if (!inject)
            CHECK(L->links() == 0 && !run.detection().found);
        else
            printf("  detect: random soup, 300 ticks: %llu births, 0 DIVIDE links, %llu INJECT links "
                   "(chains to depth %u%s)\n",
                   static_cast<unsigned long long>(births),
                   static_cast<unsigned long long>(L->inject_links()), L->max_depth_ever(),
                   run.detection().found ? ", meeting the default 2/2 gate" : "");
    }
}

// A detector on a standalone soup, for hand-built cases.
struct Watched {
    SoupParams p;
    DetectParams d;
    Lineages L;
    Soup s;
    bool init(const char* extra) {
        TextBuf cfg;
        cfg.append("heat.enabled = false\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\n"
                   "mem.upkeep = 0\nworld.millicores = 1000\n");
        cfg.append(extra);
        Config c;
        Config::ParseError pe;
        if (!c.parse(cfg.c_str(), &pe) || !soup_params_from_config(c, 1024, &p) ||
            !detect_params_from_config(c, &d))
            return false;
        s.init(&p, 5, 0);
        L.init(&d, 0);
        s.set_observer(&L);
        s.begin_tick(0);
        return true;
    }
    u32 plant(u32 at, const char* names, u32 units) {
        Array<u8> code;
        TextBuf err;
        if (!genome_from_names(p.table, names, &code, &err))
            return kNone;
        for (usize i = 0; i < code.size(); ++i)
            s.mem_mut()[at + i] = code[i];
        return s.create_body(at, static_cast<u32>(code.size()), units);
    }
    void run(u32 body, u32 n) {
        for (u32 i = 0; i < n; ++i)
            s.run_cpu(s.body(body).cpus, 1);
    }
};

// detect.written: budding onto a relative's intact remains is not copying.
// The parent never writes; its ALLOC lands right after it, where a copy of
// its code lies as waste.
void remains_do_not_count() {
    for (int written = 0; written < 2; ++written) {
        Watched w;
        CHECK(w.init(written ? "" : "detect.written = false\n"));
        u32 b = w.plant(100, "SENSE ALLOC SENSE DIVIDE", 100);
        CHECK(b != kNone);
        for (u32 i = 0; i < 4; ++i) // the remains: a copy at 104..107, unowned
            w.s.mem_mut()[104 + i] = w.s.mem()[100 + i];
        w.run(b, 4); // SENSE ALLOC SENSE DIVIDE
        CHECK(w.s.tally().births == 1);
        CHECK(w.L.links() == (written ? 0u : 1u));
    }
    // A parent that does copy itself still links with detect.written on.
    Watched w;
    CHECK(w.init(""));
    u32 b = w.plant(100, "SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE", 500);
    w.run(b, 21);
    CHECK(w.s.tally().births == 1 && w.L.links() == 1);
}

// detect.inject: an INJECT that leaves its victim a copy is a link; the victim
// joins the injector's lineage; re-infecting her adds no member. The injector
// carries a surface ("0") and a key ("1"), so its copies stay open to it.
void inject_links() {
    for (int on = 0; on < 2; ++on) {
        Watched w;
        CHECK(w.init(on ? "" : "detect.inject = false\n"));
        u32 inj = w.plant(100, "NOP0 SENSE INJECT NOP1 INC INC INC INC", 100);
        u32 vic = w.plant(200, "NOP0 INC INC INC INC INC INC INC", 100);
        CHECK(inj != kNone && vic != kNone);
        w.run(inj, 3); // NOP0, SENSE (CX = 8), INJECT keyed "1": 8 codons from my start
        CHECK(w.s.tally().injects == 1);
        CHECK(w.s.mem()[200] == w.s.mem()[100] && w.s.mem()[207] == w.s.mem()[107]);
        if (!on) {
            CHECK(w.L.links() == 0);
            continue;
        }
        CHECK(w.L.links() == 1 && w.L.inject_links() == 1 && w.L.founded() == 1);
        BlockTag ti = w.L.tag(w.s.body(inj).blocks), tv = w.L.tag(w.s.body(vic).blocks);
        CHECK(ti.lineage == 1 && ti.depth == 0 && tv.lineage == 1 && tv.depth == 1);
        w.run(inj, 7); // INC x4, NOP0, SENSE, INJECT: her again
        CHECK(w.s.tally().injects == 2 && w.L.links() == 2);
        const LineageInfo* info = w.L.lineage(1);
        CHECK(info && info->members == 2 && info->alive == 2 && info->inject_links == 2);
    }
}

// The replicator the home rig found first (design.md 6.6): it buds blank
// daughters and blind-injects itself into them. Under the Phase 1a contest it
// replicates and the detector sees it through INJECT links (and not at all
// without them: it has no COPY). Under INJECT's key rule its blind INJECT
// never lands, so it no longer replicates.
void bud_and_inject_replicator() {
    const char* genome = "seed.genome = NOP0 POP NOP0 PUSH NOP0 POP SENSE INJECT ALLOC AND SENSE ADD "
                         "PUSH NOP0 POP SENSE INJECT DIVIDE OR SENSE SHL PUSH NOP0 POP EMIT GPU "
                         "DIVIDE OR SENSE DIVIDE OR NOP0\nseed.millicores = 10\n";
    TextBuf old_rules, old_off, keyed;
    old_rules.append(genome);
    old_rules.append("contact.inject_key = false\ncontact.inject_room = 0\n");
    old_off.append(old_rules.view());
    old_off.append("detect.inject = false\n");
    keyed.append(genome);
    Outcome on = run_for(old_rules.c_str(), 50);
    Outcome off = run_for(old_off.c_str(), 50);
    Outcome now = run_for(keyed.c_str(), 50);
    CHECK(on.detections == 1 && on.max_depth >= 2 && on.first_tick < 20);
    CHECK(off.detections == 0 && off.links == 0);
    CHECK_EQ_U64(on.hash, off.hash); // still a pure observer
    CHECK(now.detections == 0 && now.links == 0);
    printf("  detect: bud-and-inject replicator: gate met at tick %llu under the Phase 1a "
           "contest; never under the key rule\n",
           static_cast<unsigned long long>(on.first_tick));
}

// The gate's thresholds are knobs.
void gate_knobs() {
    TextBuf cfg;
    cfg.append(kShare);
    cfg.append("detect.gate_depth = 1000\n");
    Outcome o = run_for(cfg.c_str(), 30);
    CHECK(o.ok && o.detections == 0 && o.links > 0);
    TextBuf bad;
    bad.append(kSeeded);
    bad.append("detect.gate_alive = 0\n");
    Run run;
    Config::ParseError pe;
    TextBuf err;
    CHECK(run.config().parse(bad.c_str(), &pe) && !run.init(1, &err));
}

// The detector only watches: the world hash is identical with it on or off,
// in a world where it fires and in the busy soup world.
void pure_observer() {
    Outcome on = run_for(kShare, 60);
    TextBuf cfg;
    cfg.append(kShare);
    cfg.append("detect.enabled = false\n");
    Outcome off2 = run_for(cfg.c_str(), 60);
    CHECK(on.detections == 1 && off2.detections == 0);
    CHECK_EQ_U64(on.hash, off2.hash);

    TextBuf busy_off;
    busy_off.append(kSoupWorld);
    busy_off.append("detect.enabled = false\n");
    Run a, b;
    CHECK(make_run(a, kSoupWorld, 4));
    CHECK(make_run(b, busy_off.c_str(), 4));
    a.run_until(200);
    b.run_until(200);
    CHECK(a.world().node_lineages(0) != nullptr && b.world().node_lineages(0) == nullptr);
    CHECK_EQ_U64(a.world().hash(), b.world().hash());
}

// Lineage bookkeeping: founder plus members, living counts, extinction, the
// survey and the genebank names.
void bookkeeping() {
    TextBuf cfg;
    cfg.append(kSeeded);
    cfg.append(kShare);
    Run run;
    CHECK(make_run(run, cfg.c_str(), 1));
    run.run_until(20);
    const Lineages* L = run.world().node_lineages(0);
    CHECK(L != nullptr);
    if (!L)
        return;
    const LineageInfo* info = L->lineage(1);
    CHECK(info && info->founder_len == 8 && info->members >= 3 && info->peak_alive >= 2);
    CHECK(info && info->detected == 1);
    CHECK(info && info->max_depth >= 2 && info->alive >= 2);
    // The survey sees the living: founder and funded daughters (depth 1); the
    // unfunded granddaughters died at the end of their birth tick.
    LineageSurvey sv;
    L->survey(&sv);
    CHECK(sv.lineages >= 1 && sv.top_lineage == 1 && sv.top_alive >= 2 && sv.max_depth >= 1);
    CHECK(sv.top_genotypes == 1);
    // No hazards: every member is an exact copy, so one genotype, saved once
    // its living count reached the threshold (2) and named like Tierra's.
    const Genebank& gb = L->genebank();
    CHECK(gb.saved() == 1);
    TextBuf name;
    Genebank::format_name(8, 0, name);
    CHECK(name.view().eq("0008aaa"));
    CHECK(run.detection().found && run.detection().len == 8);
    CHECK(run.detection_genome().eq("SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE"));

    // Names: four digits of length, then base-26 letters.
    TextBuf n2;
    Genebank::format_name(80, 27, n2);
    n2.append_char(' ');
    Genebank::format_name(12345, 17576, n2);
    CHECK(n2.view().eq("0080abb 12345baaa"));
}

// The genebank and lineage files a run writes.
void output_files() {
    const char* bank = "nova_test_genebank.txt";
    const char* lin = "nova_test_lineages.csv";
    TextBuf cfg;
    cfg.append(kSeeded);
    cfg.append(kShare);
    Run run;
    CHECK(make_run(run, cfg.c_str(), 1));
    TextBuf err;
    CHECK(run.open_genebank(bank, &err));
    CHECK(run.open_lineages(lin, &err));
    run.run_until(40);
    run.finish();

    Array<u8> bytes;
    CHECK(read_file(bank, &bytes));
    bytes.push(0);
    const char* text = reinterpret_cast<const char*>(bytes.data());
    CHECK(strstr(text, "\n0008aaa tick=") != nullptr);
    CHECK(strstr(text, " parent=none ") != nullptr); // the founder's genotype
    CHECK(strstr(text, "\n  SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE\n") != nullptr);

    CHECK(read_file(lin, &bytes));
    bytes.push(0);
    text = reinterpret_cast<const char*>(bytes.data());
    CHECK(strncmp(text, "lineage,node,founded,ended,status,", 34) == 0);
    CHECK(strstr(text, "\n0.1,0,") != nullptr); // lineage ids are node.count (Phase 3)
    remove(bank);
    remove(lin);
}

// Gate events go into the replay log and replay exactly at 1 and 20 threads,
// across nodes in node order; a tampered or missing event is caught.
void event_replay() {
    const char* path = "nova_test_detect.replay";
    TextBuf cfg;
    cfg.append(kSeeded4);
    cfg.append(kShare);
    Run run;
    CHECK(make_run(run, cfg.c_str(), 20));
    TextBuf err;
    CHECK(run.open_replay(path, 10, &err));
    run.run_until(40);
    run.finish();
    CHECK(run.event_count() == 4); // one gate per node
    bool node_order = true;
    for (usize i = 1; i < run.event_count(); ++i)
        node_order = node_order && run.event_tick(i) >= run.event_tick(i - 1);
    CHECK(node_order);

    ReplayLog log;
    CHECK(log.load_file(path, &err));
    CHECK(log.version() == 2 && log.event_count() == 4);
    ReplayResult r;
    CHECK(replay_verify(log, 1, &r));
    CHECK(r.ok && r.events_checked == 4);
    CHECK(replay_verify(log, 20, &r));

    Array<u8> bytes;
    CHECK(read_file(path, &bytes));
    {
        TextBuf t; // a different event text
        t.append(Str(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        char* p = strstr(const_cast<char*>(t.c_str()), " alive=");
        CHECK(p != nullptr);
        if (p)
            p[7] = p[7] == '9' ? '8' : '9';
        ReplayLog bad;
        CHECK(bad.parse(t.view(), &err));
        CHECK(!replay_verify(bad, 4, &r));
        CHECK(strstr(r.msg, "event") != nullptr);
    }
    {
        TextBuf t; // an event removed
        Str all2(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        usize i = 0;
        bool dropped = false;
        while (i < all2.n) {
            usize e = i;
            while (e < all2.n && all2.p[e] != '\n')
                ++e;
            Str line = all2.sub(i, e - i);
            if (!dropped && line.starts_with("event ")) {
                dropped = true;
            } else {
                t.append(line);
                t.append_char('\n');
            }
            i = e + 1;
        }
        ReplayLog bad;
        CHECK(bad.parse(t.view(), &err));
        CHECK(bad.event_count() == 3);
        CHECK(!replay_verify(bad, 1, &r));
        CHECK(strstr(r.msg, "event") != nullptr);
    }
    remove(path);
}

// A snapshot resume starts detection afresh and changes nothing in the world.
void snapshot_resume() {
    TextBuf cfg;
    cfg.append(kSeeded);
    cfg.append(kShare);
    Run straight;
    CHECK(make_run(straight, cfg.c_str(), 1));
    straight.run_until(30);

    Run first;
    CHECK(make_run(first, cfg.c_str(), 1));
    first.run_until(15);
    ByteWriter bytes;
    first.snapshot(bytes);
    Run resumed;
    TextBuf err;
    CHECK(resumed.init_from_snapshot(bytes.data(), bytes.size(), 1, &err));
    resumed.run_until(30);
    CHECK_EQ_U64(resumed.world().hash(), straight.world().hash());
    const Lineages* L = resumed.world().node_lineages(0);
    CHECK(L && L->founded() >= 1); // new links found lineages again
}

} // namespace

void suite_detect() {
    share_fires();
    sketch_does_not_fire();
    no_copy_no_link();
    random_budding_never_links();
    remains_do_not_count();
    inject_links();
    bud_and_inject_replicator();
    gate_knobs();
    pure_observer();
    bookkeeping();
    output_files();
    event_replay();
    snapshot_resume();
}
