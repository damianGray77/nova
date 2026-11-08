// Disasters (docs/disasters-plan.md X1-X11): rare, random disturbances --
// power surges, OS crashes, burnouts, disk failures and lightning storms --
// each warned of by symptoms (X10) and each with a voice in the signal field
// (X11).
//
// The world's side: the schedule (X1), each kind's strike (X2-X6), the symptom
// episodes and the storms, the sounds, the events (X7) and the disaster
// state's hash and snapshot section (X9). The soup's side: what each disaster
// does to one node, and what its symptoms do during the node's tick.
//
// Determinism. Every draw comes from a keyed world stream: the schedule's by
// kind and tick, an episode's strike by its serial, a storm's by its serial
// (continued from its saved counter), a sound's by its source and tick.
// Everything is applied at the barrier, serially, in a fixed order: repairs,
// then each kind in turn (surge, crash, burnout, disk failure: due episodes
// strike or fade, then new ones begin), then the storms, then the sounds; a
// surge's collapses follow through the lifecycle. Each node's symptoms are set
// before its tick, serially, from the episodes and storms alone.
#include <cmath>  // atan2, cos, exp, floor, log, sqrt
#include <cstdio> // snprintf

#include "nova/keys.h"
#include "nova/lineage.h"
#include "nova/world.h"

namespace nova {

namespace {

constexpr u64 kSchedSalt = 0x6469736173746572ull;  // "disaster": the schedule, by kind and tick
constexpr u64 kStrikeSalt = 0x737472696B650000ull; // "strike": an episode's strike, by serial
constexpr u64 kStormSalt = 0x73746F726D000000ull;  // "storm": a storm's own stream, by serial
constexpr u64 kSoundSalt = 0x736F756E64000000ull;  // "sound": a sound, by its source and tick
constexpr double kTau = 6.283185307179586;

// Each kind's band of channels, from its first (disaster.<kind>_band): a
// surge's hum and a storm's thunder are broad, a disk's grinding narrow.
constexpr u32 kBandWidth[kDisKinds] = {3, 2, 2, 1, 3};

u32 draw_u32(Rng& r, u32 lo, u32 hi) { return lo + static_cast<u32>(r.below(static_cast<u64>(hi) - lo + 1)); }
double draw_f64(Rng& r, double lo, double hi) { return lo + (hi - lo) * r.unit(); }

// A Poisson count with mean lambda (small: a disaster a barrier is rare).
u32 poisson(Rng& r, double lambda) {
    if (lambda <= 0)
        return 0;
    double p = exp(-lambda), s = p;
    const double u = r.unit();
    u32 k = 0;
    while (u > s && k < 64) {
        ++k;
        p *= lambda / k;
        s += p;
    }
    return k;
}

void kv_u64(TextBuf& t, const char* key, u64 v) {
    t.append_char(' ');
    t.append(key);
    t.append_char('=');
    t.append_u64(v);
}

} // namespace

const char* disaster_kind_name(u32 kind) {
    static const char* const n[kDisKinds] = {"surge", "crash", "burnout", "diskfail", "storm"};
    return kind < kDisKinds ? n[kind] : "?";
}

// ===========================================================================
// The soup's side: one node
// ===========================================================================

void Soup::order_compact() {
    usize w = 0;
    for (usize k = 0; k < order_.size(); ++k) {
        const u32 b = order_[k];
        if (bodies_[b].alive)
            order_[w++] = b;
        else
            free_bodies_.push(b);
    }
    order_.resize(w);
}

// X3. The running programs (resting dormant ones aren't), a random share of
// them picked without replacement; they die ordinary deaths (remnants stay,
// scavengeable). The rest restart: each thread back at its body's start with
// every register cleared, warps ended (a GPU-born program, whose warp is its
// thread, gets a fresh one at its start), every action in progress dropped,
// async results still due to its threads ignored. They count as running at
// `restart`, the reboot's first tick, so the watchdog doesn't take the reboot
// for a freeze.
DisasterCount Soup::crash(double share, u64 restart, Rng& r) {
    DisasterCount out = {};
    Array<u32>& run = scratch_;
    run.clear();
    for (usize k = 0; k < order_.size(); ++k)
        if (bodies_[order_[k]].alive && !dormant(order_[k]))
            run.push(order_[k]);
    const u32 n = static_cast<u32>(run.size());
    u32 doomed = static_cast<u32>(floor(share * n + 0.5));
    doomed = doomed > n ? n : doomed;
    for (u32 i = 0; i < doomed; ++i) {
        const u32 j = i + static_cast<u32>(r.below(n - i));
        const u32 t = run[i];
        run[i] = run[j];
        run[j] = t;
    }
    for (u32 i = 0; i < doomed; ++i)
        kill(run[i]);
    for (u32 i = doomed; i < n; ++i) {
        const u32 b = run[i];
        Body& B = bodies_[b];
        if (B.blocks == kNone)
            continue;
        const u32 start = blocks_[B.blocks].start;
        const u32 o = owner_[start];
        const u32 wrap = o != kNone && blocks_[o].body == b ? o : kNone;
        for (u32 c = B.cpus; c != kNone; c = cpus_[c].next) {
            Cpu& C = cpus_[c];
            const u32 next = C.next;
            const u64 executed = C.executed;
            C = Cpu{};
            C.ip = C.rh = C.wh = start;
            C.body = b;
            C.block = wrap;
            C.next = next;
            C.executed = executed;
            if (p_->accel_async && c < cpu_gen_.size()) { // results still due aren't the new thread's
                ++cpu_gen_[c];
                cpu_result_[c] = 0;
                cpu_pending_[c] = 0;
            }
        }
        if (p_->warps && warp_count_[b] > 0) {
            kill_warps(b);
            if (B.form == kFormGpu && gpu_count(b) > 0)
                new_warp(b, start, nullptr);
        }
        if (p_->frozen_ticks > 0)
            last_ran_[b] = restart;
        ++out.b;
    }
    tally_.actions_failed += actions_.size(); // every action in progress is dropped
    actions_.clear();
    out.a = doomed;
    order_compact();
    ++tally_.crashes;
    tally_.crash_deaths += out.a;
    tally_.crash_restarts += out.b;
    return out;
}

// X4. Running programs at random, without replacement, until those picked
// hold `share` of the node's cores (millicores and GPU units together).
void Soup::pick_burnout(double share, Rng& r, Array<u32>* units) const {
    Array<u32> run;
    for (usize k = 0; k < order_.size(); ++k)
        if (bodies_[order_[k]].alive && !dormant(order_[k]))
            run.push(order_[k]);
    const double target = share * (static_cast<double>(p_->millicores) + p_->gpus);
    const u32 n = static_cast<u32>(run.size());
    double taken = 0;
    for (u32 i = 0; i < n && taken < target; ++i) {
        const u32 j = i + static_cast<u32>(r.below(n - i));
        const u32 t = run[i];
        run[i] = run[j];
        run[j] = t;
        const u32 b = run[i];
        for (u32 u = bodies_[b].unit_head; u != kNone; u = unit_next_[u])
            units->push(u);
        if (p_->gpus)
            for (u32 u = gpu_.head[b]; u != kNone; u = gpu_.next[u])
                units->push(u | kOfflineGpu);
        taken += static_cast<double>(bodies_[b].units) + gpu_count(b);
    }
}

// X4. The burnt units' holders now (in the units' order, each once) are the
// victims: every core they hold goes offline until `until` -- whole
// programs, since a dormant program can't hold cores --, and the burnt units
// still free go offline too, so the cores that ran the fever are the ones that
// burn. Each victim goes dormant as a frozen one does, but resting here first
// (go_dormant's burnout mode); one with no reserve gets a grace reserve with
// chance grace_chance, else dies (an ordinary death). Without dormancy every
// victim dies. NPU units go back to their pool, as a dormant program's do.
DisasterCount Soup::burnout(const u32* units, u32 n, u64 until, double grace_chance, u32 grace, Rng& r) {
    DisasterCount out = {};
    Array<u32> victims;
    for (u32 i = 0; i < n; ++i) {
        const u32 u = units[i] & ~kOfflineGpu;
        u32 o = kNone;
        if (units[i] & kOfflineGpu)
            o = u < gpu_.units() ? gpu_.owner[u] : kNone;
        else
            o = u < p_->millicores ? unit_owner_[u] : kNone;
        if (o == kNone || !bodies_[o].alive || dormant(o))
            continue;
        bool seen = false;
        for (usize v = 0; v < victims.size() && !seen; ++v)
            seen = victims[v] == o;
        if (!seen)
            victims.push(o);
    }
    for (u32 i = 0; i < n; ++i) { // the burnt units nobody holds
        const u32 u = units[i] & ~kOfflineGpu;
        if (units[i] & kOfflineGpu) {
            if (u < gpu_.units() && gpu_.owner[u] == kNone && gpu_.pool_pos[u] != kNone) {
                gpu_.pool_remove(u);
                offline_.push(OfflineCore{until, units[i], 0});
                ++out.a;
            }
        } else if (u < p_->millicores && unit_owner_[u] == kNone && pool_pos_[u] != kNone) {
            pool_remove(u);
            offline_.push(OfflineCore{until, u, 0});
            ++out.a;
        }
    }
    for (usize v = 0; v < victims.size(); ++v) {
        const u32 b = victims[v];
        Body& B = bodies_[b];
        out.a += B.units + gpu_count(b);
        const u32 code = B.blocks == kNone ? 0 : blocks_[B.blocks].owned;
        const bool bare = B.mem <= code && disk_held(b) == 0;
        const u32 g = bare && grace > 0 && r.chance(grace_chance) ? grace : 0;
        if (p_->dormancy_auto && go_dormant(b, until, g)) { // (the automatic rule: life.dormancy)
            ++out.b;
            out.c += bare ? 1 : 0;
            continue;
        }
        while (B.unit_head != kNone) { // it dies, and its cores burn all the same
            const u32 u = B.unit_head;
            unlink_unit(u);
            offline_.push(OfflineCore{until, u, 0});
        }
        if (p_->gpus)
            while (gpu_.head[b] != kNone) {
                const u32 u = gpu_.head[b];
                gpu_.unlink(u);
                offline_.push(OfflineCore{until, u | kOfflineGpu, 0});
            }
        kill(b);
        ++out.d;
    }
    if (out.d)
        order_compact();
    ++tally_.burnouts;
    tally_.cores_burnt += out.a;
    tally_.burnout_dormant += out.b;
    tally_.burnout_graced += out.c;
    tally_.burnout_deaths += out.d;
    return out;
}

// X4. Repaired cores come back to their pools, cool, in the order they went.
u32 Soup::repair(u64 tick) {
    u32 back = 0;
    usize w = 0;
    for (usize i = 0; i < offline_.size(); ++i) {
        const OfflineCore x = offline_[i];
        if (x.until > tick) {
            offline_[w++] = x;
            continue;
        }
        const u32 u = x.unit & ~kOfflineGpu;
        if (x.unit & kOfflineGpu) {
            gpu_.heat[u] = 0.0f;
            gpu_.pool_push(u);
        } else {
            unit_heat_[u] = 0.0f;
            pool_push(u);
        }
        ++back;
    }
    offline_.resize(w);
    return back;
}

// X5, as amended by the disk tiers (docs/world-plan.md W4): one tier of the
// disk is lost, chosen by size -- the archive with chance archive / (disk +
// archive), drawn from the disaster's own stream `r`, and only when the node
// has both tiers (with one, it is that one, and nothing is drawn, so a world
// without an archive draws as before). Every holding on that tier is released
// (in schedule order), every codon of it becomes INERT (codon 0 under
// nova_v2) with no stamp, and resting dormant programs lose that tier's
// reserve (fail_disk_tier). Nobody dies directly. Returns the codons that
// were held on it; *archive says which tier it was.
u32 Soup::fail_disk(Rng& r, bool* archive) {
    *archive = false;
    if (!disk_on())
        return 0;
    const u32 fast_n = disk_.fast, arch_n = disk_.size() - disk_.fast;
    if (fast_n + arch_n == 0)
        return 0;
    bool arch = fast_n == 0; // (an archive alone)
    if (fast_n > 0 && arch_n > 0)
        arch = r.below(static_cast<u64>(fast_n) + arch_n) < arch_n;
    const u32 held = arch ? arch_n - disk_.afree : fast_n - disk_.fast_free();
    fail_disk_tier(arch);
    ++tally_.disk_failures;
    tally_.disk_lost += held;
    *archive = arch;
    return held;
}

// X6. A strike randomises a run of memory, living code or not; ownership
// stays, so programs caught in it carry mangled code (most fail, some
// mutate). Random codons are nobody's remnant.
u32 Soup::strike(u32 at, u32 len, Rng& r) {
    const u32 n = p_->size;
    if (at >= n || len == 0)
        return 0;
    if (p_->rays) { // docs/lifehistory-plan.md R2: a ray that randomises what it passes
        const bool fwd = r.below(2) == 0;
        const double e = static_cast<double>(len) * p_->ray_strike * p_->ray_absorb;
        const RayRun h = ray(at, fwd, e, false, true, r);
        if (obs_ && h.n)
            obs_->on_wipe(h.lo, h.n);
        ++tally_.strikes;
        tally_.strike_codons += h.n;
        return h.n;
    }
    if (len > n - at)
        len = n - at;
    for (u32 a = at; a < at + len; ++a) {
        mem_[a] = static_cast<u8>(r.next() & 63);
        if (!stamp_.empty())
            stamp_[a] = 0;
    }
    if (obs_)
        obs_->on_wipe(at, len);
    ++tally_.strikes;
    tally_.strike_codons += len;
    return len;
}

// X10, at the end of the tick: a failing disk's extra rot (counted with the
// disk's own; over both tiers, since which one fails is drawn at the strike,
// docs/world-plan.md W4), and a storm's static -- single-bit flips anywhere
// in memory, sampled as a Poisson process over the soup like rot.
void Soup::weather_hazards() {
    if (weather_.disk_rot > 0 && disk_on())
        tally_.disk_rot += disk_.rot(rng_, weather_.disk_rot);
    if (weather_.static_rot > 0 && p_->rays) // docs/lifehistory-plan.md R2: static as low-energy rays
        tally_.static_flips +=
            ray_shower(ray_rate(weather_.static_rot, p_->ray_static), p_->ray_static, false, rng_);
    else if (weather_.static_rot > 0) {
        const double rate = weather_.static_rot, n = static_cast<double>(p_->size);
        double pos = -log(1.0 - rng_.unit()) / rate;
        while (pos < n) {
            mem_[static_cast<u32>(pos)] ^= static_cast<u8>(1u << rng_.below(6));
            ++tally_.static_flips;
            pos += -log(1.0 - rng_.unit()) / rate;
        }
    }
}

// ===========================================================================
// The world's side
// ===========================================================================

// X8: every disaster.* key, read once; none is a live knob.
void World::read_disasters(Config& cfg) {
    dp_ = DisasterParams{};
    disasters_ = soup_params_.disasters;
    if (!disasters_)
        return;
    DisasterParams& d = dp_;
    char key[64], hi[64];
    auto pair_u32 = [&](const char* lo_key, const char* hi_key, u32* lo, u32* hi_v) {
        *lo = key_u32(cfg, lo_key);
        *hi_v = key_u32(cfg, hi_key);
        if (*lo > *hi_v) {
            snprintf(hi, sizeof hi, "must not exceed %s", hi_key);
            cfg.fail(lo_key, hi);
        }
    };
    auto pair_f64 = [&](const char* lo_key, const char* hi_key, double* lo, double* hi_v) {
        *lo = key_f64(cfg, lo_key);
        *hi_v = key_f64(cfg, hi_key);
        if (*lo > *hi_v) {
            snprintf(hi, sizeof hi, "must not exceed %s", hi_key);
            cfg.fail(lo_key, hi);
        }
    };
    char lead_hi[64];
    for (u32 k = 0; k < kDisKinds; ++k) {
        const char* name = disaster_kind_name(k);
        snprintf(key, sizeof key, "disaster.%s", name);
        const bool on = key_flag(cfg, key);
        snprintf(key, sizeof key, "disaster.%s_every", name);
        d.every[k] = static_cast<double>(key_u64(cfg, key));
        d.on[k] = on && d.every[k] > 0;
        if (k != kDisStorm) { // a storm's warning is the storm itself
            snprintf(key, sizeof key, "disaster.%s_lead_min", name);
            snprintf(lead_hi, sizeof lead_hi, "disaster.%s_lead_max", name);
            pair_u32(key, lead_hi, &d.lead_min[k], &d.lead_max[k]);
        }
        snprintf(key, sizeof key, "disaster.%s_band", name);
        d.band[k] = key_u32(cfg, key);
        if (k == kDisStorm) // (a storm's steady voice is its rumble)
            snprintf(key, sizeof key, "disaster.rumble");
        else
            snprintf(key, sizeof key, "disaster.%s_loud", name);
        d.loud[k] = key_f64(cfg, key);
    }
    pair_u32("disaster.surge_hops_min", "disaster.surge_hops_max", &d.hops_min, &d.hops_max);
    pair_u32("disaster.reboot_min", "disaster.reboot_max", &d.reboot_min, &d.reboot_max);
    pair_f64("disaster.crash_share_min", "disaster.crash_share_max", &d.crash_min, &d.crash_max);
    pair_f64("disaster.burnout_share_min", "disaster.burnout_share_max", &d.burn_min, &d.burn_max);
    pair_u32("disaster.repair_min", "disaster.repair_max", &d.repair_min, &d.repair_max);
    d.grace_chance = key_f64(cfg, "disaster.grace_chance");
    d.grace = key_u32(cfg, "disaster.grace");
    pair_u32("disaster.storm_life_min", "disaster.storm_life_max", &d.life_min, &d.life_max);
    pair_u32("disaster.storm_move_min", "disaster.storm_move_max", &d.move_min, &d.move_max);
    d.turn = key_f64(cfg, "disaster.storm_turn") * kTau / 360.0;
    pair_u32("disaster.strike_every_min", "disaster.strike_every_max", &d.strike_every_min, &d.strike_every_max);
    pair_u32("disaster.strike_min", "disaster.strike_max", &d.strike_min, &d.strike_max);
    d.symptoms = key_flag(cfg, "disaster.symptoms");
    d.false_alarms = key_f64(cfg, "disaster.false_alarms");
    d.brownout = key_f64(cfg, "disaster.brownout");
    d.noise = key_f64(cfg, "disaster.noise");
    d.faults = key_f64(cfg, "disaster.crash_faults");
    d.halts = key_f64(cfg, "disaster.crash_halts");
    d.fever = key_f64(cfg, "disaster.fever");
    d.disk_rot = key_f64(cfg, "disaster.disk_rot");
    d.static_rot = key_f64(cfg, "disaster.static");
    d.sounds = key_flag(cfg, "disaster.sounds");
    d.bang = key_f64(cfg, "disaster.bang");
    d.thunder = key_f64(cfg, "disaster.thunder");
}

u64 World::rebooting_count() const {
    u64 c = 0;
    for (u32 n = 0; n < reboot_until_.size(); ++n)
        c += rebooting(n) ? 1 : 0;
    return c;
}

u64 World::cores_offline() const {
    if (!disasters_)
        return 0;
    u64 c = 0;
    for (u32 n = 0; n < nodes_; ++n)
        c += soups_[n].offline_count();
    return c;
}

// X10: an episode's strength at a tick, 0 to 1. It rises as the square of
// the time since it began over its lead time -- weak early, within the
// normal variation, and strong late -- so a real one reaches 1 when it
// strikes. A false alarm rises the same way to its peak, then fades linearly
// to nothing by its end.
double World::episode_level(const Episode& e, u64 t) const {
    if (t < e.start || t > e.end || e.lead == 0)
        return 0.0;
    if (t <= e.peak) {
        const double x = static_cast<double>(t - e.start) / static_cast<double>(e.lead);
        return x * x;
    }
    const double xp = static_cast<double>(e.peak - e.start) / static_cast<double>(e.lead);
    return xp * xp * static_cast<double>(e.end - t) / static_cast<double>(e.end - e.peak);
}

// X10: each node's symptoms for the tick about to run, from the episodes and
// storms as they stand: a surge's brownout (heat on every unit and page, and
// signal noise) over its region; a crash's spurious faults and halts on its
// node; a burnout's fever on the cores that will burn; a disk's rot; a storm's
// static (bit flips and signal noise), full under it and building on the node
// it moves to next as the move approaches.
void World::weather_update() {
    for (u32 n = 0; n < nodes_; ++n) {
        node_symptom_[n] = 0;
        soups_[n].set_weather(Weather{});
        soups_[n].clear_fever();
    }
    if (!dp_.symptoms)
        return;
    const u64 t = tick_;
    for (usize i = 0; i < episodes_.size(); ++i) {
        const Episode& e = episodes_[i];
        const double lv = episode_level(e, t);
        if (lv <= 0)
            continue;
        const u32* list = ep_list_.data() + e.off;
        if (e.kind == kDisSurge) {
            for (u32 j = 0; j < e.n; ++j) {
                const u32 n = list[j];
                if (!live(n))
                    continue;
                Weather w = soups_[n].weather();
                w.heat += static_cast<float>(dp_.brownout * lv);
                w.noise += static_cast<float>(dp_.noise * lv);
                soups_[n].set_weather(w);
                node_symptom_[n] |= 1u << kDisSurge;
            }
            continue;
        }
        const u32 n = e.node;
        if (!live(n))
            continue;
        Weather w = soups_[n].weather();
        if (e.kind == kDisCrash) {
            w.faults += dp_.faults * lv;
            w.halts = dp_.halts;
        } else if (e.kind == kDisBurnout) {
            for (u32 j = 0; j < e.n; ++j)
                soups_[n].add_fever(list[j], static_cast<float>(dp_.fever * lv));
        } else if (e.kind == kDisDiskfail) {
            w.disk_rot += dp_.disk_rot * lv;
        }
        soups_[n].set_weather(w);
        node_symptom_[n] |= 1u << e.kind;
    }
    for (usize i = 0; i < storms_.size(); ++i) {
        const Storm& s = storms_[i];
        if (s.node < nodes_ && live(s.node)) {
            Weather w = soups_[s.node].weather();
            w.static_rot += dp_.static_rot;
            w.noise += static_cast<float>(dp_.noise);
            soups_[s.node].set_weather(w);
            node_symptom_[s.node] |= 1u << kDisStorm;
        }
        if (s.dest < nodes_ && s.dest != s.node && live(s.dest) && s.next_move > s.last_move) {
            double f = static_cast<double>(t - s.last_move) / static_cast<double>(s.next_move - s.last_move);
            f = f < 0 ? 0 : (f > 1 ? 1 : f);
            Weather w = soups_[s.dest].weather();
            w.static_rot += dp_.static_rot * f;
            w.noise += static_cast<float>(dp_.noise * f);
            soups_[s.dest].set_weather(w);
            node_symptom_[s.dest] |= 1u << kDisStorm;
        }
    }
}

void World::dis_event(const TextBuf& t) {
    dev_tick_.push(tick_);
    dev_off_.push(static_cast<u32>(dev_text_.size()));
    dev_text_.append(t.view());
}

// A random live node (up_only: not rebooting), from the stream given.
u32 World::dis_live_node(Rng& r, bool up_only) {
    u32 count = 0;
    for (u32 n = 0; n < nodes_; ++n)
        count += live(n) && !(up_only && rebooting(n)) ? 1 : 0;
    if (count == 0)
        return kNone;
    u32 pick = static_cast<u32>(r.below(count));
    for (u32 n = 0; n < nodes_; ++n)
        if (live(n) && !(up_only && rebooting(n)) && pick-- == 0)
            return n;
    return kNone;
}

// X1: the barrier's disasters. Repairs first; reboots over; then each kind in
// its fixed order: the episodes due now (in the order they began) strike, or
// fade if false, and the schedule's draw for this tick begins new ones (or,
// without symptoms, strikes at once). Then the storms.
void World::disasters_barrier() {
    const u64 T = tick_;
    for (u32 n = 0; n < nodes_; ++n)
        node_struck_[n] = 0;
    snd_bang_.clear();
    for (u32 n = 0; n < nodes_; ++n) {
        if (live(n) && soups_[n].offline_count())
            soups_[n].repair(T);
        if (reboot_until_[n] && (reboot_until_[n] <= T + 1 || !live(n)))
            reboot_until_[n] = 0;
    }
    for (u32 k = 0; k < kDisStorm; ++k) {
        bool removed = false;
        for (usize i = 0; i < episodes_.size(); ++i) {
            if (episodes_[i].kind != k || episodes_[i].end > T)
                continue;
            const Episode e = episodes_[i];
            const bool struck = !e.false_alarm && dis_strike(k, e);
            TextBuf t;
            t.append("symptom_end kind=");
            t.append(disaster_kind_name(k));
            kv_u64(t, "node", e.node);
            kv_u64(t, "start", e.start);
            kv_u64(t, "false", e.false_alarm);
            kv_u64(t, "struck", struck ? 1 : 0);
            dis_event(t);
            episodes_[i].kind = kDisKinds; // (gone)
            removed = true;
        }
        if (removed) { // the rest, in order, with their lists packed
            Array<u32> list;
            usize w = 0;
            for (usize i = 0; i < episodes_.size(); ++i) {
                if (episodes_[i].kind == kDisKinds)
                    continue;
                Episode e = episodes_[i];
                const u32 off = static_cast<u32>(list.size());
                list.append(ep_list_.data() + e.off, e.n);
                e.off = off;
                episodes_[w++] = e;
            }
            episodes_.resize(w);
            ep_list_ = static_cast<Array<u32>&&>(list);
        }
        if (!dp_.on[k] || (k == kDisSurge && !lifecycle_)) // (a surge needs the lifecycle: a map world)
            continue;
        Rng r(seed_ ^ kSchedSalt, Rng::kWorldNode, (T << 3) | k);
        const double rate = 1.0 / dp_.every[k] / (dp_.symptoms ? 1.0 - dp_.false_alarms : 1.0);
        const u32 count = poisson(r, rate);
        for (u32 c = 0; c < count; ++c)
            dis_begin(k, r);
    }
    dis_storms();
}

// One disaster of a kind, its node drawn from the schedule's stream (a crash's
// among nodes not already rebooting). Its victims are fixed now: a surge's
// region (every live node within its drawn hops along paths), a burnout's
// cores. With symptoms, an episode begins -- a false alarm with chance
// disaster.false_alarms --; without, it strikes at once.
void World::dis_begin(u32 kind, Rng& r) {
    const u32 node = dis_live_node(r, kind == kDisCrash);
    if (node == kNone)
        return;
    Episode e = {};
    e.kind = kind;
    e.node = node;
    e.serial = dis_serial_++;
    e.off = static_cast<u32>(ep_list_.size());
    if (kind == kDisSurge) { // breadth first over the paths, live nodes only
        e.hops = draw_u32(r, dp_.hops_min, dp_.hops_max);
        Array<u32> dist;
        dist.resize(nodes_);
        dist.fill(kNone);
        dist[node] = 0;
        ep_list_.push(node);
        for (usize q = e.off; q < ep_list_.size(); ++q) {
            const u32 u = ep_list_[q];
            if (dist[u] >= e.hops)
                continue;
            for (u32 x = out_begin_[u]; x < out_begin_[u + 1]; ++x) {
                const u32 v = edge_dst_[x];
                if (dist[v] != kNone || !live(v))
                    continue;
                dist[v] = dist[u] + 1;
                ep_list_.push(v);
            }
        }
    }
    if (kind == kDisBurnout)
        e.share = draw_f64(r, dp_.burn_min, dp_.burn_max);
    if (!dp_.symptoms) {
        if (kind == kDisBurnout)
            soups_[node].pick_burnout(e.share, r, &ep_list_);
        e.n = static_cast<u32>(ep_list_.size()) - e.off;
        e.start = e.peak = e.end = tick_;
        dis_strike(kind, e);
        ep_list_.resize(e.off);
        return;
    }
    e.lead = draw_u32(r, dp_.lead_min[kind], dp_.lead_max[kind]);
    e.false_alarm = r.chance(dp_.false_alarms) ? 1 : 0;
    if (kind == kDisBurnout) // (a false alarm's fever is on cores that would have burnt)
        soups_[node].pick_burnout(e.share, r, &ep_list_);
    e.n = static_cast<u32>(ep_list_.size()) - e.off;
    e.start = tick_ + 1;
    if (!e.false_alarm) {
        e.peak = e.end = e.start + e.lead;
    } else { // it peaks somewhere in its lead time, then fades in half the time it rose
        const double frac = draw_f64(r, 0.3, 1.0);
        e.peak = e.start + static_cast<u64>(floor(frac * static_cast<double>(e.lead) + 0.5));
        const u64 fade = (e.peak - e.start) / 2;
        e.end = e.peak + (fade > 0 ? fade : 1);
    }
    episodes_.push(e);
    ++dis_tally_.episodes;
    dis_tally_.false_alarms += e.false_alarm;
    TextBuf t;
    t.append("symptom kind=");
    t.append(disaster_kind_name(kind));
    kv_u64(t, "node", node);
    kv_u64(t, "start", e.start);
    kv_u64(t, "until", e.end);
    kv_u64(t, "false", e.false_alarm);
    if (kind == kDisSurge) {
        kv_u64(t, "hops", e.hops);
        kv_u64(t, "nodes", e.n);
    }
    if (kind == kDisBurnout)
        kv_u64(t, "cores", e.n);
    if (dp_.sounds && channels_) { // its voice (X11): band and peak loudness
        kv_u64(t, "band", dp_.band[kind] % channels_);
        kv_u64(t, "loud", static_cast<u64>(dp_.loud[kind]));
    }
    dis_event(t);
}

// The disaster itself, at this barrier, its draws from a stream keyed by its
// serial; false if it found nothing to hit (its node gone, or down already).
bool World::dis_strike(u32 kind, const Episode& e) {
    Rng r(seed_ ^ kStrikeSalt, Rng::kWorldNode, e.serial);
    const u64 T = tick_;
    const u32* list = ep_list_.data() + e.off;
    TextBuf t;
    if (kind == kDisSurge) { // X2: every live node of the region collapses (the lifecycle does it next)
        u32 nodes = 0, source = kNone;
        u64 deaths = 0;
        for (u32 i = 0; i < e.n; ++i) {
            const u32 n = list[i];
            if (!live(n))
                continue;
            deaths += soups_[n].body_count();
            soups_[n].force_collapse();
            reboot_until_[n] = 0;
            ++node_struck_[n];
            ++nodes;
            source = source == kNone ? n : source;
        }
        if (nodes == 0)
            return false;
        ++dis_tally_.surges;
        dis_tally_.surge_nodes += nodes;
        dis_tally_.surge_deaths += deaths;
        snd_bang_.push(live(e.node) ? e.node : source); // the bang (X11)
        snd_bang_.push(kDisSurge);
        t.append("surge node=");
        t.append_u64(e.node);
        kv_u64(t, "hops", e.hops);
        kv_u64(t, "nodes", nodes);
        kv_u64(t, "deaths", deaths);
        if (dp_.sounds && channels_)
            kv_u64(t, "bang", static_cast<u64>(dp_.bang));
    } else if (kind == kDisCrash) { // X3
        const u32 n = e.node;
        if (!live(n) || rebooting(n))
            return false;
        const u32 reboot = draw_u32(r, dp_.reboot_min, dp_.reboot_max);
        const double share = draw_f64(r, dp_.crash_min, dp_.crash_max);
        const DisasterCount c = soups_[n].crash(share, T + 1 + reboot, r);
        reboot_until_[n] = T + 1 + reboot;
        ++node_struck_[n];
        t.append("crash node=");
        t.append_u64(n);
        kv_u64(t, "deleted", c.a);
        kv_u64(t, "restarted", c.b);
        kv_u64(t, "reboot", reboot);
    } else if (kind == kDisBurnout) { // X4
        const u32 n = e.node;
        if (!live(n))
            return false;
        const u32 repair = draw_u32(r, dp_.repair_min, dp_.repair_max);
        const DisasterCount c = soups_[n].burnout(list, e.n, T + repair, dp_.grace_chance, dp_.grace, r);
        ++node_struck_[n];
        t.append("burnout node=");
        t.append_u64(n);
        kv_u64(t, "cores", c.a);
        kv_u64(t, "dormant", c.b);
        kv_u64(t, "graced", c.c);
        kv_u64(t, "died", c.d);
        kv_u64(t, "repair", repair);
    } else if (kind == kDisDiskfail) { // X5 (W4: one tier, chosen by size, from this disaster's stream)
        const u32 n = e.node;
        if (!live(n) || (node_params_[n].disk == 0 && node_params_[n].archive == 0))
            return false;
        bool archive = false;
        const u32 lost = soups_[n].fail_disk(r, &archive);
        ++node_struck_[n];
        t.append("diskfail node=");
        t.append_u64(n);
        kv_u64(t, "codons", lost);
        if (node_params_[n].archive) // (which tier, where there are two)
            t.append(archive ? " tier=archive" : " tier=fast");
    } else {
        return false;
    }
    dis_event(t);
    return true;
}

// X6: the neighbour a storm over `node` would move to: along a path, to a live
// node, the one whose direction on the map best matches its heading (ties to
// the nearer). Without a map, a random one. kNone with nowhere to go.
u32 World::storm_dest(u32 node, double heading, Rng& r) const {
    if (node >= nodes_)
        return kNone;
    const u32 b = out_begin_[node], deg = out_begin_[node + 1] - out_begin_[node];
    if (!spatial_) {
        u32 open = 0;
        for (u32 k = 0; k < deg; ++k)
            open += live(edge_dst_[port_list_[b + k]]) ? 1 : 0;
        if (open == 0)
            return kNone;
        u32 pick = static_cast<u32>(r.below(open));
        for (u32 k = 0; k < deg; ++k)
            if (live(edge_dst_[port_list_[b + k]]) && pick-- == 0)
                return edge_dst_[port_list_[b + k]];
        return kNone;
    }
    double best = -2.0;
    u32 to = kNone;
    for (u32 k = 0; k < deg; ++k) { // nearest first
        const u32 m = edge_dst_[port_list_[b + k]];
        if (!live(m))
            continue;
        const double score = cos(atan2(node_y_[m] - node_y_[node], node_x_[m] - node_x_[node]) - heading);
        if (score > best) {
            best = score;
            to = m;
        }
    }
    return to;
}

// X6: each storm in turn: its strikes due (only where it is), its end, its
// move (to the neighbour decided ahead, if it's still there, else the best
// now; staying put with nowhere to go), its heading's drift and its next
// move decided. Then new storms form.
void World::dis_storms() {
    const u64 T = tick_;
    bool removed = false;
    for (usize i = 0; i < storms_.size(); ++i) {
        Storm& s = storms_[i];
        Rng r(seed_ ^ kStormSalt, Rng::kWorldNode, s.serial);
        r.set_counter(s.draws);
        while (s.next_strike <= T) {
            if (!s.dry && live(s.node)) {
                const u32 len = draw_u32(r, dp_.strike_min, dp_.strike_max);
                const u32 at = static_cast<u32>(r.below(node_params_[s.node].size));
                const u32 got = soups_[s.node].strike(at, len, r);
                ++node_struck_[s.node];
                snd_bang_.push(s.node); // thunder (X11)
                snd_bang_.push(kDisStorm);
                TextBuf t;
                t.append("lightning node=");
                t.append_u64(s.node);
                kv_u64(t, "codons", got);
                if (dp_.sounds && channels_)
                    kv_u64(t, "thunder", static_cast<u64>(dp_.thunder));
                dis_event(t);
            }
            s.next_strike += 1 + static_cast<u64>(floor(-log(1.0 - r.unit()) * s.strike_mean));
        }
        if (T >= s.end) {
            TextBuf t;
            t.append("storm ended node=");
            t.append_u64(s.node);
            kv_u64(t, "moves", s.moves);
            dis_event(t);
            s.draws = r.counter();
            s.moves = 0xFFFFFFFFu; // (gone)
            removed = true;
            continue;
        }
        if (T >= s.next_move) {
            u32 to = s.dest;
            bool adjacent = false;
            if (to < nodes_ && live(to))
                for (u32 x = out_begin_[s.node]; x < out_begin_[s.node + 1] && !adjacent; ++x)
                    adjacent = edge_dst_[x] == to;
            if (!adjacent)
                to = storm_dest(s.node, s.heading, r);
            if (to != kNone && to != s.node) {
                TextBuf t;
                t.append("storm moved from=");
                t.append_u64(s.node);
                kv_u64(t, "to", to);
                dis_event(t);
                s.node = to;
                ++s.moves;
            }
            s.heading += (2.0 * r.unit() - 1.0) * dp_.turn;
            s.heading -= kTau * floor(s.heading / kTau);
            s.last_move = T;
            s.next_move = T + draw_u32(r, dp_.move_min, dp_.move_max);
            s.dest = storm_dest(s.node, s.heading, r);
        }
        s.draws = r.counter();
    }
    if (removed) {
        usize w = 0;
        for (usize i = 0; i < storms_.size(); ++i)
            if (storms_[i].moves != 0xFFFFFFFFu)
                storms_[w++] = storms_[i];
        storms_.resize(w);
    }
    if (!dp_.on[kDisStorm])
        return;
    Rng r(seed_ ^ kSchedSalt, Rng::kWorldNode, (T << 3) | kDisStorm);
    const double rate = 1.0 / dp_.every[kDisStorm] / (dp_.symptoms ? 1.0 - dp_.false_alarms : 1.0);
    const u32 count = poisson(r, rate);
    for (u32 c = 0; c < count; ++c) {
        const u32 node = dis_live_node(r, false);
        if (node == kNone)
            continue;
        Storm s = {};
        s.serial = dis_serial_++;
        Rng sr(seed_ ^ kStormSalt, Rng::kWorldNode, s.serial);
        s.born = T;
        s.node = node;
        s.heading = kTau * sr.unit();
        s.end = T + draw_u32(sr, dp_.life_min, dp_.life_max);
        s.strike_mean = draw_f64(sr, dp_.strike_every_min, dp_.strike_every_max);
        s.dry = dp_.symptoms && sr.chance(dp_.false_alarms) ? 1 : 0; // a false alarm: static and rumble, no lightning
        s.last_move = T;
        s.next_move = T + draw_u32(sr, dp_.move_min, dp_.move_max);
        s.next_strike = T + 1 + static_cast<u64>(floor(-log(1.0 - sr.unit()) * s.strike_mean));
        s.dest = storm_dest(node, s.heading, sr);
        s.draws = sr.counter();
        storms_.push(s);
        ++dis_tally_.storms_formed;
        TextBuf t;
        t.append("storm formed node=");
        t.append_u64(node);
        kv_u64(t, "heading", static_cast<u64>(floor(s.heading * 360.0 / kTau + 0.5)) % 360);
        kv_u64(t, "dry", s.dry);
        if (dp_.sounds && channels_) {
            kv_u64(t, "band", dp_.band[kDisStorm] % channels_);
            kv_u64(t, "loud", static_cast<u64>(dp_.loud[kDisStorm]));
        }
        dis_event(t);
    }
}

void World::sound_add(u32 node, u32 channel, double amp) {
    const usize i = static_cast<usize>(node) * channels_ + channel % channels_;
    if (snd_[i] == 0.0)
        snd_touched_.push(static_cast<u32>(i));
    snd_[i] += amp;
}

// X11: the disasters' sounds this tick, emitted into the signal field as a
// program's EMIT is -- spread at this barrier along the signal paths, faded
// per hop, delayed by the paths' latency, dropped under each target's floor
// -- so the source hears its own at full strength next tick and its
// neighbours fainter and later. Each kind has its own band and pattern:
//   surge     a hum on its region, alternating in sign, rising with the
//             brownout; a bang from its epicentre at the surge;
//   crash     stuttering clicks, more often as the instability grows;
//   burnout   a crackle from the fevered node, random in size and sign;
//   diskfail  a grinding: a regular pulse whose period shortens from 32
//             ticks to 4 as the decay grows;
//   storm     a rumble wherever a storm is, and thunder at each strike.
// False alarms sound too, and fade with their symptoms.
void World::scatter_sounds() {
    snd_channel_.fill(0.0);
    if (!dp_.sounds) {
        snd_bang_.clear();
        return;
    }
    const u64 T = tick_;
    for (usize i = 0; i < episodes_.size(); ++i) {
        const Episode& e = episodes_[i];
        const double lv = episode_level(e, T);
        if (lv <= 0)
            continue;
        const u32 band = dp_.band[e.kind], width = kBandWidth[e.kind];
        const double loud = dp_.loud[e.kind];
        if (e.kind == kDisSurge) {
            const double amp = loud * lv * ((T & 1) ? -1.0 : 1.0);
            const u32* list = ep_list_.data() + e.off;
            for (u32 j = 0; j < e.n; ++j)
                if (live(list[j]))
                    for (u32 w = 0; w < width; ++w)
                        sound_add(list[j], band + w, amp);
            continue;
        }
        if (!live(e.node))
            continue;
        Rng sr(seed_ ^ kSoundSalt, static_cast<u32>(e.serial), T);
        if (e.kind == kDisCrash) {
            if (sr.chance(lv))
                sound_add(e.node, band + static_cast<u32>(sr.below(width)), loud);
        } else if (e.kind == kDisBurnout) {
            if (sr.chance(lv))
                sound_add(e.node, band + static_cast<u32>(sr.below(width)),
                          loud * (0.5 + sr.unit()) * (sr.below(2) ? 1.0 : -1.0));
        } else if (e.kind == kDisDiskfail) {
            const u64 period = 4 + static_cast<u64>(floor(28.0 * (1.0 - sqrt(lv)) + 0.5));
            if ((T - e.start) % period == 0)
                for (u32 w = 0; w < width; ++w)
                    sound_add(e.node, band + w, loud);
        }
    }
    for (usize i = 0; i < storms_.size(); ++i) {
        const Storm& s = storms_[i];
        if (s.node >= nodes_ || !live(s.node))
            continue;
        Rng sr(seed_ ^ kSoundSalt, static_cast<u32>(s.serial), T);
        for (u32 w = 0; w < kBandWidth[kDisStorm]; ++w)
            sound_add(s.node, dp_.band[kDisStorm] + w, dp_.loud[kDisStorm] * (2.0 * sr.unit() - 1.0));
    }
    for (usize i = 0; i + 1 < snd_bang_.size(); i += 2) {
        const u32 n = snd_bang_[i], kind = snd_bang_[i + 1];
        const double amp = kind == kDisSurge ? dp_.bang : dp_.thunder;
        for (u32 w = 0; w < kBandWidth[kind]; ++w)
            sound_add(n, dp_.band[kind] + w, amp);
    }
    snd_bang_.clear();
    // Onto the paths, in source and channel order, as scatter_signals does.
    Array<u64> keys;
    for (usize i = 0; i < snd_touched_.size(); ++i)
        keys.push(snd_touched_[i]);
    snd_touched_.clear();
    sort_u64(keys.data(), keys.size());
    for (usize i = 0; i < keys.size(); ++i) {
        if (i > 0 && keys[i] == keys[i - 1])
            continue;
        const usize at = static_cast<usize>(keys[i]);
        const double amp = snd_[at];
        snd_[at] = 0.0;
        const u32 e = static_cast<u32>(at / channels_), c = static_cast<u32>(at % channels_);
        if (amp == 0.0 || !live(e))
            continue;
        const double mag = fabs(amp);
        ++dis_tally_.sounds;
        dis_tally_.sound_amp += static_cast<u64>(floor(mag + 0.5));
        snd_channel_[c] += mag;
        for (u32 j = route_begin_[e]; j < route_begin_[e + 1]; ++j) {
            const u32 m = route_tgt_[j];
            const double a = amp * att_pow_[route_hops_[j]];
            if (fabs(a) < node_params_[m].signal_floor)
                continue;
            const usize slot = static_cast<usize>((T + route_delay_[j]) % sig_slots_);
            pending_[(slot * nodes_ + m) * channels_ + c] += a;
        }
    }
}

// X9: the disaster state -- every node's reboot, the episodes and their
// lists, the storms, the serial counter -- hashed and saved whenever
// disasters are on. (Each soup's offline cores are in its own section.)
void World::disasters_hash(Hasher& h) const {
    h.word(dis_serial_);
    h.bytes(reboot_until_.data(), reboot_until_.size() * sizeof(u64));
    h.word(episodes_.size());
    h.bytes(episodes_.data(), episodes_.size() * sizeof(Episode));
    h.word(ep_list_.size());
    h.bytes(ep_list_.data(), ep_list_.size() * sizeof(u32));
    h.word(storms_.size());
    h.bytes(storms_.data(), storms_.size() * sizeof(Storm));
}

void World::disasters_save(ByteWriter& w) const {
    w.put_u64(dis_serial_);
    for (u32 n = 0; n < nodes_; ++n)
        w.put_u64(reboot_until_[n]);
    w.put_u32(static_cast<u32>(episodes_.size()));
    w.put_bytes(episodes_.data(), episodes_.size() * sizeof(Episode));
    w.put_u32(static_cast<u32>(ep_list_.size()));
    w.put_bytes(ep_list_.data(), ep_list_.size() * sizeof(u32));
    w.put_u32(static_cast<u32>(storms_.size()));
    w.put_bytes(storms_.data(), storms_.size() * sizeof(Storm));
}

bool World::disasters_load(ByteReader& r, TextBuf* err, u32 version) {
    reboot_until_.resize(nodes_);
    reboot_until_.fill(0);
    episodes_.clear();
    ep_list_.clear();
    storms_.clear();
    dis_serial_ = 0;
    clear_disaster_events();
    if (version < 4) // (before v4: nothing pending)
        return true;
    u32 ne = 0, nl = 0, ns = 0;
    bool ok = r.get_u64(&dis_serial_);
    for (u32 n = 0; ok && n < nodes_; ++n)
        ok = r.get_u64(&reboot_until_[n]);
    ok = ok && r.get_u32(&ne) && static_cast<u64>(ne) * sizeof(Episode) <= r.remaining();
    if (ok) {
        episodes_.resize(ne);
        ok = r.get_bytes(episodes_.data(), static_cast<usize>(ne) * sizeof(Episode));
    }
    ok = ok && r.get_u32(&nl) && static_cast<u64>(nl) * sizeof(u32) <= r.remaining();
    if (ok) {
        ep_list_.resize(nl);
        ok = r.get_bytes(ep_list_.data(), static_cast<usize>(nl) * sizeof(u32));
    }
    ok = ok && r.get_u32(&ns) && static_cast<u64>(ns) * sizeof(Storm) <= r.remaining();
    if (ok) {
        storms_.resize(ns);
        ok = r.get_bytes(storms_.data(), static_cast<usize>(ns) * sizeof(Storm));
    }
    for (usize i = 0; ok && i < episodes_.size(); ++i) {
        const Episode& e = episodes_[i];
        ok = e.kind < kDisStorm && e.node < nodes_ && static_cast<u64>(e.off) + e.n <= nl && e.serial < dis_serial_ &&
             e.start <= e.peak && e.peak <= e.end;
        if (ok && e.kind == kDisSurge)
            for (u32 j = 0; ok && j < e.n; ++j)
                ok = ep_list_[e.off + j] < nodes_;
    }
    for (usize i = 0; ok && i < storms_.size(); ++i) {
        const Storm& s = storms_[i];
        ok = s.node < nodes_ && (s.dest == kNone || s.dest < nodes_) && s.serial < dis_serial_ && s.dry <= 1;
    }
    if (!ok) {
        err->append("snapshot: disaster section truncated or out of range");
        return false;
    }
    return true;
}

} // namespace nova
