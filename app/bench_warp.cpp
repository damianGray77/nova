// nova bench warp DUMP [--repeats N] [--threads N] [--variants ...] [--ops]
// (docs/kernel-spike-plan.md K1, K2; docs/dxc-plan.md): one warp round
// dumped by `nova resume --dump-round`, replayed on
// the GPU's warp kernel. Every word the GPU gives (the images and the lanes)
// is checked against the CPU reference first; then the kernel is timed on the
// device's clock, repeats interleaved and medians shown:
//   - the whole round;
//   - the longest body alone (against the whole round: is a step slow by
//     itself, or because other bodies crowd it?);
//   - the longest bodies only, and the round without its long bodies (every
//     body up to a chain length: the GPU's floor once the longest body stops
//     setting the time).
// A body's chain is its warps' steps, one after another (they run in turn).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nova/accel.h"
#include "nova/array.h"
#include "nova/isa.h"
#include "nova/pool.h"
#include "nova/soup.h"
#include "nova/str.h"

#include "../core/src/warp_layout.inc"

using namespace nova;

namespace {

constexpr u32 kLaneWords = sizeof(Lane) / 4;
constexpr u32 kLdsWords = 2048; // accel_dx.cpp's kWarpLdsWords: larger records run on the plain kernel

// The dumped round, flat: per job its node and where its image, records and
// lanes start in the shared arrays.
struct Dump {
    u32 lanes = 0; // a warp's
    Array<u32> cst;
    Array<u32> node, nimg, nrec, nlane;
    Array<u64> oimg, orec, olane;
    Array<u32> img, recs, words;
    Array<Lane> lane;
    usize jobs() const { return node.size(); }
};

bool read_words(FILE* f, void* p, usize words) {
    return words == 0 || fread(p, 4, words, f) == words;
}

bool load_dump(const char* path, Dump* d, TextBuf* err) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        err->append("cannot open the dump");
        return false;
    }
    u32 head[5] = {};
    // (version 2: the memory tiers' record words, docs/world-plan.md W3; a version 1 dump's records are laid out otherwise)
    bool ok = read_words(f, head, 5) && head[0] == 0x4452574Eu && head[1] == 2u && head[2] >= 1 && head[2] <= kMaxLanes;
    if (!ok) {
        fclose(f);
        err->append("not a warp round dump (version 2)");
        return false;
    }
    d->lanes = head[2];
    d->cst.resize(head[3]);
    ok = read_words(f, d->cst.data(), head[3]);
    for (u32 j = 0; ok && j < head[4]; ++j) {
        u32 jh[4] = {};
        ok = read_words(f, jh, 4);
        if (!ok)
            break;
        d->node.push(jh[0]);
        d->nimg.push(jh[1]);
        d->nrec.push(jh[2]);
        d->nlane.push(jh[3]);
        d->oimg.push(d->img.size());
        d->orec.push(d->recs.size());
        d->olane.push(d->lane.size());
        d->img.resize(d->img.size() + jh[1]);
        d->recs.resize(d->recs.size() + jh[2]);
        d->words.resize(d->words.size() + jh[2]);
        d->lane.resize(d->lane.size() + jh[3]);
        ok = read_words(f, d->img.data() + d->oimg[j], jh[1]) && read_words(f, d->recs.data() + d->orec[j], jh[2]) &&
             read_words(f, d->words.data() + d->orec[j], jh[2]) &&
             read_words(f, d->lane.data() + d->olane[j], static_cast<usize>(jh[3]) * kLaneWords);
    }
    fclose(f);
    if (!ok)
        err->append("the dump is cut short");
    return ok;
}

// The reference on every record, a job a task.
struct RefRun {
    const Dump* d;
    u32* img;
    Lane* lane;
    static void task(void* ctx, u32 j) {
        const RefRun& k = *static_cast<const RefRun*>(ctx);
        const Dump& d = *k.d;
        for (u32 r = 0; r < d.nrec[j]; ++r)
            warp_run_reference(k.img + d.oimg[j], d.cst.data(), k.lane + d.olane[j], d.recs[d.orec[j] + r]);
    }
};

double median(Array<double>& v) {
    for (usize i = 1; i < v.size(); ++i) // a few values: insertion sort
        for (usize k = i; k > 0 && v[k - 1] > v[k]; --k) {
            const double t = v[k];
            v[k] = v[k - 1];
            v[k - 1] = t;
        }
    return v.empty() ? 0.0 : v[v.size() / 2];
}

// One selection of the round's records (a flag per record, in dump order),
// built into the GPU's jobs: each job's chosen records, their sizes, and
// every lane they need sent (the heap starts each run from the dump's lanes).
constexpr u32 kMaxVariants = 8;

struct Pick {
    const char* name;
    Array<u8> on;
    Array<WarpJob> jobs;
    Array<u32> recs, words, send, chains;
    Array<u64> js; // per job: where its records and sends start in recs/send
    u64 steps = 0, longest = 0, nrec = 0;
    Array<double> ms[kMaxVariants];
};

void build_pick(const Dump& d, const Array<u64>& chain, Pick* p, u32* gpu_img) {
    p->jobs.clear();
    p->recs.clear();
    p->words.clear();
    p->send.clear();
    p->chains.clear();
    p->js.clear();
    p->steps = p->longest = p->nrec = 0;
    for (usize j = 0; j < d.jobs(); ++j) {
        const u64 r0 = p->recs.size(), s0 = p->send.size();
        const u32* I = d.img.data() + d.oimg[j];
        for (u32 r = 0; r < d.nrec[j]; ++r) {
            const usize k = d.orec[j] + r;
            if (!p->on[k])
                continue;
            p->recs.push(d.recs[k]);
            p->words.push(d.words[k]);
            p->chains.push(chain[k] < 0xFFFFFFFFull ? static_cast<u32>(chain[k]) : 0xFFFFFFFFu);
            for (u32 i = 0; i < I[d.recs[k] + WB_NWARP]; ++i)
                p->send.push(I[d.recs[k] + WB_WARPS + i * WW_SIZE + WW_LANE0] / d.lanes);
            p->steps += chain[k];
            p->longest = chain[k] > p->longest ? chain[k] : p->longest;
            ++p->nrec;
        }
        p->js.push(r0);
        p->js.push(s0);
    }
    // The pointers once the arrays are whole (they may have moved while growing).
    for (usize j = 0; j < d.jobs(); ++j) {
        const u64 r0 = p->js[2 * j], s0 = p->js[2 * j + 1];
        const u64 r1 = j + 1 < d.jobs() ? p->js[2 * j + 2] : p->recs.size();
        const u64 s1 = j + 1 < d.jobs() ? p->js[2 * j + 3] : p->send.size();
        if (r1 == r0)
            continue;
        WarpJob w = {};
        w.node = d.node[j];
        w.img = gpu_img + d.oimg[j];
        w.img_words = d.nimg[j];
        w.recs = p->recs.data() + r0;
        w.rec_words = p->words.data() + r0;
        w.rec_chain = p->chains.data() + r0;
        w.nrec = static_cast<u32>(r1 - r0);
        w.lanes = d.lane.data() + d.olane[j];
        w.nlanes = d.nlane[j];
        w.send = p->send.data() + s0;
        w.nsend = static_cast<u32>(s1 - s0);
        p->jobs.push(w);
    }
}

// One run of a pick: the images restored, the round sent, the kernel's time.
bool run_pick(WarpGpu& gpu, const Dump& d, Pick& p, u32* gpu_img, ThreadPool& pool, double* kernel_ms, TextBuf* err) {
    memcpy(gpu_img, d.img.data(), d.img.size() * 4);
    if (!gpu.submit(d.cst.data(), static_cast<u32>(d.cst.size()), p.jobs.data(), static_cast<u32>(p.jobs.size()), err,
                    &pool) ||
        !gpu.finish(err, &pool))
        return false;
    *kernel_ms = gpu.last_kernel_ms();
    return true;
}

// The kernels to compare (docs/dxc-plan.md), by name. Each variant's kernel
// says which compiler it got: a DXC variant falls back to FXC where DXC's
// can't run (WarpGpu::kernel()).
struct Variant {
    const char* name;
    const char* what;
    WarpKernelOptions opt;
};

bool find_variant(Str name, Variant* v) {
    static const struct {
        const char* name;
        const char* what;
        u32 lds[4];
        bool fxc, long_first;
    } kAll[] = {
        {"fxc", "FXC's kernel, groupshared images up to 2,048 words (before DXC)", {2048, 0, 0, 0}, true, false},
        {"fxclong", "FXC's kernel, the long bodies first", {2048, 0, 0, 0}, true, true},
        {"dxc2k", "DXC's kernel, groupshared images up to 2,048 words", {2048, 0, 0, 0}, false, false},
        {"dxcplain", "DXC's kernel, every record in video memory", {0, 0, 0, 0}, false, false},
        {"dxctiers", "DXC's kernel, groupshared tiers of 256, 512, 1,024 and 2,048 words", {256, 512, 1024, 2048},
         false, false},
        {"dxc", "DXC's kernel, the tiers, the long bodies first (the world's)", {256, 512, 1024, 2048}, false, true},
    };
    for (const auto& a : kAll)
        if (name.eq(a.name)) {
            v->name = a.name;
            v->what = a.what;
            v->opt = WarpKernelOptions{};
            for (u32 k = 0; k < 4; ++k)
                (a.fxc ? v->opt.fxc_lds : v->opt.lds)[k] = a.lds[k];
            v->opt.fxc = a.fxc;
            v->opt.long_first = a.long_first;
            return true;
        }
    return false;
}

} // namespace

int cmd_bench_warp(int argc, char** argv) {
    // nova bench warp DUMP [--repeats N] [--threads N] [--variants a,b,...] [--ops]
    if (argc < 4) {
        fprintf(stderr, "usage: nova bench warp DUMP [--repeats N] [--threads N] [--ops] [--variants "
                        "fxc,fxclong,dxc2k,dxcplain,dxctiers,dxc]\n");
        return 2;
    }
    const char* path = argv[3];
    u64 repeats = 5, threads = 0;
    bool ops = false; // --ops: the reference's instruction mix first
    Variant vars[kMaxVariants];
    u32 nvar = 0;
    for (int i = 4; i < argc; i += 2) {
        if (strcmp(argv[i], "--ops") == 0) {
            ops = true;
            --i;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "nova bench warp: '%s' needs a value\n", argv[i]);
            return 2;
        }
        if (strcmp(argv[i], "--repeats") == 0 && parse_u64(argv[i + 1], &repeats) && repeats >= 1 && repeats <= 100)
            continue;
        if (strcmp(argv[i], "--threads") == 0 && parse_u64(argv[i + 1], &threads) && threads <= 1024)
            continue;
        if (strcmp(argv[i], "--variants") == 0) {
            const char* s = argv[i + 1];
            bool good = true;
            while (*s && good) {
                const char* comma = strchr(s, ',');
                const usize len = comma ? static_cast<usize>(comma - s) : strlen(s);
                good = nvar < kMaxVariants && find_variant(Str(s, len), &vars[nvar++]);
                s += len + (comma ? 1 : 0);
            }
            if (good)
                continue;
        }
        fprintf(stderr, "nova bench warp: bad option '%s %s'\n", argv[i], argv[i + 1]);
        return 2;
    }
    if (nvar == 0) {
        find_variant(Str("fxc"), &vars[nvar++]);
        find_variant(Str("dxc"), &vars[nvar++]);
    }
    Dump d;
    TextBuf err;
    if (!load_dump(path, &d, &err)) {
        fprintf(stderr, "nova bench warp: %s: %s\n", path, err.c_str());
        return 1;
    }
    ThreadPool pool;
    pool.start(static_cast<u32>(threads));
    const usize nrec = d.recs.size();
    printf("nova bench warp: %s: %u jobs, %llu records, %.1f MB of image, %llu lanes (%u a warp)\n", path,
           static_cast<u32>(d.jobs()), static_cast<unsigned long long>(nrec), d.img.size() * 4.0 / 1048576.0,
           static_cast<unsigned long long>(d.lane.size()), d.lanes);

    // The reference, on copies.
    Array<u32> ref_img;
    ref_img.assign(d.img.data(), d.img.size());
    Array<Lane> ref_lane;
    ref_lane.assign(d.lane.data(), d.lane.size());
    double t0 = clock_seconds();
    RefRun rr = {&d, ref_img.data(), ref_lane.data()};
    pool.run(static_cast<u32>(d.jobs()), &RefRun::task, &rr);
    const double ref_s = clock_seconds() - t0;

    // Each record's chain (its warps' steps, from the reference), and the
    // round's shape.
    Array<u64> chain;
    chain.resize(nrec);
    u64 steps = 0, longest = 0, big = 0, big_steps = 0;
    usize top = 0;
    for (usize j = 0; j < d.jobs(); ++j) {
        const u32* I = ref_img.data() + d.oimg[j];
        for (u32 r = 0; r < d.nrec[j]; ++r) {
            const usize k = d.orec[j] + r;
            u64 c = 0;
            for (u32 i = 0; i < I[d.recs[k] + WB_NWARP]; ++i)
                c += I[d.recs[k] + WB_WARPS + i * WW_SIZE + WW_STEPS];
            chain[k] = c;
            steps += c;
            if (c > longest) {
                longest = c;
                top = k;
            }
            if (d.words[k] > kLdsWords) {
                ++big;
                big_steps += c;
            }
        }
    }
    printf("nova bench warp: the reference: %.0f ms on %u threads; %llu warp steps, the longest body %llu\n",
           ref_s * 1000.0, pool.threads(), static_cast<unsigned long long>(steps),
           static_cast<unsigned long long>(longest));
    for (u32 pass = 0; ops && pass < 2; ++pass) { // the instruction mix (the whole round, then its longest
                                                  // body): the reference again, on one thread, counting
        Array<u64> hist;
        hist.resize(2 * kOpCount + 2);
        hist.fill(0);
        Array<u32> oi;
        oi.assign(d.img.data(), d.img.size());
        Array<Lane> ol;
        ol.assign(d.lane.data(), d.lane.size());
        RefRun orr = {&d, oi.data(), ol.data()};
        warp_op_histogram(hist.data());
        if (pass == 0) {
            for (u32 j = 0; j < d.jobs(); ++j)
                RefRun::task(&orr, j);
        } else {
            usize j = 0;
            while (j + 1 < d.jobs() && d.orec[j + 1] <= top)
                ++j;
            warp_run_reference(oi.data() + d.oimg[j], d.cst.data(), ol.data() + d.olane[j], d.recs[top]);
        }
        warp_op_histogram(nullptr);
        printf("nova bench warp: %s's instruction mix:\n", pass == 0 ? "the whole round" : "the longest body");
        u64 lanes_all = 0, seen_all = 0;
        for (u32 k = 0; k <= kOpCount; ++k)
            lanes_all += hist[k];
        for (u32 k = 0; k < kOpCount; ++k)
            seen_all += hist[kOpCount + 1 + k];
        const u64 st = pass == 0 ? steps : longest;
        printf("  %llu lane-instructions (%.1f a warp step), %llu places searched (%.2f a "
               "lane-instruction); by Op (share of lane-instructions, places searched each, share of the searching):\n",
               static_cast<unsigned long long>(lanes_all), st ? static_cast<double>(lanes_all) / st : 0.0,
               static_cast<unsigned long long>(seen_all),
               lanes_all ? static_cast<double>(seen_all) / lanes_all : 0.0);
        for (u32 shown = 0; shown < 18; ++shown) {
            u32 best = kOpCount + 1;
            for (u32 k = 0; k <= kOpCount; ++k)
                if (hist[k] && (best > kOpCount || hist[k] > hist[best]))
                    best = k;
            if (best > kOpCount)
                break;
            const u64 n = hist[best], seen = best < kOpCount ? hist[kOpCount + 1 + best] : 0;
            printf("  %-14s %6.2f%%  %8.2f  %6.2f%%\n", best < kOpCount ? op_name(static_cast<Op>(best)) : "(not its code)",
                   100.0 * n / lanes_all, n ? static_cast<double>(seen) / n : 0.0,
                   seen_all ? 100.0 * seen / seen_all : 0.0);
            hist[best] = 0;
        }
    }
    printf("nova bench warp: %llu records over %u words (the plain kernel), with %.1f%% of the steps\n",
           static_cast<unsigned long long>(big), kLdsWords, steps ? 100.0 * big_steps / steps : 0.0);
    {
        static const u32 kCut[6] = {256, 512, 1024, 2048, 4096, 7680};
        u64 cnt[7] = {}, stp[7] = {};
        for (usize k = 0; k < nrec; ++k) {
            u32 b = 0;
            while (b < 6 && d.words[k] > kCut[b])
                ++b;
            ++cnt[b];
            stp[b] += chain[k];
        }
        printf("nova bench warp: records by size (words: records, share of the steps):");
        for (u32 b = 0; b < 7; ++b) {
            if (b < 6)
                printf(" <=%u: %llu, %.1f%%;", kCut[b], static_cast<unsigned long long>(cnt[b]),
                       steps ? 100.0 * stp[b] / steps : 0.0);
            else
                printf(" more: %llu, %.1f%%\n", static_cast<unsigned long long>(cnt[b]),
                       steps ? 100.0 * stp[b] / steps : 0.0);
        }
    }

    // The longest bodies: size, kernel, place in the dispatch order (records
    // that fit groupshared memory first, in node order, then the rest).
    {
        Array<u64> order; // (chain << 24 | record): the ten longest, longest first
        for (usize k = 0; k < nrec; ++k) {
            const u64 key = chain[k] << 24 | k;
            if (order.size() < 10)
                order.push(key);
            else if (key > order[9])
                order[9] = key;
            else
                continue;
            for (usize i = order.size() - 1; i > 0 && order[i - 1] < order[i]; --i) {
                const u64 t = order[i];
                order[i] = order[i - 1];
                order[i - 1] = t;
            }
        }
        Array<u64> place; // per record: its index in its dispatch
        place.resize(nrec);
        u64 nsmall = 0, nlarge = 0;
        for (usize k = 0; k < nrec; ++k)
            place[k] = d.words[k] <= kLdsWords ? nsmall++ : nlarge++;
        printf("nova bench warp: the longest bodies (chain, record words, warps, kernel, place in its dispatch):\n");
        for (usize i = 0; i < order.size() && i < 10; ++i) {
            const usize k = static_cast<usize>(order[i] & 0xFFFFFFu);
            usize j = 0;
            while (j + 1 < d.jobs() && d.orec[j + 1] <= k)
                ++j;
            const u32 nw = d.img[d.oimg[j] + d.recs[k] + WB_NWARP];
            const bool lds = d.words[k] <= kLdsWords;
            printf("  %6llu steps  %6u words  %3u warps  %s  %llu of %llu\n", static_cast<unsigned long long>(chain[k]),
                   d.words[k], nw, lds ? "groupshared" : "plain      ", static_cast<unsigned long long>(place[k]),
                   static_cast<unsigned long long>(lds ? nsmall : nlarge));
        }
    }

    WarpGpu gpus[kMaxVariants];
    {
        AccelDevice devs[16];
        const u32 n = accel_list(devs, 16);
        u32 pick = 0xFFFFFFFFu;
        for (u32 i = 0; i < n && pick == 0xFFFFFFFFu; ++i)
            if (devs[i].gpu && devs[i].hardware)
                pick = i;
        if (pick == 0xFFFFFFFFu) {
            fprintf(stderr, "nova bench warp: no hardware GPU\n");
            return 1;
        }
        for (u32 v = 0; v < nvar; ++v) {
            double cms = 0;
            if (!gpus[v].open(pick, d.lanes, &err, &cms, &vars[v].opt)) {
                fprintf(stderr, "nova bench warp: %s: %s\n", vars[v].name, err.c_str());
                return 1;
            }
            printf("nova bench warp: variant %s (%s) on %s, ready in %.0f ms: %s\n", vars[v].name, vars[v].what,
                   gpus[v].name(), cms, gpus[v].kernel());
        }
    }
    Array<u32> gpu_img;
    gpu_img.resize_unfilled(d.img.size());

    // The selections.
    const u32 kSweep = 6; // every body up to longest / 2, 4, ... 64
    Pick picks[3 + kSweep];
    u32 npick = 0;
    auto add = [&](const char* name) -> Pick& {
        Pick& p = picks[npick++];
        p.name = name;
        p.on.resize(nrec);
        p.on.fill(0);
        return p;
    };
    {
        Pick& all = add("the whole round");
        all.on.fill(1);
        Pick& alone = add("the longest body alone");
        alone.on[top] = 1;
        Pick& tail = add("bodies over a quarter of the longest");
        for (usize k = 0; k < nrec; ++k)
            tail.on[k] = chain[k] * 4 > longest;
        static const char* const names[kSweep] = {"bodies up to 1/2 of the longest", "bodies up to 1/4",
                                                  "bodies up to 1/8",                "bodies up to 1/16",
                                                  "bodies up to 1/32",               "bodies up to 1/64"};
        for (u32 s = 0; s < kSweep; ++s) {
            Pick& p = add(names[s]);
            const u64 cut = longest >> (s + 1);
            for (usize k = 0; k < nrec; ++k)
                p.on[k] = chain[k] <= cut;
        }
    }
    for (u32 i = 0; i < npick; ++i)
        build_pick(d, chain, &picks[i], gpu_img.data());

    // The check, each variant: the whole round, every image word and every lane it ran.
    for (u32 v = 0; v < nvar; ++v) {
        WarpGpu& gpu = gpus[v];
        double ms = 0;
        if (!run_pick(gpu, d, picks[0], gpu_img.data(), pool, &ms, &err)) {
            fprintf(stderr, "nova bench warp: the GPU failed: %s\n", err.c_str());
            return 1;
        }
        u64 bad_words = 0;
        for (usize i = 0; i < d.img.size(); ++i)
            bad_words += gpu_img[i] != ref_img[i];
        Array<Lane> gpu_lane;
        gpu_lane.assign(d.lane.data(), d.lane.size());
        Array<WarpFetch> fl;
        for (usize j = 0; j < d.jobs(); ++j)
            for (u32 r = 0; r < d.nrec[j]; ++r) {
                const u32* I = d.img.data() + d.oimg[j];
                const u32 rec = d.recs[d.orec[j] + r];
                for (u32 i = 0; i < I[rec + WB_NWARP]; ++i) {
                    const u32 slot = I[rec + WB_WARPS + i * WW_SIZE + WW_LANE0] / d.lanes;
                    fl.push(WarpFetch{d.node[j], slot, gpu_lane.data() + d.olane[j] + static_cast<usize>(slot) * d.lanes});
                }
            }
        if (!gpu.fetch(fl.data(), static_cast<u32>(fl.size()), &err)) {
            fprintf(stderr, "nova bench warp: fetching the lanes failed: %s\n", err.c_str());
            return 1;
        }
        u64 bad_lanes = 0;
        for (usize i = 0; i < d.lane.size(); ++i)
            bad_lanes += memcmp(&gpu_lane[i], &ref_lane[i], sizeof(Lane)) != 0;
        printf("nova bench warp: %s against the reference: %llu image words and %llu lanes differ (%llu warps "
               "fetched)\n",
               vars[v].name, static_cast<unsigned long long>(bad_words), static_cast<unsigned long long>(bad_lanes),
               static_cast<unsigned long long>(fl.size()));
        if (bad_words || bad_lanes) {
            fprintf(stderr, "nova bench warp: %s: the GPU's round differs from the reference\n", vars[v].name);
            return 1;
        }
    }

    // The timings, interleaved: each repeat runs every pick on every variant.
    for (u64 rep = 0; rep < repeats; ++rep)
        for (u32 i = 0; i < npick; ++i)
            for (u32 v = 0; v < nvar; ++v) {
                double ms = 0;
                if (!run_pick(gpus[v], d, picks[i], gpu_img.data(), pool, &ms, &err)) {
                    fprintf(stderr, "nova bench warp: %s: the GPU failed: %s\n", vars[v].name, err.c_str());
                    return 1;
                }
                picks[i].ms[v].push(ms);
            }
    printf("nova bench warp: the kernel on the device's clock, median of %llu (records, warp steps, longest body, "
           "then each variant's kernel ms, and us a step of the longest / million warp steps a second):\n",
           static_cast<unsigned long long>(repeats));
    printf("  %-38s %7s %10s %7s", "", "records", "steps", "longest");
    for (u32 v = 0; v < nvar; ++v)
        printf(" | %-22s", vars[v].name);
    printf("\n");
    for (u32 i = 0; i < npick; ++i) {
        Pick& p = picks[i];
        printf("  %-38s %7llu %10llu %7llu", p.name, static_cast<unsigned long long>(p.nrec),
               static_cast<unsigned long long>(p.steps), static_cast<unsigned long long>(p.longest));
        for (u32 v = 0; v < nvar; ++v) {
            const double ms = median(p.ms[v]);
            printf(" | %7.3f %5.2f/%6.1f", ms, p.longest ? ms * 1000.0 / static_cast<double>(p.longest) : 0.0,
                   ms > 0 ? static_cast<double>(p.steps) / (ms * 1000.0) : 0.0);
        }
        printf("\n");
    }
    return 0;
}
