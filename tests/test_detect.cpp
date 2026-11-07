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

// Descent, not likeness (disasters plan X12). A hand-built birth: the parent's
// code at 100 and the daughter's block at 300 (each a body made through the
// test API, as DIVIDE leaves them), every daughter codon marked as written by
// the parent's body, or none; then the DIVIDE hook.
const u32 kParLen = 24;

void parent_code(u8* out) { // 24 distinct codons
    for (u32 i = 0; i < kParLen; ++i)
        out[i] = static_cast<u8>((i * 7 + 3) % 64);
}

// One of the first 8 codons not in parent_code, picked by i mod 8.
u8 foreign_codon(u32 i) {
    u8 par[kParLen];
    parent_code(par);
    u32 found = 0;
    for (u32 v = 0; v < 64; ++v) {
        bool in = false;
        for (u32 j = 0; j < kParLen; ++j)
            in = in || par[j] == v;
        if (!in && found++ == i % 8)
            return static_cast<u8>(v);
    }
    return 0;
}

struct Birth {
    Watched w;
    u32 parent = kNone, daughter = kNone;
    bool make(const u8* dau, u32 dlen, bool written, const char* extra = "") {
        if (!w.init(extra))
            return false;
        u8 par[kParLen];
        parent_code(par);
        for (u32 i = 0; i < kParLen; ++i)
            w.s.mem_mut()[100 + i] = par[i];
        parent = w.s.create_body(100, kParLen, 100);
        for (u32 i = 0; i < dlen; ++i)
            w.s.mem_mut()[300 + i] = dau[i];
        daughter = w.s.create_body(300, dlen, 100);
        if (parent == kNone || daughter == kNone)
            return false;
        if (written)
            for (u32 i = 0; i < dlen; ++i)
                w.L.on_write(300 + i);
        w.L.on_divide(w.s, w.s.body(parent).blocks, w.s.body(daughter).blocks, 0);
        return true;
    }
    BlockTag ptag() const { return w.L.tag(w.s.body(parent).blocks); }
    BlockTag dtag() const { return w.L.tag(w.s.body(daughter).blocks); }
    // She joined her parent's lineage one generation deeper.
    bool joined() const {
        const BlockTag p = ptag(), d = dtag();
        return p.lineage != 0 && d.lineage == p.lineage && d.depth == p.depth + 1 && w.L.links() == 1;
    }
    bool apart() const { return dtag().lineage == 0 && ptag().lineage == 0 && w.L.links() == 0; }
};

// A rotation, a shift or a partial copy of her parent joins its lineage one
// generation deeper (each was a new lineage at depth 0 before X12), and so
// does her own rotated daughter, two deep. Unrelated codons, a copy her parent
// didn't write, and a partial copy short of the share don't join.
void descent_not_likeness() {
    u8 par[kParLen], dau[64];
    parent_code(par);
    {
        Birth b; // an exact copy, as always
        CHECK(b.make(par, kParLen, true) && b.joined() && b.dtag().depth == 1);
    }
    {
        Birth b; // a rotation: she starts 9 codons into the parent (beyond the shifts)
        for (u32 i = 0; i < kParLen; ++i)
            dau[i] = par[(i + 9) % kParLen];
        CHECK(b.make(dau, kParLen, true) && b.joined());
        // Her own daughter, rotated 5 more, joins two generations deep.
        u8 gd[kParLen];
        for (u32 i = 0; i < kParLen; ++i)
            gd[i] = dau[(i + 5) % kParLen];
        for (u32 i = 0; i < kParLen; ++i)
            b.w.s.mem_mut()[500 + i] = gd[i];
        const u32 g = b.w.s.create_body(500, kParLen, 100);
        CHECK(g != kNone);
        for (u32 i = 0; i < kParLen; ++i)
            b.w.L.on_write(500 + i);
        b.w.L.on_divide(b.w.s, b.w.s.body(b.daughter).blocks, b.w.s.body(g).blocks, 1);
        const BlockTag tg = b.w.L.tag(b.w.s.body(g).blocks);
        CHECK(tg.lineage == b.ptag().lineage && tg.depth == 2 && b.w.L.lineage(tg.lineage)->max_depth == 2);
        // ... and the same rotation lying as remains (not written) doesn't.
        Birth r;
        for (u32 i = 0; i < kParLen; ++i)
            dau[i] = par[(i + 9) % kParLen];
        CHECK(r.make(dau, kParLen, false) && r.apart());
    }
    {
        // A head two codons late: two codons of junk, then the parent from its
        // start (her first codons aren't in the parent, so only a shift finds
        // her). With detect.shift = 0 she is a stranger.
        dau[0] = foreign_codon(0);
        dau[1] = foreign_codon(1);
        for (u32 i = 2; i < kParLen; ++i)
            dau[i] = par[i - 2];
        Birth b;
        CHECK(b.make(dau, kParLen, true) && b.joined());
        Birth none;
        CHECK(none.make(dau, kParLen, true, "detect.shift = 0\n") && none.apart());
        // Three codons ahead, and stopping short: a partial copy, shifted.
        for (u32 i = 0; i < 18; ++i)
            dau[i] = par[i + 3];
        Birth s3;
        CHECK(s3.make(dau, 18, true) && s3.joined());
    }
    {
        Birth b; // a partial copy: the parent's first half, measured against her length
        CHECK(b.make(par, kParLen / 2, true) && b.joined());
        for (u32 i = 0; i < 12; ++i) // 8 of her 12 codons match: short of 3/4
            dau[i] = i < 8 ? par[i] : foreign_codon(i);
        Birth s;
        CHECK(s.make(dau, 12, true) && s.apart());
        for (u32 i = 0; i < 12; ++i) // 9 of 12 do
            dau[i] = i < 9 ? par[i] : foreign_codon(i);
        Birth t;
        CHECK(t.make(dau, 12, true) && t.joined());
    }
    {
        Birth b; // unrelated codons, written by the parent
        for (u32 i = 0; i < kParLen; ++i)
            dau[i] = foreign_codon(i);
        CHECK(b.make(dau, kParLen, true) && b.apart());
    }
    {
        Birth b; // a copy of twice the parent (her write head wrapped): all hers match
        for (u32 i = 0; i < 2 * kParLen; ++i)
            dau[i] = par[i % kParLen];
        CHECK(b.make(dau, 2 * kParLen, true) && b.joined());
    }
    // detect.shift is a knob, 0 to 64.
    Watched w;
    CHECK(w.init("detect.shift = 64\n") && w.d.shift == 64);
    Watched bad;
    CHECK(!bad.init("detect.shift = 65\n"));
}

// The same through the interpreter: a parent ALLOCs her, writes a rotation of
// itself into her, and DIVIDEs.
void rotation_through_divide() {
    Watched w;
    CHECK(w.init(""));
    const u32 b = w.plant(100, "ALLOC DIVIDE INC DEC SHL ZERO NOT PUSH POP SENSE OR AND ADD SUB NOP0 NOP1", 500);
    CHECK(b != kNone);
    Cpu& c = w.s.cpu_mut(w.s.body(b).cpus);
    c.bx = 0; // memory
    c.cx = 16;
    w.run(b, 1); // ALLOC
    const u32 k = w.s.body(b).latest;
    CHECK(k != kNone && w.s.block(k).len == 16);
    const u32 at = w.s.block(k).start;
    for (u32 i = 0; i < 16; ++i) { // a rotation by 6, as the parent's body would write it
        w.s.mem_mut()[at + i] = w.s.mem()[100 + (i + 6) % 16];
        w.L.on_write(at + i);
    }
    c.cx = 32;
    w.run(b, 1); // DIVIDE, here
    CHECK(w.s.tally().births == 1 && w.L.links() == 1 && w.L.founded() == 1);
    const BlockTag t = w.L.tag(k);
    CHECK(t.lineage == w.L.tag(w.s.body(b).blocks).lineage && t.depth == 1);
}

// A copy of body b's code at a free place, written by b's body, and the DIVIDE
// hook: b founds a lineage (if it isn't in one). Returns b's lineage.
u64 found_from(Watched& w, u32 b) {
    const u32 start = w.s.block(w.s.body(b).blocks).start, len = w.s.block(w.s.body(b).blocks).len;
    const u32 at = w.s.find_free_fwd(600, len);
    if (at == kNone)
        return 0;
    for (u32 i = 0; i < len; ++i)
        w.s.mem_mut()[at + i] = w.s.mem()[start + i];
    const u32 d = w.s.create_body(at, len, 0);
    if (d == kNone)
        return 0;
    for (u32 i = 0; i < len; ++i)
        w.L.on_write(at + i);
    w.L.on_divide(w.s, w.s.body(b).blocks, w.s.body(d).blocks, 0);
    return w.L.tag(w.s.body(b).blocks).lineage;
}

// Founders by origin (world plan W7): each body carries how it began, and a
// lineage records its founder's; the detector counts foundings by origin.
void founder_origins() {
    { // a spark
        Watched w;
        CHECK(w.init("spark.initial = 1\nspark.grant = 10\nspark.min_len = 12\nspark.max_len = 12\n"));
        CHECK(w.s.body_count() == 1);
        const u32 b = w.s.body_at(0);
        CHECK(w.s.origin(b) == kOriginSpark);
        const u64 id = found_from(w, b);
        CHECK(id != 0 && w.L.lineage(id)->founder_origin == kOriginSpark);
        CHECK(w.L.founded_by(kOriginSpark) == 1 && w.L.founded() == 1);
        // A member dispersing carries her founder's origin, so the lineage's
        // entry on the next node names the same founder (and isn't a founding there).
        const u32 k = w.s.body(b).blocks;
        const CargoTag t = w.L.cargo_tag(k);
        CHECK(t.lineage == id && t.origin == kOriginSpark);
        Watched there;
        CHECK(there.init(""));
        CHECK(there.s.arrive(w.s.mem() + w.s.block(k).start, w.s.block(k).len, 50, 0, 0, 0, t));
        const LineageInfo* li = there.L.lineage(id);
        CHECK(li && li->founder_origin == kOriginSpark && li->alive == 1 && there.L.founded() == 0);
    }
    { // a daughter: budded off (not a copy), she then founds one of her own
        Watched w;
        CHECK(w.init(""));
        const u32 p = w.plant(100, "ALLOC DIVIDE INC INC", 640);
        Cpu& c = w.s.cpu_mut(w.s.body(p).cpus);
        c.bx = 0;
        c.cx = 6;
        w.run(p, 1);
        const u32 k = w.s.body(p).latest;
        c.cx = 32;
        w.run(p, 1);
        CHECK(w.s.tally().births == 1 && w.L.links() == 0 && k != kNone);
        const u32 d = w.s.block(k).body;
        CHECK(d != p && w.s.origin(d) == kOriginDaughter && w.s.origin(p) == kOriginUnknown);
        const u64 id = found_from(w, d);
        CHECK(id != 0 && w.L.lineage(id)->founder_origin == kOriginDaughter && w.L.founded_by(kOriginDaughter) == 1);
    }
    { // an arrival, untagged
        Watched w;
        CHECK(w.init(""));
        u8 code[10];
        for (u32 i = 0; i < 10; ++i)
            code[i] = static_cast<u8>(10 + i);
        CHECK(w.s.arrive(code, 10, 50, 0, 0, 0, CargoTag{}));
        const u32 b = w.s.body_at(w.s.body_count() - 1);
        CHECK(w.s.origin(b) == kOriginArrival);
        const u64 id = found_from(w, b);
        CHECK(id != 0 && w.L.lineage(id)->founder_origin == kOriginArrival && w.L.founded_by(kOriginArrival) == 1);
    }
    { // woke from dormancy, untagged
        Watched w;
        CHECK(w.init("life.dormancy = true\nworld.disk = 1024\n"));
        const u32 b = w.plant(100, "ALLOC INC INC INC", 200);
        Cpu& c = w.s.cpu_mut(w.s.body(b).cpus);
        c.bx = 3; // disk: its reserve
        c.cx = 16;
        w.run(b, 1);
        CHECK(w.s.force_dormant(b) && w.s.dormant(b)); // a hermit: it rests here
        w.s.begin_tick(1);
        w.s.death_sweep(); // and wakes on its own freed millicores
        CHECK(!w.s.dormant(b) && w.s.body(b).alive && w.s.origin(b) == kOriginWake);
        const u64 id = found_from(w, b);
        CHECK(id != 0 && w.L.lineage(id)->founder_origin == kOriginWake && w.L.founded_by(kOriginWake) == 1);
    }
    { // made through the test API (as a seeded genome is): unknown
        Watched w;
        CHECK(w.init(""));
        const u32 b = w.plant(100, "INC DEC INC DEC SHL", 100);
        CHECK(w.s.origin(b) == kOriginUnknown);
        const u64 id = found_from(w, b);
        CHECK(id != 0 && w.L.lineage(id)->founder_origin == kOriginUnknown && w.L.founded_by(kOriginUnknown) == 1);
    }
    CHECK(strcmp(origin_name(kOriginSpark), "spark") == 0 && strcmp(origin_name(kOriginDaughter), "daughter") == 0 &&
          strcmp(origin_name(kOriginArrival), "arrival") == 0 && strcmp(origin_name(kOriginWake), "wake") == 0 &&
          strcmp(origin_name(kOriginUnknown), "unknown") == 0);
}

// The sum of a stats.csv column over its rows (0 if there is no such column).
u64 csv_column_sum(const char* text, const char* name) {
    const usize nlen = strlen(name);
    // Which column: count commas in the header up to the name.
    const char* eol = strchr(text, '\n');
    u32 col = 0;
    bool found = false;
    for (const char* p = text; p && p < eol;) {
        const char* end = p;
        while (end < eol && *end != ',')
            ++end;
        if (static_cast<usize>(end - p) == nlen && strncmp(p, name, nlen) == 0) {
            found = true;
            break;
        }
        ++col;
        p = end + 1;
    }
    if (!found || !eol)
        return 0;
    u64 sum = 0;
    for (const char* line = eol + 1; *line;) {
        const char* p = line;
        for (u32 c = 0; c < col && *p && *p != '\n'; ++p)
            c += *p == ',';
        u64 v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + static_cast<u64>(*p++ - '0');
        sum += v;
        const char* next = strchr(line, '\n');
        if (!next)
            break;
        line = next + 1;
    }
    return sum;
}

// The four stats columns count, per window, the lineages founded by a spark,
// a daughter, an arrival and a woken program, in a dispersal world with
// dormancy: summed over the run they are the detectors' counts by origin, and
// with the seeded founders (unknown) every lineage founded.
void founders_in_the_csv() {
    const char* csv = "nova_test_founders.csv";
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append("life.frozen_ticks = 40\nlife.dormancy = true\nmem.upkeep = 6\nworld.disk = 1024\n");
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    TextBuf err;
    CHECK(run.open_stats(csv, 50, &err));
    run.run_until(400);
    run.finish();
    u64 by[kOriginCount] = {}, founded = 0;
    for (u32 n = 0; n < run.world().node_count(); ++n) {
        const Lineages* L = run.world().node_lineages(n);
        CHECK(L != nullptr);
        if (!L)
            return;
        founded += L->founded();
        for (u32 o = 0; o < kOriginCount; ++o)
            by[o] += L->founded_by(o);
    }
    Array<u8> bytes;
    CHECK(read_file(csv, &bytes));
    bytes.push(0);
    const char* text = reinterpret_cast<const char*>(bytes.data());
    const u64 spark = csv_column_sum(text, "founded_spark"), daughter = csv_column_sum(text, "founded_daughter"),
              arrival = csv_column_sum(text, "founded_arrival"), wake = csv_column_sum(text, "founded_wake");
    CHECK(spark == by[kOriginSpark] && daughter == by[kOriginDaughter] && arrival == by[kOriginArrival] &&
          wake == by[kOriginWake]);
    CHECK(spark + daughter + arrival + wake + by[kOriginUnknown] == founded);
    CHECK(by[kOriginUnknown] >= 1 && founded > by[kOriginUnknown]); // the seeds, and life since
    printf("  detect: founders by origin, 24-node dispersal world, 400 ticks: %llu spark, %llu daughter, "
           "%llu arrival, %llu wake, %llu unknown (seeded)\n",
           static_cast<unsigned long long>(spark), static_cast<unsigned long long>(daughter),
           static_cast<unsigned long long>(arrival), static_cast<unsigned long long>(wake),
           static_cast<unsigned long long>(by[kOriginUnknown]));
    remove(csv);
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
    CHECK(strstr(text, ",founder_genotype,detected,founder_origin\n") != nullptr); // W7: the last column
    CHECK(strstr(text, "\n0.1,0,") != nullptr); // lineage ids are node.count (Phase 3)
    const char* row = strstr(text, "\n0.1,0,");
    const char* eol = row ? strchr(row + 1, '\n') : nullptr;
    CHECK(eol && eol - row > 8 && strncmp(eol - 8, ",unknown", 8) == 0); // the seeded founder
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
    CHECK(log.version() == kReplayVersion && log.event_count() == 4); // (events since v2; v3 since the integration)
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
    // W7: origins aren't snapshotted, so every body alive at the resume began
    // unknown (the daughters born before it included).
    const Soup& rs = resumed.world().node_soup(0);
    bool unknown = rs.body_count() > 1;
    for (usize i = 0; i < rs.body_count(); ++i)
        unknown = unknown && rs.origin(rs.body_at(i)) == kOriginUnknown;
    CHECK(unknown);
    const Soup& fs = first.world().node_soup(0);
    usize daughters = 0;
    for (usize i = 0; i < fs.body_count(); ++i)
        daughters += fs.origin(fs.body_at(i)) == kOriginDaughter;
    CHECK(daughters > 0); // (they were daughters before it)
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
    descent_not_likeness();
    rotation_through_divide();
    founder_origins();
    founders_in_the_csv();
    inject_links();
    bud_and_inject_replicator();
    gate_knobs();
    pure_observer();
    bookkeeping();
    output_files();
    event_replay();
    snapshot_resume();
}
