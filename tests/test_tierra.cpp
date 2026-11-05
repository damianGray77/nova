// Gate 1 (design.md 9.1): the light Tierra mode runs Ray's ancestor 0080aaa
// with his published replication cost.
//
// Which published cost: Ray's 1991/1992 papers give 839 instructions for the
// first replication and 813 for each one after. Those came from his original
// 1990 simulator, which is lost. Every later Tierra release Ray published lists
// the byte-identical 80-instruction genome at 827 / 809 (V3.0 1991 through
// V5.0 on tomray.me), under semantics his docs state: adr/adrb/adrf and call
// step over their own templates. Reaching 839/813 needs two behaviours no Ray
// source documents, so this gate checks 827/809 under the documented
// semantics (design.md 6.5). The research is in the side-session notes,
// tierra-ancestor-research.md.
#include "check.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

// Ray 1991, Appendix C (identical in Ray 1992 App. C and Tierra's 0080aaa.tie).
const u8 kAncestor[80] = {
    0x01, 0x01, 0x01, 0x01, 0x04, 0x02, 0x03, 0x03, 0x18, 0x1c, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x19, 0x1d, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x01, 0x01, 0x00, 0x01, 0x1e,
    0x16, 0x00, 0x00, 0x01, 0x01, 0x1f, 0x14, 0x00, 0x00, 0x01, 0x00, 0x05, 0x01, 0x01,
    0x00, 0x00, 0x0c, 0x0d, 0x0e, 0x01, 0x00, 0x01, 0x00, 0x1a, 0x0a, 0x05, 0x14, 0x00,
    0x01, 0x00, 0x00, 0x08, 0x09, 0x14, 0x00, 0x01, 0x00, 0x01, 0x05, 0x01, 0x00, 0x01,
    0x01, 0x12, 0x11, 0x10, 0x17, 0x01, 0x01, 0x01, 0x00, 0x05,
};

const char* const kTierraMode = "isa.table = tierra0\n"
                                "isa.membrane = open\n"
                                "heat.enabled = false\n"
                                "hazard.copy = 0\n"
                                "hazard.flaw = 0\n"
                                "hazard.rot = 0\n";

struct Counts {
    u32 per_divide[3] = {};
    u32 divides = 0;
    bool daughters_exact = true;
    bool ok = false;
};

// Runs one CPU instruction by instruction, recording how many it executes up to
// and including each successful divide (Ray's convention, Tierra V3 bookeep.c).
Counts count_divides(Soup& s, u32 body, u32 want) {
    Counts r;
    u32 cpu = s.body(body).cpus;
    u64 births = s.tally().births;
    u32 n = 0;
    for (u32 guard = 0; guard < 20000 && r.divides < want; ++guard) {
        u32 latest = s.body(body).latest;
        s.run_cpu(cpu, 1);
        ++n;
        if (s.tally().births != births) {
            births = s.tally().births;
            r.per_divide[r.divides++] = n;
            n = 0;
            const Block& d = s.block(latest);
            bool exact = d.len == 80;
            for (u32 i = 0; i < 80 && exact; ++i)
                exact = s.mem()[d.start + i] == kAncestor[i];
            r.daughters_exact = r.daughters_exact && exact;
        }
    }
    r.ok = r.divides == want && !s.cpu(cpu).halted;
    return r;
}

void ancestor_cost(u64 seed) {
    SoupParams p;
    CHECK(make_params(&p, kTierraMode, 4096));
    Soup s;
    s.init(&p, seed, 0); // random waste all round, as in a Nova soup
    const u32 at = 1000;
    for (u32 i = 0; i < 80; ++i)
        s.mem_mut()[at + i] = kAncestor[i];
    u32 mother = s.create_body(at, 80, 1000);
    CHECK(mother != kNone);
    s.begin_tick(0);

    Counts m = count_divides(s, mother, 3);
    CHECK(m.ok);
    CHECK(m.per_divide[0] == 827); // first replication
    CHECK(m.per_divide[1] == 809); // each one after
    CHECK(m.per_divide[2] == 809);
    CHECK(m.daughters_exact); // breeds true
    CHECK(soup_ok(s));

    // The first daughter is a full Tierran too: same cost from her own birth.
    u32 daughter = kNone;
    for (usize k = 0; k < s.body_count(); ++k)
        if (s.body_at(k) != mother && daughter == kNone)
            daughter = s.body_at(k);
    CHECK(daughter != kNone);
    if (daughter != kNone) {
        Counts d = count_divides(s, daughter, 2);
        CHECK(d.ok && d.per_divide[0] == 827 && d.per_divide[1] == 809 && d.daughters_exact);
    }
    // tierra0's divide hands over a fixed share (tierra.divide_share = 32/64).
    CHECK(s.body(mother).units < 1000 && s.body(mother).units > 0);

    if (seed == 1)
        printf("  tierra: ancestor 0080aaa replicates in %u then %u, %u instructions "
               "(Ray's V3+ figures 827/809)\n",
               m.per_divide[0], m.per_divide[1], m.per_divide[2]);
}

// The open-membrane flag is what lets a Tierran read a neighbour's code (the
// route Tierra's parasites use). Under private code the same movii fails.
void membrane_matters() {
    SoupParams open, priv;
    CHECK(make_params(&open, kTierraMode, 4096));
    CHECK(make_params(&priv,
                      "isa.table = tierra0\nheat.enabled = false\nhazard.copy = 0\n"
                      "hazard.flaw = 0\nhazard.rot = 0\n",
                      4096));
    // A parasite-like probe: movii reads [BX] from someone else's body.
    for (int mode = 0; mode < 2; ++mode) {
        Soup s;
        s.init(mode ? &priv : &open, 9, 0);
        for (u32 i = 0; i < 80; ++i)
            s.mem_mut()[100 + i] = kAncestor[i];
        u32 host = s.create_body(100, 80, 100);
        // probe: mal (cx=0 -> fails harmlessly) is not needed; set regs directly.
        s.mem_mut()[500] = 0x1a; // movii
        u32 probe = s.create_body(500, 1, 100);
        CHECK(host != kNone && probe != kNone);
        u32 c = s.body(probe).cpus;
        s.cpu_mut(c).ax = 500;  // write into its own body
        s.cpu_mut(c).bx = 110;  // read the host's code
        s.begin_tick(0);
        s.run_cpu(c, 1);
        CHECK(s.cpu(c).err == (mode ? 1 : 0));
    }
}

} // namespace

void suite_tierra() {
    ancestor_cost(1);
    ancestor_cost(2);
    ancestor_cost(3);
    membrane_matters();
}
