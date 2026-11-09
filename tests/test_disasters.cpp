// Disasters (docs/disasters-plan.md X1-X11): power surges, OS crashes,
// burnouts, disk failures and lightning storms, their symptoms and sounds.
// Each disaster on a small world at high rates; the symptoms raising their
// quantities on the right nodes, cores or disk over their lead time, and
// false alarms fading with no disaster; the sounds at the source at full
// strength and fainter and later a hop away, silent without signals; and a
// busy world with every disaster frequent the same at 1, 4 and 20 threads,
// under shuffled dispatch, and through a snapshot taken during a reboot with
// cores offline. (Every other suite runs with disasters off, the legacy
// defaults, and keeps its hashes.)
#include <cmath>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/keys.h"
#include "nova/landscape.h"
#include "nova/lineage.h"
#include "nova/snapshot.h"
#include "soup_helpers.h"

using namespace nova;

extern const char* const kLegacyDefaults; // test_main.cpp: the defaults the suites run on

namespace {

// " key=value" in an event's text: the value (0 if absent).
u64 ev(Str t, const char* key) {
    TextBuf k;
    k.append_char(' ');
    k.append(key);
    k.append_char('=');
    const Str kk = k.view();
    for (usize i = 0; i + kk.n <= t.n; ++i) {
        if (memcmp(t.p + i, kk.p, kk.n) != 0)
            continue;
        usize e = i + kk.n;
        while (e < t.n && t.p[e] != ' ')
            ++e;
        u64 v = 0;
        return parse_u64(t.sub(i + kk.n, e - i - kk.n), &v) ? v : 0;
    }
    return 0;
}

// The node of an event that starts "<kind> node=N".
u32 ev_node(Str t) {
    const usize at = t.find('=');
    if (at == t.n)
        return kNone;
    usize e = at + 1;
    while (e < t.n && t.p[e] != ' ')
        ++e;
    u64 v = 0;
    return parse_u64(t.sub(at + 1, e - at - 1), &v) ? static_cast<u32>(v) : kNone;
}

// Only the named kinds on (the rest off), symptoms off unless asked.
void only(TextBuf& cfg, const char* kinds, bool symptoms = false) {
    const char* const all[kDisKinds] = {"surge", "crash", "burnout", "diskfail", "storm"};
    cfg.append("disaster.enabled = true\n");
    for (const char* k : all) {
        cfg.append("disaster.");
        cfg.append(k);
        cfg.append(strstr(kinds, k) ? " = true\n" : " = false\n");
    }
    cfg.append(symptoms ? "disaster.symptoms = true\n" : "disaster.symptoms = false\n");
}

bool all_ok(const World& w) {
    bool ok = true;
    for (u32 n = 0; n < w.node_count(); ++n)
        ok = soup_ok(w.node_soup(n)) && ok;
    return ok;
}

// The live nodes within `hops` of `from` along the given edges (the graph as
// it was), breadth first.
void region(const Array<u32>& ob, const Array<u32>& dst, const Array<u8>& live, u32 from, u32 hops, Array<u32>* out) {
    out->clear();
    Array<u32> dist;
    dist.resize(live.size());
    dist.fill(kNone);
    dist[from] = 0;
    out->push(from);
    for (usize q = 0; q < out->size(); ++q) {
        const u32 u = (*out)[q];
        if (dist[u] >= hops)
            continue;
        for (u32 x = ob[u]; x < ob[u + 1]; ++x) {
            const u32 v = dst[x];
            if (dist[v] == kNone && live[v]) {
                dist[v] = dist[u] + 1;
                out->push(v);
            }
        }
    }
}

// X2: a surge collapses the epicentre and every live node within its hops
// (the graph as it stood), nothing else, and the nodes come back through the
// lifecycle's recovery.
void test_surge() {
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    only(cfg, "surge");
    cfg.append("disaster.surge_every = 60\nnode.fallow_min = 40\nnode.recover_every = 10\n");
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    World& w = run.world();
    CHECK(w.lifecycle() && w.disasters()); // surges bring the lifecycle with them
    u32 surges = 0, exact = 0, recovered = 0;
    usize seen = 0;
    Array<u32> ob, dst, reg;
    Array<u8> live;
    for (u64 t = 0; t < 600; ++t) {
        ob.clear();
        dst.clear();
        live.clear();
        for (u32 n = 0; n <= w.node_count(); ++n)
            ob.push(n < w.node_count() ? w.out_begin(n) : w.edge_count());
        for (u32 e = 0; e < w.edge_count(); ++e)
            dst.push(w.edge_dst(e));
        for (u32 n = 0; n < w.node_count(); ++n)
            live.push(w.live(n) ? 1 : 0);
        run.step();
        u32 in_step = 0;
        for (usize i = seen; i < run.event_count(); ++i)
            in_step += run.event_text(i).starts_with("surge ") ? 1 : 0;
        for (; seen < run.event_count(); ++seen) {
            const Str e = run.event_text(seen);
            recovered += e.starts_with("recovered ") ? 1 : 0;
            if (!e.starts_with("surge "))
                continue;
            ++surges;
            const u32 epi = ev_node(e), hops = static_cast<u32>(ev(e, "hops"));
            CHECK(hops >= 1 && hops <= 2 && live[epi]);
            region(ob, dst, live, epi, hops, &reg);
            CHECK(reg.size() == ev(e, "nodes"));
            bool gone = true;
            for (usize i = 0; i < reg.size(); ++i)
                gone = gone && !w.live(reg[i]);
            CHECK(gone);
            if (in_step == 1) { // (alone at its barrier: nothing else collapsed)
                u32 others = 0;
                for (u32 n = 0; n < w.node_count(); ++n) {
                    bool in = false;
                    for (usize i = 0; i < reg.size() && !in; ++i)
                        in = reg[i] == n;
                    others += live[n] && !in && !w.live(n) ? 1 : 0;
                }
                CHECK(others == 0);
                ++exact;
            }
        }
    }
    CHECK(surges > 0 && exact > 0 && recovered > 0 && w.live_nodes() > 0);
    CHECK(all_ok(w));
    printf("  surge: %u surges in 600 ticks (%u checked alone), %u recoveries, %llu live nodes now\n", surges, exact,
           recovered, static_cast<unsigned long long>(w.live_nodes()));
}

// X3: a crash deletes about the drawn share of the node's programs, restarts
// the rest (threads at their body's start, registers cleared), and the node
// runs nothing and takes no arrivals until its reboot is over.
void test_crash() {
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    only(cfg, "crash");
    cfg.append("disaster.crash_every = 30\ndisaster.reboot_min = 10\ndisaster.reboot_max = 30\n");
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    World& w = run.world();
    u32 crashes = 0, idle_ok = 0, idle_bad = 0, ran_after = 0;
    u64 deleted = 0, restarted = 0, refused = 0;
    usize seen = 0;
    Array<u64> until; // per node: its reboot's end, and its programs then
    Array<u64> kept;
    until.resize(w.node_count());
    kept.resize(w.node_count());
    for (u64 t = 0; t < 500; ++t) {
        run.step();
        const u64 ran = w.tick() - 1; // the tick just run
        for (u32 n = 0; n < w.node_count(); ++n) {
            if (until[n] == 0)
                continue;
            const SoupTally& st = w.node_soup(n).tally();
            if (ran < until[n]) { // down: nothing ran, nothing arrived or was born
                const bool quiet = st.instructions == 0 && st.births == 0 && st.arrived == 0 &&
                                   w.node_soup(n).body_count() <= kept[n];
                idle_ok += quiet ? 1 : 0;
                idle_bad += quiet ? 0 : 1;
                refused += st.lost;
            } else if (ran == until[n]) {
                ran_after += st.instructions > 0 ? 1 : 0;
                until[n] = 0;
            }
        }
        for (; seen < run.event_count(); ++seen) {
            const Str e = run.event_text(seen);
            if (!e.starts_with("crash "))
                continue;
            ++crashes;
            const u32 n = ev_node(e);
            const u64 d = ev(e, "deleted"), s = ev(e, "restarted"), r = ev(e, "reboot");
            deleted += d;
            restarted += s;
            const Soup& sp = w.node_soup(n);
            CHECK(sp.body_count() == s); // the survivors (no dormancy here)
            if (d + s > 0) {
                const double share = static_cast<double>(d) / static_cast<double>(d + s);
                CHECK(share >= 0.3 - 0.5 / (d + s) && share <= 0.8 + 0.5 / (d + s)); // (the default 30-80%)
            }
            bool fresh = true;
            for (usize k = 0; k < sp.body_count(); ++k) {
                const u32 b = sp.body_at(k);
                u32 start = 0, hi = 0; // (a parasite's: its code in its host, docs/inject-plan.md M2)
                if (sp.is_parasite(b)) {
                    sp.parasite_span(b, &start, &hi);
                    if (hi <= start)
                        start = sp.block(sp.body(sp.host_of(b)).blocks).start;
                } else
                    start = sp.block(sp.body(b).blocks).start;
                for (u32 c = sp.body(b).cpus; c != kNone; c = sp.cpu(c).next) {
                    const Cpu& C = sp.cpu(c);
                    fresh = fresh && C.ip == start && C.rh == start && C.wh == start && C.ax == 0 && C.bx == 0 &&
                            C.cx == 0 && C.dx == 0 && C.sp == 0 && !C.halted && C.errs == 0;
                }
            }
            CHECK(fresh);
            CHECK(w.rebooting(n) && w.reboot_until(n) == run.event_tick(seen) + 1 + r && r >= 10 && r <= 30);
            until[n] = w.reboot_until(n);
            kept[n] = s;
        }
    }
    CHECK(crashes > 0 && deleted > 0 && restarted > 0 && idle_ok > 0 && idle_bad == 0 && ran_after > 0);
    CHECK(refused > 0); // daughters sent to a rebooting node are lost
    CHECK(all_ok(w));
    printf("  crash: %u crashes in 500 ticks, %llu programs deleted, %llu restarted; %u node-ticks down and quiet, "
           "%llu arrivals refused\n",
           crashes, static_cast<unsigned long long>(deleted), static_cast<unsigned long long>(restarted), idle_ok,
           static_cast<unsigned long long>(refused));
}

const char* const kDormancy = "life.frozen_ticks = 40\nlife.dormancy = true\nmem.upkeep = 6\nworld.disk = 1024\n";

// X4: a burnout takes whole programs' cores offline, sends them dormant
// resting at home (some graced, some dying), and returns the cores, cool,
// at the barrier their repair is done.
void test_burnout() {
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append(kDormancy);
    only(cfg, "burnout");
    cfg.append("disaster.burnout_every = 20\ndisaster.repair_min = 100\ndisaster.repair_max = 100\n"
               "disaster.burnout_share_min = 0.2\n");
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    World& w = run.world();
    u32 burnouts = 0, late = 0;
    u64 cores = 0, dormant = 0, graced = 0, died = 0, most = 0;
    usize seen = 0;
    for (u64 t = 0; t < 500; ++t) {
        run.step();
        const u64 T = w.tick() - 1; // this barrier's tick
        for (u32 n = 0; n < w.node_count(); ++n) {
            const Array<OfflineCore>& off = w.node_soup(n).offline();
            for (usize i = 0; i < off.size(); ++i)
                late += off[i].until <= T ? 1 : 0; // repaired at its barrier, never later
        }
        most = w.cores_offline() > most ? w.cores_offline() : most;
        for (; seen < run.event_count(); ++seen) {
            const Str e = run.event_text(seen);
            if (!e.starts_with("burnout "))
                continue;
            ++burnouts;
            const u32 n = ev_node(e);
            const u64 c = ev(e, "cores"), d = ev(e, "dormant"), g = ev(e, "graced"), x = ev(e, "died");
            cores += c;
            dormant += d;
            graced += g;
            died += x;
            CHECK(ev(e, "repair") == 100);
            const Soup& sp = w.node_soup(n);
            u64 mine = 0, burnt = 0, kind = 0;
            for (usize i = 0; i < sp.offline().size(); ++i)
                mine += sp.offline()[i].until == T + 100 ? 1 : 0;
            for (usize k = 0; k < sp.body_count(); ++k) { // resting here, holding nothing but code
                const u32 b = sp.body_at(k);
                if (sp.dormant(b) && (sp.dormant_state(b).flags & kDormantBurnt)) {
                    ++burnt;
                    kind += (sp.dormant_state(b).flags & kDormantGraced) ? 1 : 0;
                }
            }
            CHECK(mine == c && burnt >= d && kind >= g);
        }
    }
    CHECK(burnouts > 0 && cores > 0 && dormant > 0 && graced > 0 && died > 0 && late == 0);
    CHECK(all_ok(w));
    printf("  burnout: %u burnouts in 500 ticks: %llu cores offline (at most %llu at once), %llu programs dormant (%llu "
           "graced), %llu died\n",
           burnouts, static_cast<unsigned long long>(cores), static_cast<unsigned long long>(most),
           static_cast<unsigned long long>(dormant), static_cast<unsigned long long>(graced),
           static_cast<unsigned long long>(died));
}

// X5: a disk failure leaves every disk codon INERT, no holdings and no
// resting program with a disk reserve.
void test_diskfail() {
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append(kDormancy);
    only(cfg, "diskfail");
    cfg.append("disaster.diskfail_every = 25\n");
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    World& w = run.world();
    u32 fails = 0, clean = 0;
    u64 lost = 0;
    usize seen = 0;
    for (u64 t = 0; t < 300; ++t) {
        run.step();
        for (; seen < run.event_count(); ++seen) {
            const Str e = run.event_text(seen);
            if (!e.starts_with("diskfail "))
                continue;
            ++fails;
            lost += ev(e, "codons");
            const Soup& sp = w.node_soup(ev_node(e));
            bool ok = sp.free_disk() == sp.disk().size();
            for (u32 a = 0; a < sp.disk().size(); ++a)
                ok = ok && sp.disk().mem[a] == 0;
            for (usize k = 0; k < sp.body_count(); ++k) {
                const u32 b = sp.body_at(k);
                ok = ok && sp.disk_held(b) == 0 && !(sp.dormant(b) && sp.dormant_state(b).disk_reserve);
            }
            clean += ok ? 1 : 0;
        }
    }
    CHECK(fails > 0 && clean == fails);
    CHECK(all_ok(w));
    printf("  disk failure: %u in 300 ticks, all INERT and unheld after; %llu codons were held\n", fails,
           static_cast<unsigned long long>(lost));
}

// X6: a storm moves along paths to the neighbour its heading points at
// (decided a move ahead), turning at most storm_turn a move, strikes only
// where it is, and dies out at the end of its lifetime; a strike randomises
// its run of codons, ownership and all left as it was.
void test_storms() {
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    only(cfg, "storm");
    cfg.append("disaster.storm_every = 60\ndisaster.storm_life_min = 300\ndisaster.storm_life_max = 600\n"
               "disaster.storm_move_min = 20\ndisaster.storm_move_max = 40\ndisaster.strike_every_min = 10\n"
               "disaster.strike_every_max = 20\ndisaster.strike_min = 64\ndisaster.strike_max = 128\n");
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    World& w = run.world();
    u32 formed = 0, moves = 0, strikes = 0, ended = 0, bad = 0;
    usize seen = 0;
    Array<Storm> prev;
    const double turn = 30.0 * 6.283185307179586 / 360.0;
    for (u64 t = 0; t < 1200; ++t) {
        prev.clear();
        for (u32 i = 0; i < w.storm_count(); ++i)
            prev.push(w.storm(i));
        run.step();
        const u64 T = w.tick() - 1;
        for (; seen < run.event_count(); ++seen) {
            const Str e = run.event_text(seen);
            if (e.starts_with("storm formed ")) {
                ++formed;
            } else if (e.starts_with("lightning ")) { // only where a storm was
                ++strikes;
                bool there = false;
                for (usize i = 0; i < prev.size() && !there; ++i)
                    there = prev[i].node == ev_node(e);
                const u64 c = ev(e, "codons");
                bad += there && c >= 1 && c <= 128 ? 0 : 1;
            } else if (e.starts_with("storm moved ")) {
                ++moves;
                const u32 from = static_cast<u32>(ev(e, "from")), to = static_cast<u32>(ev(e, "to"));
                const Storm* s = nullptr; // a storm there due to move, to where it had decided
                for (usize i = 0; i < prev.size() && !s; ++i)
                    if (prev[i].node == from && prev[i].next_move <= T && prev[i].dest == to)
                        s = &prev[i];
                bool ok = s != nullptr;
                bool adjacent = false;
                double best = -2;
                for (u32 x = w.out_begin(from); x < w.out_end(from); ++x) {
                    const u32 m = w.edge_dst(x);
                    adjacent = adjacent || m == to;
                    if (s)
                        best = fmax(best, cos(atan2(w.node_y(m) - w.node_y(from), w.node_x(m) - w.node_x(from)) -
                                              s->heading));
                }
                if (s) // the best match for its heading
                    ok = ok && cos(atan2(w.node_y(to) - w.node_y(from), w.node_x(to) - w.node_x(from)) - s->heading) >=
                                   best - 1e-12;
                bad += ok && adjacent ? 0 : 1;
            } else if (e.starts_with("storm ended ")) {
                ++ended;
                bool due = false;
                for (usize i = 0; i < prev.size() && !due; ++i)
                    due = prev[i].node == ev_node(e) && prev[i].end <= T;
                bad += due ? 0 : 1;
            }
        }
        for (u32 i = 0; i < w.storm_count(); ++i) { // a heading turns at most storm_turn a move
            for (usize j = 0; j < prev.size(); ++j) {
                if (prev[j].serial != w.storm(i).serial || prev[j].last_move == w.storm(i).last_move)
                    continue;
                double d = fabs(w.storm(i).heading - prev[j].heading);
                d = d > 3.141592653589793 ? 6.283185307179586 - d : d;
                bad += d <= turn + 1e-9 ? 0 : 1;
            }
            bad += w.storm(i).end > T ? 0 : 1;
        }
    }
    CHECK(formed > 0 && moves > 0 && strikes > 0 && ended > 0 && bad == 0);
    CHECK(static_cast<u64>(formed) == static_cast<u64>(ended) + w.storm_count());
    CHECK(all_ok(w));
    // A strike, on its own: random codons over the run, owners as they were.
    SoupParams p;
    CHECK(make_params(&p, kQuiet, 1024));
    Soup s;
    s.init(&p, 7, 0);
    s.begin_tick(0);
    const u32 b = plant(s, 100, "NOP1 INC INC INC INC INC INC INC INC INC INC INC INC INC INC INC", 100);
    Array<u8> before;
    before.assign(s.mem(), 1024);
    Rng r(1, 2, 3);
    CHECK(s.strike(96, 64, r) == 64 && s.strike(1000, 64, r) == 24); // (clipped at the end)
    u32 changed = 0;
    for (u32 a = 96; a < 160; ++a)
        changed += s.mem()[a] != before[a] ? 1 : 0;
    CHECK(changed > 48 && s.owner_body(100) == b && s.owner_body(115) == b && s.body(b).mem == 16);
    CHECK(s.mem()[95] == before[95] && s.mem()[160] == before[160] && s.tally().strikes == 2);
    CHECK(soup_ok(s));
    printf("  storms: %u formed, %u moves (each to the neighbour its heading picked, ahead), %u strikes where a storm "
           "was, %u ended\n",
           formed, moves, strikes, ended);
}

// X10, on one node: each symptom raises its quantity -- a crash's spurious
// faults (and stray halts), a surge's brownout heat, a burnout's fever on its
// cores alone, a failing disk's rot, a storm's static.
void test_symptom_effects() {
    const char* base = "heat.enabled = true\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\n"
                       "world.millicores = 1000\nworld.disk = 1024\ndisk.rot = 0\n";
    SoupParams p;
    CHECK(make_params(&p, base, 2048));
    const char* loop = "NOP1 INC INC INC INC INC INC INC INC JMP NOP0";
    { // faults: flagged, as IFERR reads; with halts 1, the first halts its thread
        Soup s;
        s.init(&p, 3, 0);
        s.begin_tick(0);
        const u32 b = plant(s, 100, loop, 500);
        Weather wx = {};
        wx.faults = 0.05;
        s.set_weather(wx);
        s.tick(1);
        CHECK(s.tally().spurious_faults > 0 && s.body(b).alive);
        wx.halts = 1.0;
        s.set_weather(wx);
        s.tick(2); // halted outright, it dies in the sweep (no thread left running)
        CHECK(s.tally().spurious_faults == 1 && s.tally().halts == 1 && !s.body(b).alive);
        Soup q; // and none without
        q.init(&p, 3, 0);
        q.begin_tick(0);
        plant(q, 100, loop, 500);
        q.tick(1);
        CHECK(q.tally().spurious_faults == 0);
    }
    { // brownout, fever: hotter units and pages; the fevered units hotter than their neighbours'
        Soup hot, cold;
        hot.init(&p, 5, 0);
        cold.init(&p, 5, 0);
        hot.begin_tick(0);
        cold.begin_tick(0);
        const u32 hb = plant(hot, 100, loop, 400), cb = plant(cold, 100, loop, 400);
        const u32 other = plant(hot, 600, loop, 400);
        Weather wx = {};
        wx.heat = 1.0f;
        hot.set_weather(wx);
        hot.clear_fever();
        for (u32 u = 0; u < p.millicores; ++u) // a fever on every unit hb holds
            if (hot.unit_owner(u) == hb)
                hot.add_fever(u, 2.0f);
        for (u64 t = 1; t <= 60; ++t) {
            hot.tick(t);
            cold.tick(t);
        }
        double h_hot = 0, h_cold = 0, h_other = 0;
        u32 n_hot = 0, n_cold = 0, n_other = 0;
        for (u32 u = 0; u < p.millicores; ++u) {
            if (hot.unit_owner(u) == hb) {
                h_hot += hot.unit_heat(u);
                ++n_hot;
            } else if (hot.unit_owner(u) == other) {
                h_other += hot.unit_heat(u);
                ++n_other;
            }
            if (cold.unit_owner(u) == cb) {
                h_cold += cold.unit_heat(u);
                ++n_cold;
            }
        }
        CHECK(n_hot && n_cold && n_other);
        if (n_hot && n_cold && n_other) { // (hot units throttle, so less than the +10 and +18 a cool one would gain)
            CHECK(h_other / n_other > h_cold / n_cold + 3.0); // the brownout: about +5 on a busy unit (60 -> 65)
            CHECK(h_hot / n_hot > h_other / n_other + 5.0); // the fever: about +8 more (-> 73)
        }
        CHECK(hot.page_heat(1) > cold.page_heat(1) + 5.0f);
    }
    { // a failing disk's rot; a storm's static
        Soup s, q;
        s.init(&p, 9, 0);
        q.init(&p, 9, 0);
        Weather wx = {};
        wx.disk_rot = 0.01;
        wx.static_rot = 0.001;
        s.set_weather(wx);
        u64 rot = 0, flips = 0;
        for (u64 t = 0; t < 20; ++t) {
            s.tick(t);
            q.tick(t);
            rot += s.tally().disk_rot;
            flips += s.tally().static_flips;
            CHECK(q.tally().disk_rot == 0 && q.tally().static_flips == 0);
        }
        CHECK(rot > 100 && flips > 10); // about 205 and 41 expected
    }
}

// X10 in a world: every episode's symptom is on its victims for the tick
// (its region, its node, its cores, its disk) at its strength, nowhere else;
// a real episode ends in its disaster at the end of its lead time, on its
// node; a false alarm fades with none.
void test_symptoms_world() {
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append(kDormancy);
    only(cfg, "surge crash burnout diskfail storm", true);
    cfg.append("disaster.surge_every = 150\ndisaster.crash_every = 40\ndisaster.burnout_every = 40\n"
               "disaster.diskfail_every = 60\ndisaster.storm_every = 150\ndisaster.false_alarms = 0.4\n"
               "disaster.surge_lead_min = 40\ndisaster.surge_lead_max = 120\ndisaster.crash_lead_min = 40\n"
               "disaster.crash_lead_max = 120\ndisaster.burnout_lead_min = 40\ndisaster.burnout_lead_max = 120\n"
               "disaster.diskfail_lead_min = 40\ndisaster.diskfail_lead_max = 120\nnode.fallow_min = 60\n"
               "node.recover_every = 10\ndisaster.storm_move_min = 20\ndisaster.storm_move_max = 40\n");
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    World& w = run.world();
    const DisasterParams& dp = w.disaster_params();
    u32 bad = 0, checked = 0, real = 0, fake = 0, struck = 0, quiet = 0;
    usize seen = 0;
    const u32 N = w.node_count();
    Array<double> heat, faults, rot;
    Array<u8> touched, live;
    Array<Episode> eps; // the episodes, storms and live nodes as the tick began (its weather's source)
    Array<u32> lists;
    Array<Storm> storms;
    for (u64 t = 0; t < 700; ++t) {
        eps.clear();
        lists.clear();
        storms.clear();
        live.clear();
        for (u32 i = 0; i < w.episode_count(); ++i) {
            Episode e = w.episode(i);
            const u32 off = static_cast<u32>(lists.size());
            lists.append(w.episode_list(i), e.n);
            e.off = off;
            eps.push(e);
        }
        for (u32 i = 0; i < w.storm_count(); ++i)
            storms.push(w.storm(i));
        for (u32 n = 0; n < N; ++n)
            live.push(w.live(n) ? 1 : 0);
        run.step();
        const u64 ran = w.tick() - 1; // the weather on the soups is the tick just run's
        heat.resize(N);
        heat.fill(0);
        faults.resize(N);
        faults.fill(0);
        rot.resize(N);
        rot.fill(0);
        touched.resize(N);
        touched.fill(0);
        for (usize i = 0; i < eps.size(); ++i) {
            const Episode& e = eps[i];
            const double lv = w.episode_level(e, ran);
            if (lv <= 0)
                continue;
            const u32* list = lists.data() + e.off;
            if (e.kind == kDisSurge) {
                for (u32 j = 0; j < e.n; ++j)
                    if (live[list[j]]) {
                        heat[list[j]] += static_cast<float>(dp.brownout * lv);
                        touched[list[j]] = 1;
                    }
            } else if (live[e.node]) {
                touched[e.node] = 1;
                ++checked;
                if (e.kind == kDisCrash)
                    faults[e.node] += dp.faults * lv;
                if (e.kind == kDisDiskfail)
                    rot[e.node] += dp.disk_rot * lv;
            }
        }
        for (usize i = 0; i < storms.size(); ++i) {
            touched[storms[i].node] = 1;
            if (storms[i].dest < N)
                touched[storms[i].dest] = 1;
        }
        for (u32 n = 0; n < N; ++n) {
            const Weather& wx = w.node_soup(n).weather();
            const bool near = fabs(wx.heat - heat[n]) < 1e-4 + 1e-5 * heat[n] && fabs(wx.faults - faults[n]) < 1e-12 &&
                              fabs(wx.disk_rot - rot[n]) < 1e-12;
            bad += near ? 0 : 1;
            checked += heat[n] > 0 ? 1 : 0;
            if (!touched[n]) {
                const bool none = wx.heat == 0 && wx.faults == 0 && wx.disk_rot == 0 && wx.static_rot == 0 &&
                                  wx.noise == 0 && w.node_symptoms(n) == 0;
                quiet += none ? 1 : 0;
                bad += none ? 0 : 1;
            }
        }
        // the episodes' ends this barrier: a real one strikes its node now, a false one doesn't
        for (usize i = seen; i < run.event_count(); ++i) {
            const Str e = run.event_text(i);
            if (!e.starts_with("symptom_end "))
                continue;
            const bool f = ev(e, "false") != 0, s = ev(e, "struck") != 0;
            fake += f ? 1 : 0;
            real += f ? 0 : 1;
            struck += s ? 1 : 0;
            bad += f && s ? 1 : 0;
            if (s) { // its disaster's event, on its node, at this barrier
                bool found = false;
                const u32 node = static_cast<u32>(ev(e, "node"));
                for (usize j = seen; j < run.event_count() && !found; ++j) {
                    const Str d = run.event_text(j);
                    found = run.event_tick(j) == run.event_tick(i) && ev_node(d) == node &&
                            (d.starts_with("surge ") || d.starts_with("crash ") || d.starts_with("burnout ") ||
                             d.starts_with("diskfail "));
                }
                bad += found ? 0 : 1;
            }
        }
        seen = run.event_count();
    }
    CHECK(bad == 0 && checked > 0 && quiet > 0 && real > 0 && fake > 0 && struck > 0);
    CHECK(all_ok(w));
    printf("  symptoms: %u episodes ended (%u real, %u struck; %u false alarms, none struck); weather right on every "
           "node every tick (%u node-ticks with symptoms)\n",
           real + fake, real, struck, fake, checked);
}

// The shape of an episode's symptoms over time: rising as the square of the
// time since it began, reaching 1 at its strike; a false alarm peaking
// lower, then fading to nothing.
void test_episode_level() {
    Run run;
    CHECK(make_run(run, "world.model = soup\nworld.nodes = 1\nworld.soup_size = 1024\ndisaster.enabled = true\n", 1));
    const World& w = run.world();
    Episode e = {};
    e.start = 100;
    e.lead = 100;
    e.peak = e.end = 200;
    CHECK(w.episode_level(e, 99) == 0 && w.episode_level(e, 100) == 0 && w.episode_level(e, 150) == 0.25 &&
          w.episode_level(e, 200) == 1 && w.episode_level(e, 201) == 0);
    e.false_alarm = 1;
    e.peak = 150;
    e.end = 175;
    CHECK(w.episode_level(e, 150) == 0.25 && fabs(w.episode_level(e, 160) - 0.15) < 1e-12 &&
          w.episode_level(e, 175) == 0 && w.episode_level(e, 176) == 0);
}

// X11: thunder arrives on the storm's channels at the source at full
// strength the tick after the strike, and halved (attenuation 0.5) a tick
// later one hop away; a world without signals is silent.
void test_sounds() {
    const char* base = "seed = 7\nworld.model = soup\nworld.nodes = 12\nworld.chords = 0\nworld.soup_size = 1024\n"
                       "spark.initial = 0\nspark.rate = 0\n";
    TextBuf cfg;
    cfg.append(base);
    cfg.append("signal.channels = 16\nsignal.floor = 0\nsignal.attenuation = 0.5\nsignal.reach = 3\n");
    only(cfg, "storm");
    cfg.append("disaster.storm_every = 40\ndisaster.storm_life_min = 100000\ndisaster.storm_life_max = 100000\n"
               "disaster.strike_every_min = 30\ndisaster.strike_every_max = 30\ndisaster.rumble = 0\n"
               "disaster.thunder = 1000\n");
    Run run;
    CHECK(make_run(run, cfg.c_str(), 2));
    World& w = run.world();
    const u32 N = w.node_count(), band = w.disaster_params().band[kDisStorm];
    const u64 ticks = 400;
    Array<i32> heard; // [tick][node]: the field on the storm's first channel
    heard.resize(static_cast<usize>(ticks + 2) * N);
    Array<u64> strike_tick;
    Array<u32> strike_node;
    usize seen = 0;
    u64 sounds = 0;
    for (u64 t = 0; t < ticks; ++t) {
        run.step();
        sounds += w.last_soup_tick().sounds;
        for (u32 n = 0; n < N; ++n)
            heard[static_cast<usize>(t) * N + n] = w.node_soup(n).field()[band];
        for (; seen < run.event_count(); ++seen)
            if (run.event_text(seen).starts_with("lightning ")) {
                strike_tick.push(run.event_tick(seen));
                strike_node.push(ev_node(run.event_text(seen)));
            }
    }
    u32 checked = 0, bad = 0;
    for (usize i = 0; i < strike_tick.size(); ++i) {
        const u64 T = strike_tick[i];
        bool alone = T + 2 < ticks && T >= 4;
        for (usize j = 0; j < strike_tick.size() && alone; ++j)
            alone = j == i || strike_tick[j] + 4 < T || strike_tick[j] > T + 2;
        if (!alone)
            continue;
        const u32 n = strike_node[i], a = (n + 1) % N, b = (n + N - 1) % N;
        auto at = [&](u64 t, u32 m) { return heard[static_cast<usize>(t) * N + m]; };
        const bool ok = at(T + 1, n) == 1000 && at(T + 1, a) == 0 && at(T + 1, b) == 0 && at(T + 2, a) == 500 &&
                        at(T + 2, b) == 500 && at(T + 2, n) == 0;
        bad += ok ? 0 : 1;
        ++checked;
    }
    CHECK(checked > 0 && bad == 0 && sounds > 0);
    // Without signals: the same storms strike, in silence.
    TextBuf quiet;
    quiet.append(base);
    quiet.append("signal.channels = 0\n");
    only(quiet, "storm");
    quiet.append("disaster.storm_every = 40\ndisaster.strike_every_min = 30\ndisaster.strike_every_max = 30\n");
    Run q;
    CHECK(make_run(q, quiet.c_str(), 2));
    u64 q_sounds = 0, q_strikes = 0;
    for (u64 t = 0; t < 200; ++t) {
        q.step();
        q_sounds += q.world().last_soup_tick().sounds;
        q_strikes += q.world().last_soup_tick().strikes;
    }
    CHECK(q_sounds == 0 && q_strikes > 0);
    printf("  sounds: thunder at full strength at the source, halved a hop away a tick later (%u strikes checked); "
           "silent without signals (%llu strikes)\n",
           checked, static_cast<unsigned long long>(q_strikes));
}

// Every disaster frequent, with symptoms, sounds, dormancy, the disk and
// signals: the same world at 1, 4 and 20 threads, under shuffled dispatch,
// and through a snapshot taken during a reboot with cores offline.
const char* const kEveryDisaster =
    "disaster.enabled = true\nsignal.channels = 16\nworld.disk = 1024\nlife.frozen_ticks = 40\nlife.dormancy = true\n"
    "mem.upkeep = 6\ndisaster.surge_every = 100\ndisaster.crash_every = 40\ndisaster.burnout_every = 30\n"
    "disaster.diskfail_every = 80\ndisaster.storm_every = 100\ndisaster.false_alarms = 0.3\n"
    "disaster.surge_lead_min = 20\ndisaster.surge_lead_max = 60\ndisaster.crash_lead_min = 20\n"
    "disaster.crash_lead_max = 60\ndisaster.burnout_lead_min = 20\ndisaster.burnout_lead_max = 60\n"
    "disaster.diskfail_lead_min = 20\ndisaster.diskfail_lead_max = 60\ndisaster.reboot_min = 20\n"
    "disaster.reboot_max = 50\ndisaster.repair_min = 60\ndisaster.repair_max = 200\ndisaster.storm_life_min = 100\n"
    "disaster.storm_life_max = 300\ndisaster.storm_move_min = 20\ndisaster.storm_move_max = 60\n"
    "disaster.strike_every_min = 10\ndisaster.strike_every_max = 30\nnode.fallow_min = 60\nnode.recover_every = 10\n";

void test_determinism() {
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append(kEveryDisaster);
    const u64 ticks = 500;
    u64 hashes[4] = {};
    const u32 threads[4] = {1, 4, 20, 4};
    Array<u32> order;
    SoupTally sum = {};
    ByteWriter snap;
    u64 snap_tick = 0, snap_down = 0, snap_off = 0, most_off = 0;
    for (u32 v = 0; v < 4; ++v) {
        Run run;
        CHECK(make_run(run, cfg.c_str(), threads[v]));
        if (v == 3) { // a shuffled dispatch order
            const u32 n = run.world().node_count();
            order.resize(n);
            for (u32 i = 0; i < n; ++i)
                order[i] = i;
            Rng r(99, 0, 0);
            for (u32 i = n - 1; i > 0; --i) {
                const u32 j = static_cast<u32>(r.below(i + 1));
                const u32 x = order[i];
                order[i] = order[j];
                order[j] = x;
            }
            run.set_dispatch_order(order.data());
        }
        for (u64 t = 1; t <= ticks; ++t) {
            run.run_until(t);
            if (v != 0)
                continue;
            const World& w = run.world();
            const u64* s = reinterpret_cast<const u64*>(&w.last_soup_tick());
            u64* d = reinterpret_cast<u64*>(&sum);
            for (usize i = 0; i < sizeof(SoupTally) / sizeof(u64); ++i)
                d[i] += s[i];
            most_off = w.cores_offline() > most_off ? w.cores_offline() : most_off;
            if (snap.size() == 0 && t > 50 && w.rebooting_count() > 0 && w.cores_offline() > 0) {
                run.snapshot(snap); // during a reboot, with cores offline
                snap_tick = t;
                snap_down = w.rebooting_count();
                snap_off = w.cores_offline();
            }
        }
        hashes[v] = run.world().hash();
        if (v == 0)
            CHECK(all_ok(run.world()));
    }
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK_EQ_U64(hashes[3], hashes[0]);
    CHECK(sum.surges > 0 && sum.crashes > 0 && sum.burnouts > 0 && sum.disk_failures > 0 && sum.strikes > 0);
    CHECK(sum.episodes > 0 && sum.false_alarms > 0 && sum.spurious_faults > 0 && sum.static_flips > 0 &&
          sum.sounds > 0);
    CHECK(snap.size() > 0);
    if (snap.size() > 0) {
        Run b;
        TextBuf err;
        CHECK(b.init_from_snapshot(snap.data(), snap.size(), 4, &err));
        CHECK(b.world().rebooting_count() == snap_down && b.world().cores_offline() == snap_off);
        b.run_until(ticks);
        CHECK_EQ_U64(b.world().hash(), hashes[0]);
    }
    printf("  every disaster, %llu ticks on %u nodes: %llu surges (%llu nodes, %llu bodies), %llu crashes (%llu "
           "deleted, %llu restarted), %llu burnouts (%llu cores, %llu dormant, %llu graced, %llu died; at most %llu "
           "cores offline), %llu disk failures (%llu codons), %llu storms, %llu strikes (%llu codons); %llu episodes "
           "(%llu false), %llu spurious faults, %llu static flips, %llu sounds; snapshot at tick %llu with %llu "
           "rebooting and %llu cores offline resumed exactly; hash %016llx\n",
           static_cast<unsigned long long>(ticks), 24u, static_cast<unsigned long long>(sum.surges),
           static_cast<unsigned long long>(sum.surge_nodes), static_cast<unsigned long long>(sum.surge_deaths),
           static_cast<unsigned long long>(sum.crashes), static_cast<unsigned long long>(sum.crash_deaths),
           static_cast<unsigned long long>(sum.crash_restarts), static_cast<unsigned long long>(sum.burnouts),
           static_cast<unsigned long long>(sum.cores_burnt), static_cast<unsigned long long>(sum.burnout_dormant),
           static_cast<unsigned long long>(sum.burnout_graced), static_cast<unsigned long long>(sum.burnout_deaths),
           static_cast<unsigned long long>(most_off), static_cast<unsigned long long>(sum.disk_failures),
           static_cast<unsigned long long>(sum.disk_lost), static_cast<unsigned long long>(sum.storms_formed),
           static_cast<unsigned long long>(sum.strikes), static_cast<unsigned long long>(sum.strike_codons),
           static_cast<unsigned long long>(sum.episodes), static_cast<unsigned long long>(sum.false_alarms),
           static_cast<unsigned long long>(sum.spurious_faults), static_cast<unsigned long long>(sum.static_flips),
           static_cast<unsigned long long>(sum.sounds), static_cast<unsigned long long>(snap_tick),
           static_cast<unsigned long long>(snap_down), static_cast<unsigned long long>(snap_off),
           static_cast<unsigned long long>(hashes[0]));
}

// The same on the engine's own defaults (no legacy fallback): warps (on the
// CPU reference), async accelerators, threads and E cores, the GPU and NPU,
// vents and decay -- so crashes restart GPU-born programs and burnouts take
// GPU units and E millicores -- at 1 and 4 threads and through a snapshot.
void test_defaults_world() {
    CHECK(Config::set_fallback(nullptr, nullptr));
    TextBuf cfg;
    cfg.append("world.model = soup\nworld.graph = procedural\nworld.nodes = 12\nworld.soup_size = 4096\n"
               "spark.initial = 40\nspark.rate = 0.25\naccel.gpu = cpu\naccel.npu = cpu\nwarp.device = cpu\n");
    cfg.append(kEveryDisaster);
    const u64 ticks = 160, snap_at = 80;
    u64 h[2] = {};
    const u32 threads[2] = {1, 4};
    SoupTally sum = {};
    ByteWriter snap;
    for (u32 v = 0; v < 2; ++v) {
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
            if (t == snap_at)
                run.snapshot(snap);
        }
        h[v] = run.world().hash();
        if (v == 0)
            CHECK(all_ok(run.world()));
    }
    CHECK_EQ_U64(h[1], h[0]);
    CHECK(sum.crashes > 0 && sum.burnouts > 0 && sum.strikes > 0 && sum.episodes > 0);
    Run b;
    TextBuf err;
    CHECK(b.init_from_snapshot(snap.data(), snap.size(), 4, &err));
    b.run_until(ticks);
    CHECK_EQ_U64(b.world().hash(), h[0]);
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
    printf("  every disaster on every default, %llu ticks: %llu crashes, %llu burnouts (%llu cores), %llu strikes, "
           "%llu surges, %llu disk failures, %llu episodes; the same at 1 and 4 threads and through a snapshot at "
           "tick %llu; hash %016llx\n",
           static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(sum.crashes),
           static_cast<unsigned long long>(sum.burnouts), static_cast<unsigned long long>(sum.cores_burnt),
           static_cast<unsigned long long>(sum.strikes), static_cast<unsigned long long>(sum.surges),
           static_cast<unsigned long long>(sum.disk_failures), static_cast<unsigned long long>(sum.episodes),
           static_cast<unsigned long long>(snap_at), static_cast<unsigned long long>(h[0]));
}

// The engine defaults: every disaster on, symptoms and sounds too; the test
// runner's legacy defaults: off. Storms, crashes and surges at a fifth of
// the rates docs/bites-plan.md section 7 first set (Damian, 2026-11-09).
void test_defaults() {
    SoupParams p;
    CHECK(make_params(&p, "", 1024) && !p.disasters); // (the legacy fallback)
    CHECK(Config::set_fallback(nullptr, nullptr));
    Run run;
    CHECK(make_run(run, "world.model = soup\nworld.graph = procedural\nworld.nodes = 9\nworld.soup_size = 2048\n"
                        "spark.initial = 4\naccel.gpu = cpu\naccel.npu = cpu\nwarp.device = cpu\n",
                   1));
    const DisasterParams& d = run.world().disaster_params();
    CHECK(run.world().disasters() && d.symptoms && d.sounds && d.false_alarms == 0.25);
    CHECK(d.every[kDisSurge] == 275 && d.every[kDisCrash] == 15 && d.every[kDisBurnout] == 2000 &&
          d.every[kDisDiskfail] == 10000 && d.every[kDisStorm] == 1500);
    CHECK(d.hops_min == 1 && d.hops_max == 2 && d.reboot_min == 20 && d.reboot_max == 100 && d.crash_min == 0.3 &&
          d.crash_max == 0.8 && d.burn_min == 0.1 && d.burn_max == 0.4 && d.repair_min == 200 &&
          d.repair_max == 2000 && d.grace_chance == 0.5 && d.grace == 32);
    CHECK(d.life_min == 2000 && d.life_max == 10000 && d.move_min == 200 && d.move_max == 800 &&
          d.strike_every_min == 30 && d.strike_every_max == 120 && d.strike_min == 512 && d.strike_max == 2048);
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
}

// --- with the other merged builds (docs/results/integration.md) ---------------------------

// Whether every codon of one tier of s's disk is INERT and no one holds any of it.
bool tier_gone(const Soup& s, bool archive) {
    const DiskLedger& d = s.disk();
    const u32 lo = archive ? d.fast : 0, hi = archive ? d.size() : d.fast;
    bool ok = true;
    for (u32 a = lo; a < hi; ++a)
        ok = ok && d.mem[a] == 0 && d.owner[a] == kNone;
    for (usize k = 0; k < s.body_count(); ++k) {
        const u32 b = s.body_at(k);
        ok = ok && (!d.tiered() || d.held_in(b, archive) == 0);
        if (s.dormant(b))
            ok = ok && (archive ? s.dormant_state(b).archive_reserve : s.dormant_state(b).disk_reserve) == 0;
    }
    return ok;
}

// X5 with the disk tiers (docs/world-plan.md W4): a failure takes one tier,
// chosen by size -- the archive with chance archive / (disk + archive), from
// the disaster's own stream -- and leaves the other as it was.
void test_diskfail_tiers() {
    // The draw: 4,000 failures of one soup, at two splits of fast disk and archive.
    const u32 splits[2][2] = {{768, 256}, {256, 256}};
    for (const auto& sp : splits) {
        SoupParams p;
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.disk = ");
        cfg.append_u64(sp[0]);
        cfg.append("\nworld.archive = ");
        cfg.append_u64(sp[1]);
        cfg.append_char('\n');
        CHECK(make_params(&p, cfg.c_str(), 1024));
        Soup s;
        s.init(&p, 5, 0);
        const u32 n = 4000;
        u32 arch = 0;
        for (u32 i = 0; i < n; ++i) {
            Rng r(31, Rng::kWorldNode, i);
            bool a = false;
            s.fail_disk(r, &a);
            arch += a ? 1 : 0;
        }
        const double share = static_cast<double>(sp[1]) / (sp[0] + sp[1]);
        const double want = n * share, sd = sqrt(n * share * (1 - share));
        CHECK(arch > 0 && arch < n && fabs(arch - want) < 4 * sd);
        printf("  disk failure by tier: %u of %u on the archive (%u fast, %u archive codons: %.0f expected)\n", arch, n,
               sp[0], sp[1], want);
    }
    // One tier only: what was held and stored on the other stays.
    {
        SoupParams p;
        CHECK(make_params(&p, "heat.enabled = false\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\n"
                              "world.millicores = 1000\nworld.disk = 256\nworld.archive = 256\n",
                          1024));
        bool seen[2] = {false, false}, kept = true, gone = true;
        for (u32 i = 0; i < 40 && !(seen[0] && seen[1]); ++i) {
            Soup s;
            s.init(&p, 5, 0);
            const u32 b = plant(s, 100, "INC INC INC INC", 100);
            CHECK(b != kNone);
            Rng cr(9, 0, 0);
            s.disk_mut().alloc_run(cr, b, 32, false);
            s.disk_mut().alloc_run(cr, b, 32, true);
            for (u32 a = 0; a < s.disk().size(); ++a)
                s.disk_mut().mem[a] = static_cast<u8>(1 + a % 60);
            Rng r(32, Rng::kWorldNode, i);
            bool arch = false;
            const u32 held = s.fail_disk(r, &arch);
            seen[arch ? 1 : 0] = true;
            gone = gone && held == 32 && tier_gone(s, arch);
            const u32 lo = arch ? 0 : s.disk().fast, hi = arch ? s.disk().fast : s.disk().size();
            for (u32 a = lo; a < hi; ++a)
                kept = kept && s.disk().mem[a] == 1 + a % 60;
            kept = kept && s.disk().held_in(b, !arch) == 32 && soup_ok(s);
        }
        CHECK(seen[0] && seen[1] && gone && kept);
    }
    // In a world: each failure's tier INERT and unheld after, and both tiers hit.
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append(kDormancy); // (a disk of 1,024)
    only(cfg, "diskfail");
    cfg.append("disaster.diskfail_every = 8\nworld.archive = 256\n");
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    World& w = run.world();
    u32 fails[2] = {}, clean = 0;
    usize seen = 0;
    for (u64 t = 0; t < 400; ++t) {
        run.step();
        for (; seen < run.event_count(); ++seen) {
            const Str e = run.event_text(seen);
            if (!e.starts_with("diskfail "))
                continue;
            const bool arch = e.n >= 13 && memcmp(e.p + e.n - 13, " tier=archive", 13) == 0; // (its last field)
            ++fails[arch ? 1 : 0];
            clean += tier_gone(w.node_soup(ev_node(e)), arch) ? 1 : 0;
        }
    }
    CHECK(fails[0] > 0 && fails[1] > 0 && clean == fails[0] + fails[1]);
    CHECK(all_ok(w));
    printf("  disk failures by tier in a world (1,024 fast, 256 archive): %u fast, %u archive, each tier INERT and "
           "unheld after\n",
           fails[0], fails[1]);
}

// A run.cfg with the disasters off may still name every disaster setting (the
// setup page writes them all, docs/config-plan.md C3): nothing reads them, and
// key_accept_known (nova run, nova view) takes them as known settings, while a
// key with no entry still fails as a typo.
void test_unread_keys() {
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append("disaster.enabled = false\n");
    u32 named = 0;
    for (usize i = 0; i < key_count(); ++i) {
        const Key& k = key_at(i);
        if (strncmp(k.name, "disaster.", 9) != 0 || strcmp(k.name, "disaster.enabled") == 0)
            continue;
        cfg.append(k.name);
        cfg.append(" = ");
        cfg.append(k.def_text);
        cfg.append_char('\n');
        ++named;
    }
    Run run;
    CHECK(make_run(run, cfg.c_str(), 1));
    CHECK(!run.world().disasters() && named >= 60);
    Str unused;
    CHECK(run.config().first_unused(&unused) && unused.starts_with("disaster.")); // nothing read them
    key_accept_known(run.config());
    CHECK(!run.config().first_unused(&unused));
    Config typo;
    Config::ParseError pe;
    CHECK(typo.parse("disaster.surge_evry = 5\n", &pe));
    key_accept_known(typo);
    CHECK(typo.first_unused(&unused) && unused.eq("disaster.surge_evry"));
}

// Every feature of this build at once: a busy landscape with substrates, the
// archive, sparks by activity, every disaster frequent (symptoms and sounds),
// dormancy, dispersal and the lifecycle, and the detector linking by descent.
// The same hash and the same events (the detector's among them) at 1, 4 and
// 20 threads and under shuffled dispatch, and through a snapshot taken during
// a reboot, resumed on 20 threads.
const char* const kEverythingLand =
    "seed = 7\nworld.model = soup\nworld.graph = landscape\nworld.nodes = 48\nland.scale = 4\nland.resolution = 96\n"
    "paths.hubs = 0.05\nbridge.count = 2\nbridge.max_length = 0.6\nworld.soup_size = 2048\nworld.millicores = 600\n"
    "world.disk = 512\nworld.archive = 256\nres.disk.spread = 0.6\nres.archive.spread = 0.8\nres.archive.smooth = 0.5\n"
    "res.archive.barren = 0.2\ndisk.rot = 0.0001\nsalvage.ratio = 4\nedge.bandwidth = 8\n"
    "node.endurance = 4000\nnode.fallow_min = 60\nnode.recover_every = 10\nlife.frozen_ticks = 40\n"
    "life.dormancy = true\nmem.upkeep = 6\ncpu.instr_per_core = 2500\ncpu.wear = 0.002\nspark.timing = last\n"
    "spark.initial = 20\nspark.rate = 0.5\nspark.grant = 20\nspark.min_len = 8\nspark.max_len = 24\n"
    "seed.genome = ZERO ROTB SENSE ALLOC NOP1 COPY DJNZ NOP0 ZERO INC ROTB SENSE DIVIDE\nseed.at = 600\n"
    "seed.millicores = 200\nhazard.copy = 0.005\nhazard.flaw = 0.001\nhazard.rot = 0.00005\n"
    // the varied world (off in the test runner's legacy defaults)
    "substrate.enabled = true\nspark.activity = 2\nmem.fast_share = 0.125\n"
    "profile.enabled = true\nprofile.every = 25\nprofile.ticks = 20\nnovelty.enabled = true\n"
    // the detector, linking by descent (X12)
    "detect.enabled = true\ndetect.shift = 8\n"
    // every disaster, frequent, with symptoms and sounds
    "disaster.enabled = true\nsignal.channels = 16\ndisaster.surge_every = 150\ndisaster.crash_every = 40\n"
    "disaster.burnout_every = 30\ndisaster.diskfail_every = 40\ndisaster.storm_every = 100\n"
    "disaster.false_alarms = 0.3\ndisaster.surge_lead_min = 20\ndisaster.surge_lead_max = 60\n"
    "disaster.crash_lead_min = 20\ndisaster.crash_lead_max = 60\ndisaster.burnout_lead_min = 20\n"
    "disaster.burnout_lead_max = 60\ndisaster.diskfail_lead_min = 20\ndisaster.diskfail_lead_max = 60\n"
    "disaster.reboot_min = 20\ndisaster.reboot_max = 50\ndisaster.repair_min = 60\ndisaster.repair_max = 200\n"
    "disaster.storm_life_min = 100\ndisaster.storm_life_max = 300\ndisaster.storm_move_min = 20\n"
    "disaster.storm_move_max = 60\ndisaster.strike_every_min = 10\ndisaster.strike_every_max = 30\n";

void test_everything_merged() {
    const u64 ticks = 300;
    u64 hashes[4] = {};
    const u32 threads[4] = {1, 4, 20, 4};
    Array<u32> order;
    SoupTally sum = {};
    ByteWriter snap;
    u64 snap_tick = 0, snap_down = 0, snap_off = 0, founded = 0, links = 0, held_max = 0, arch_fails = 0;
    u32 subs = 0;
    Array<u64> ev_tick[4];
    Array<char> ev_text[4];
    for (u32 v = 0; v < 4; ++v) {
        Run run;
        CHECK(make_run(run, kEverythingLand, threads[v]));
        const World& w = run.world();
        if (v == 0) {
            CHECK(w.substrates() && w.disasters() && w.lifecycle() && w.node_params(0).spark_activity == 2.0);
            CHECK(w.node_soup(0).fast_end() > 0 && w.node_profiles(0) && run.novelty());
            Array<u64> keys;
            for (u32 n = 0; n < w.node_count(); ++n) {
                u64 key = 0;
                for (u32 t = 0; t < kSubstrateTypes; ++t)
                    key = key << 8 | w.substrate_level(n, t);
                keys.push(key);
            }
            sort_u64(keys.data(), keys.size());
            for (usize i = 0; i < keys.size(); ++i)
                subs += i == 0 || keys[i] != keys[i - 1];
        }
        if (v == 3) {
            const u32 n = w.node_count();
            order.resize(n);
            for (u32 i = 0; i < n; ++i)
                order[i] = i;
            Rng r(99, 0, 0);
            for (u32 i = n - 1; i > 0; --i) {
                const u32 j = static_cast<u32>(r.below(i + 1));
                const u32 x = order[i];
                order[i] = order[j];
                order[j] = x;
            }
            run.set_dispatch_order(order.data());
        }
        for (u64 t = 1; t <= ticks; ++t) {
            run.run_until(t);
            if (v != 0)
                continue;
            const u64* s = reinterpret_cast<const u64*>(&w.last_soup_tick());
            u64* d = reinterpret_cast<u64*>(&sum);
            for (usize i = 0; i < sizeof(SoupTally) / sizeof(u64); ++i)
                d[i] += s[i];
            u64 held = 0; // the archive held (holdings; the DISK ops on it are rare with nearest selectors)
            for (u32 n = 0; n < w.node_count(); ++n)
                held += w.node_params(n).archive - w.node_soup(n).free_archive();
            held_max = held > held_max ? held : held_max;
            if (snap.size() == 0 && t > 60 && w.rebooting_count() > 0) {
                run.snapshot(snap); // during a reboot
                snap_tick = t;
                snap_down = w.rebooting_count();
                snap_off = w.cores_offline();
            }
        }
        hashes[v] = w.hash();
        for (usize i = 0; i < run.event_count(); ++i) { // every event, the detector's and the disasters'
            ev_tick[v].push(run.event_tick(i));
            const Str e = run.event_text(i);
            ev_text[v].append(e.p, e.n);
            ev_text[v].push('\n');
            if (v == 0 && e.starts_with("diskfail ") && e.n >= 13 && memcmp(e.p + e.n - 13, " tier=archive", 13) == 0)
                ++arch_fails;
        }
        if (v == 0) {
            CHECK(all_ok(w));
            for (u32 n = 0; n < w.node_count(); ++n)
                if (const Lineages* L = w.node_lineages(n)) {
                    founded += L->founded();
                    links += L->links();
                }
        }
    }
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK_EQ_U64(hashes[3], hashes[0]);
    {
        Run unobserved;
        Config::ParseError pe;
        TextBuf err;
        CHECK(unobserved.config().parse(kEverythingLand, &pe));
        unobserved.config().set("profile.enabled", "false");
        unobserved.config().set("novelty.enabled", "false");
        CHECK(unobserved.init(4, &err));
        unobserved.run_until(ticks);
        CHECK_EQ_U64(unobserved.world().hash(), hashes[0]);
    }
    bool same_events = true;
    for (u32 v = 1; v < 4; ++v)
        same_events = same_events && ev_tick[v].size() == ev_tick[0].size() &&
                      ev_text[v].size() == ev_text[0].size() &&
                      memcmp(ev_tick[v].data(), ev_tick[0].data(), ev_tick[0].size() * sizeof(u64)) == 0 &&
                      memcmp(ev_text[v].data(), ev_text[0].data(), ev_text[0].size()) == 0;
    CHECK(same_events && ev_tick[0].size() > 0);
    CHECK(sum.crashes > 0 && sum.burnouts > 0 && sum.disk_failures > 0 && sum.strikes > 0 && sum.episodes > 0);
    CHECK(held_max > 0 && arch_fails > 0 && founded > 0 && links > 0 && subs > 1);
    CHECK(snap.size() > 0);
    if (snap.size() > 0) {
        Run b;
        TextBuf err;
        CHECK(b.init_from_snapshot(snap.data(), snap.size(), 20, &err));
        CHECK(b.world().rebooting_count() == snap_down && b.world().cores_offline() == snap_off);
        b.run_until(ticks);
        CHECK_EQ_U64(b.world().hash(), hashes[0]);
    }
    printf("  everything merged, %llu ticks on 48 nodes: %u distinct substrates; archive held up to %llu codons; %llu "
           "surges, %llu crashes, %llu burnouts, %llu disk failures (%llu of the archive), %llu strikes, %llu episodes; "
           "%llu lineages founded, %llu links; %llu events the same at 1, 4 and 20 threads and shuffled; snapshot at "
           "tick %llu with %llu rebooting resumed exactly on 20 threads; hash %016llx\n",
           static_cast<unsigned long long>(ticks), subs, static_cast<unsigned long long>(held_max),
           static_cast<unsigned long long>(sum.surges),
           static_cast<unsigned long long>(sum.crashes), static_cast<unsigned long long>(sum.burnouts),
           static_cast<unsigned long long>(sum.disk_failures), static_cast<unsigned long long>(arch_fails),
           static_cast<unsigned long long>(sum.strikes),
           static_cast<unsigned long long>(sum.episodes), static_cast<unsigned long long>(founded),
           static_cast<unsigned long long>(links), static_cast<unsigned long long>(ev_tick[0].size()),
           static_cast<unsigned long long>(snap_tick), static_cast<unsigned long long>(snap_down),
           static_cast<unsigned long long>(hashes[0]));
}

} // namespace

void suite_disasters() {
    test_episode_level();
    test_defaults();
    test_surge();
    test_crash();
    test_burnout();
    test_diskfail();
    test_storms();
    test_symptom_effects();
    test_symptoms_world();
    test_sounds();
    test_determinism();
    test_defaults_world();
    test_diskfail_tiers();
    test_unread_keys();
    test_everything_merged();
}
