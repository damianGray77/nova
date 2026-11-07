#include "nova/budget.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "nova/accel.h"
#include "nova/accelop.h"
#include "nova/keys.h" // the settings registry
#include "nova/pool.h"
#include "nova/soup.h"

#include "warp_layout.inc" // the warp image's layout (Phase 8)

namespace nova {

u64 memory_available() {
    MEMORYSTATUSEX m = {};
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? m.ullAvailPhys : 0;
}

u64 memory_physical() {
    MEMORYSTATUSEX m = {};
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0;
}

bool volume_free(const char* path, u64* bytes) {
    ULARGE_INTEGER avail = {};
    if (!GetDiskFreeSpaceExA(path, &avail, nullptr, nullptr))
        return false;
    *bytes = avail.QuadPart;
    return true;
}

double disk_bytes_per_codon(bool salvage) { return 1.0 + 4.0 + 0.125 + (salvage ? 4.0 : 0.0); }

bool parse_amount(Str s, double whole, double* bytes) {
    s = s.trim();
    if (s.n == 0)
        return false;
    double v = 0;
    if (s.p[s.n - 1] == '%') {
        if (!parse_f64(s.sub(0, s.n - 1).trim(), &v) || !(v > 0 && v <= 100))
            return false;
        *bytes = whole * v / 100.0;
        return true;
    }
    usize num = s.n;
    while (num > 0 && ((s.p[num - 1] >= 'A' && s.p[num - 1] <= 'Z') || (s.p[num - 1] >= 'a' && s.p[num - 1] <= 'z')))
        --num;
    const Str unit = s.sub(num, s.n - num);
    double mult = 1;
    auto is = [&](const char* u) {
        if (unit.n != strlen(u))
            return false;
        for (usize i = 0; i < unit.n; ++i) {
            const char c = unit.p[i] >= 'a' && unit.p[i] <= 'z' ? static_cast<char>(unit.p[i] - 32) : unit.p[i];
            if (c != u[i])
                return false;
        }
        return true;
    };
    if (unit.n == 0 || is("B"))
        mult = 1;
    else if (is("KB"))
        mult = 1024.0;
    else if (is("MB"))
        mult = 1024.0 * 1024;
    else if (is("GB"))
        mult = 1024.0 * 1024 * 1024;
    else if (is("TB"))
        mult = 1024.0 * 1024 * 1024 * 1024;
    else
        return false;
    if (!parse_f64(s.sub(0, num).trim(), &v) || !(v > 0))
        return false;
    *bytes = v * mult;
    return true;
}

namespace {

void gb(TextBuf& t, double bytes) {
    char b[48];
    snprintf(b, sizeof b, "%.1f GB", bytes / 1073741824.0);
    t.append(b);
}

bool is_off(Str s) { return s.eq("off") || s.eq("0") || s.n == 0; }

// Instructions a second one soup runs on one thread: a quarter-second
// calibration on a busy random soup (the CPU half of the device budgets).
double cpu_rate() {
    Config c;
    Config::ParseError pe;
    c.parse("spark.initial = 400\nspark.rate = 4\nspark.grant = 10\ncpu.wear = 0.001\nspark.timing = last\n"
            "world.millicores = 4000\ncpu.instr_per_core = 25000\n",
            &pe);
    SoupParams* p = new SoupParams;
    if (!soup_params_from_config(c, 16384, p)) {
        delete p;
        return 0;
    }
    Soup* s = new Soup;
    s->init(p, 0x63616C6962ull, 0); // "calib"
    u64 instr = 0, tick = 0;
    const double t0 = clock_seconds();
    double t = 0;
    while (t < 0.25) {
        s->tick(tick++);
        instr += s->tally().instructions;
        t = clock_seconds() - t0;
    }
    delete s;
    delete p;
    return t > 0 ? static_cast<double>(instr) / t : 0;
}

// A batch of accelerator work for calibrating a device: GPU element-ops, or
// NPU multiply-accumulates.
struct CalBatch {
    Array<AccelOp> ops;
    Array<u32> pick;
    Array<u8> in;
    u64 outs = 0, work = 0;
};

void cal_ops(CalBatch* b, bool npu) {
    u32 seed = 12345;
    const u32 gkinds[8] = {kAccCopy, kAccAdd, kAccXor, kAccMul, kAccSub, kAccMin, kAccMax, kAccAnd};
    const u32 count = npu ? 32 : 64;
    for (u32 i = 0; i < count; ++i) {
        AccelOp op;
        memset(&op, 0, sizeof op);
        op.kind = npu ? kAccMatmul : gkinds[i % 8];
        op.n = npu ? 64 : 4096;
        const u32 n = op.n;
        const bool two = !npu && op.kind >= kAccAdd;
        op.src_n = npu ? n * n : n;
        op.dst_n = two ? n : 0;
        op.mat_n = npu ? n * n : 0;
        op.out_n = accel_out_codons(op.kind, n);
        op.key = 7;
        op.in_off = static_cast<u32>(b->in.size());
        for (u32 j = 0; j < op.src_n + op.dst_n + op.mat_n; ++j) {
            seed = seed * 1664525u + 1013904223u;
            b->in.push(static_cast<u8>((seed >> 16) % 64));
        }
        b->pick.push(static_cast<u32>(b->ops.size()));
        b->ops.push(op);
        b->outs += op.out_n;
        b->work += npu ? static_cast<u64>(n) * n * n : n;
    }
}

// Work a second on an open device: a third of a second of repeated batches.
template <class Dev> double dev_rate(Dev& d, bool npu) {
    CalBatch b;
    cal_ops(&b, npu);
    Array<u8> out;
    Array<u32> scal;
    out.resize(static_cast<usize>(b.outs) + 1);
    scal.resize(b.ops.size());
    u32 reps = 0;
    const double t0 = clock_seconds();
    double t = 0;
    do {
        const int tk = d.submit(b.ops.data(), b.pick.data(), static_cast<u32>(b.pick.size()), b.in.data());
        if (tk < 0 || !d.collect(tk, out.data(), scal.data()))
            return 0;
        ++reps;
        t = clock_seconds() - t0;
    } while (t < 0.33 || reps < 3);
    return static_cast<double>(b.work) * reps / t;
}

// Warp work a second on GPU `index` (Phase 8): 4,096 bodies born on the GPU,
// each a warp of `lanes` lanes in a COPY loop where every lane agrees, run
// through the real kernel round after round for a third of a second.
double warp_rate(u32 index, u32 lanes) {
    Config c;
    Config::ParseError pe;
    TextBuf t;
    t.append("world.millicores = 1\nworld.gpus = 4096\ngpu.warps = true\nheat.enabled = false\nhazard.copy = 0\n"
             "hazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\nwarp.capacity = 20000\nwarp.lanes = ");
    t.append_u64(lanes);
    t.append_char('\n');
    if (!c.parse(t.view(), &pe))
        return 0;
    SoupParams* p = new SoupParams;
    const u32 n = 4096, stride = 64;
    if (!soup_params_from_config(c, n * stride, p)) {
        delete p;
        return 0;
    }
    Soup* s = new Soup;
    s->init(p, 0x7761727063616C00ull, 0); // "warpcal"
    Array<u8> code;
    TextBuf why;
    bool ok = genome_from_names(p->table, "NOP1 COPY JMP NOP0", &code, &why);
    for (u32 i = 0; ok && i < n; ++i) {
        for (usize k = 0; k < code.size(); ++k)
            s->mem_mut()[i * stride + k] = code[k];
        ok = s->create_body_form(i * stride, static_cast<u32>(code.size()), 0, 1, kFormGpu) != kNone;
    }
    WarpGpu g;
    double rate = 0;
    if (ok && g.open(index, lanes, &why)) {
        u64 work = 0, tick = 0;
        const double t0 = clock_seconds();
        double el = 0;
        u32 reps = 0;
        Array<u32> all; // the first round sends every warp's lanes; the GPU keeps them after
        for (u32 w = 0; w < n; ++w)
            all.push(w);
        do {
            s->tick_first(tick++);
            if (!s->warp_pack(0))
                break;
            WarpJob j = {0,
                         s->warp_image().data(),
                         static_cast<u32>(s->warp_image().size()),
                         s->warp_records().data(),
                         static_cast<u32>(s->warp_records().size()),
                         s->lanes_mut().data(),
                         static_cast<u32>(s->lanes_mut().size()),
                         all.data(),
                         reps == 0 ? n : 0};
            if (!g.run(s->warp_consts().data(), static_cast<u32>(s->warp_consts().size()), &j, 1, &why))
                break;
            const u32* img = s->warp_image().data();
            for (usize r = 0; r < s->warp_records().size(); ++r) {
                const u32 rec = s->warp_records()[r];
                for (u32 w = 0; w < img[rec + WB_NWARP]; ++w)
                    work += img[rec + WB_WARPS + w * WW_SIZE + WW_USED];
            }
            s->warp_unpack(true);
            s->warp_host(0);
            s->tick_second();
            ++reps;
            el = clock_seconds() - t0;
        } while (el < 0.33 || reps < 3);
        rate = el > 0 ? static_cast<double>(work) / el : 0;
    }
    delete s;
    delete p;
    return rate;
}

} // namespace

bool resolve_budgets(Config& cfg, u32 threads, TextBuf* notes, TextBuf* err) {
    // Settings the world reads (and checks) later are peeked here: the value or
    // the registry's default, no range check (nova/keys.h).
    const u64 nodes = key_peek_u64(cfg, "world.nodes");
    const bool soup = key_peek_str(cfg, "world.model").eq("soup");
    // The record of what was asked is read at the end (it may be written here).
    struct MarkRecord {
        Config& c;
        ~MarkRecord() {
            (void)key_str(c, "budget.memory_was");
            (void)key_str(c, "budget.disk_was");
            (void)key_str(c, "budget.gpu_was");
            (void)key_str(c, "budget.npu_was");
        }
    } mark{cfg};
    auto fail = [&](const char* key, const char* what) {
        err->append(key);
        err->append(": ");
        err->append(what);
        return false;
    };
    char line[320];

    // --- memory -----------------------------------------------------------------
    TextBuf req;
    req.append(key_str(cfg, "budget.memory"));
    if (!is_off(req.view())) {
        if (!soup)
            return fail("budget.memory", "needs world.model = soup");
        const double avail = static_cast<double>(memory_available());
        double bytes = 0;
        if (!parse_amount(req.view(), avail, &bytes))
            return fail("budget.memory", "must be off, a percentage (50%) or an amount (24GB)");
        const double mean = floor(bytes / kRamPerCodon / static_cast<double>(nodes ? nodes : 1));
        if (mean < 64)
            return fail("budget.memory", "too little for world.nodes (under 64 codons a node)");
        if (mean > 16777216.0)
            return fail("budget.memory", "too much for world.nodes (over 2^24 codons a node): add nodes");
        TextBuf v;
        v.append_u64(static_cast<u64>(mean));
        cfg.set("world.soup_size", v.view());
        cfg.set("budget.memory_was", req.view());
        cfg.set("budget.memory", "off");
        TextBuf t;
        t.append("budget.memory ");
        t.append(req.view());
        t.append(" = ");
        gb(t, bytes);
        snprintf(line, sizeof line, " of RAM: world.soup_size = %llu codons a node on average\n",
                 static_cast<unsigned long long>(mean));
        t.append(line);
        notes->append(t.view());
    }

    // --- disk -------------------------------------------------------------------
    TextBuf dpath;
    dpath.append(key_peek_str(cfg, "disk.path"));
    req.clear();
    req.append(key_str(cfg, "budget.disk"));
    // (assumed off when not set, as before the registry: its default is now 4)
    const bool salvage = key_peek_f64_assuming(cfg, "salvage.ratio", 0.0) > 0;
    if (!is_off(req.view())) {
        if (!soup)
            return fail("budget.disk", "needs world.model = soup");
        if (dpath.size() == 0)
            return fail("budget.disk", "needs disk.path: where the disk tier's files go");
        u64 free_b = 0;
        if (!volume_free(dpath.c_str(), &free_b))
            return fail("disk.path", "not a folder that exists");
        double bytes = 0;
        if (!parse_amount(req.view(), static_cast<double>(free_b), &bytes))
            return fail("budget.disk", "must be off, a percentage (20%) or an amount (100GB)");
        double mean = floor(bytes / disk_bytes_per_codon(salvage) / static_cast<double>(nodes ? nodes : 1));
        if (mean < 64)
            return fail("budget.disk", "too little for world.nodes (under 64 codons a node)");
        if (mean > 16777216.0)
            return fail("budget.disk", "too much for world.nodes (over 2^24 codons a node): add nodes");
        TextBuf v;
        v.append_u64(static_cast<u64>(mean));
        cfg.set("world.disk", v.view());
        cfg.set("budget.disk_was", req.view());
        cfg.set("budget.disk", "off");
        TextBuf t;
        t.append("budget.disk ");
        t.append(req.view());
        t.append(" = ");
        gb(t, bytes);
        t.append(" in ");
        t.append(dpath.view());
        snprintf(line, sizeof line, ": world.disk = %llu codons a node on average\n", static_cast<unsigned long long>(mean));
        t.append(line);
        notes->append(t.view());
    }

    // --- the GPU and NPU: sized from a calibration ---------------------------------
    for (u32 dev = 0; dev < 2; ++dev) {
        const char* key = dev == 0 ? "budget.gpu" : "budget.npu";
        req.clear();
        req.append(key_str(cfg, key));
        if (is_off(req.view()))
            continue;
        double pct = 0;
        if (!parse_amount(req.view(), 100.0, &pct) || req.view().p[req.size() - 1] != '%')
            return fail(key, "must be off or a percentage (50%)");
        const bool async = key_peek_str(cfg, "accel.mode").eq("async");
        const bool warps = dev == 0 && key_peek_flag(cfg, "gpu.warps"); // Phase 8: warps run on the real GPU
        if (!async && !warps)
            return fail(key, dev == 0 ? "needs accel.mode = async (the real devices) or gpu.warps = true"
                                      : "needs accel.mode = async (the real devices)");
        const u64 units = key_peek_u64(cfg, dev == 0 ? "world.gpus" : "world.npus");
        if (units == 0)
            return fail(key, dev == 0 ? "needs world.gpus (GPU units a node)" : "needs world.npus (NPU units a node)");
        AccelDevice devs[16];
        const u32 nd = accel_list(devs, 16);
        // The tick rate the CPU allows: a fully busy world's instructions a tick
        // over what this run's threads do a second (about 70% of linear).
        const double cpu = cpu_rate() * (threads ? threads : 1) * 0.7;
        const double millicores = static_cast<double>(key_peek_u64(cfg, "world.millicores"));
        // (assumed the presets' 25,000 when not set, as before the registry: its default is 1,000)
        const double per_core = static_cast<double>(key_peek_u64_assuming(cfg, "cpu.instr_per_core", 25000));
        const double per_tick = static_cast<double>(nodes) * millicores * per_core / 1000.0;
        const double tps = per_tick > 0 && cpu > 0 ? cpu / per_tick : 1.0;
        if (warps) {
            // Phase 8: warp.capacity, so a world whose every GPU unit runs warps
            // flat out keeps the GPU about this busy.
            const u64 lanes = key_peek_u64(cfg, "warp.lanes");
            double wrate = 0;
            const char* wdev = "";
            for (u32 i = 0; i < nd && wrate == 0; ++i) {
                if (devs[i].gpu && devs[i].hardware) {
                    wrate = warp_rate(i, static_cast<u32>(lanes < 1 ? 1 : (lanes > kMaxLanes ? kMaxLanes : lanes)));
                    wdev = devs[i].name;
                }
            }
            if (wrate <= 0)
                return fail(key, "no GPU to calibrate warps on (nova devices lists them)");
            double cap = pct / 100.0 * wrate / tps / static_cast<double>(nodes * units);
            cap = cap < 1 ? 1 : (cap > 1e8 ? 1e8 : floor(cap));
            TextBuf v;
            v.append_u64(static_cast<u64>(cap));
            cfg.set("warp.capacity", v.view());
            snprintf(line, sizeof line,
                     "%s %s: %s runs %.3g warp work a second; at about %.0f ticks a second, warp.capacity = %llu "
                     "a unit a tick\n",
                     key, req.c_str(), wdev, wrate, tps, static_cast<unsigned long long>(cap));
            notes->append(line);
            if (!async) { // no block ops on the real GPU: nothing else to size
                TextBuf was;
                was.append(key);
                was.append("_was");
                cfg.set(was.view(), req.view());
                cfg.set(key, "off");
                continue;
            }
        }
        double rate = 0;
        const char* used = "";
        TextBuf why;
        if (dev == 1) {
            for (u32 i = 0; i < nd && rate == 0; ++i) {
                if (!devs[i].npu || !devs[i].hardware)
                    continue;
                NpuBackend* npu = new NpuBackend;
                if (npu->open(i, &why)) {
                    rate = dev_rate(*npu, true);
                    used = devs[i].name;
                }
                delete npu;
            }
        }
        for (u32 i = 0; i < nd && rate == 0; ++i) { // the GPU (NPU ops go there without an NPU)
            if (!devs[i].gpu || !devs[i].hardware)
                continue;
            GpuBackend* gpu = new GpuBackend;
            if (gpu->open(i, &why)) {
                rate = dev_rate(*gpu, dev == 1);
                used = devs[i].name;
            }
            delete gpu;
        }
        if (rate <= 0)
            return fail(key, "no device to calibrate (nova devices lists them)");
        double each = pct / 100.0 * rate / tps / static_cast<double>(nodes * units);
        each = each < 1 ? 1 : (each > 1e8 ? 1e8 : floor(each));
        const char* target = dev == 0 ? "gpu.elements" : "npu.macs";
        TextBuf v;
        v.append_u64(static_cast<u64>(each));
        cfg.set(target, v.view());
        TextBuf was;
        was.append(key);
        was.append("_was");
        cfg.set(was.view(), req.view());
        cfg.set(key, "off");
        snprintf(line, sizeof line,
                 "%s %s: %s does %.3g %s a second; at about %.0f ticks a second, %s = %llu a unit a tick\n", key,
                 req.c_str(), used, rate, dev == 0 ? "element-ops" : "multiply-adds", tps, target,
                 static_cast<unsigned long long>(each));
        notes->append(line);
    }

    // --- does it fit? (C9) ------------------------------------------------------------
    // (The sums are world_footprint's, which the setup page shows too.)
    if (soup) {
        WorldFootprint fp;
        world_footprint(cfg, &fp);
        const double ram = fp.ram;
        const double avail = static_cast<double>(memory_available());
        if (avail > 0 && ram > 0.9 * avail) {
            TextBuf t;
            t.append("this world needs about ");
            gb(t, ram);
            t.append(" of RAM and ");
            gb(t, avail);
            t.append(" are available: lower world.nodes or world.soup_size, or use budget.memory");
            return fail("world.soup_size", t.c_str());
        }
        if (fp.in_file && fp.disk + fp.archive > 0) {
            u64 free_b = 0;
            if (!volume_free(dpath.c_str(), &free_b))
                return fail("disk.path", "not a folder that exists");
            const double need = fp.disk_file;
            if (need > 0.95 * static_cast<double>(free_b)) {
                TextBuf t;
                t.append("the disk tier needs about ");
                gb(t, need);
                t.append(" and the volume has ");
                gb(t, static_cast<double>(free_b));
                t.append(" free: lower world.disk or world.archive, or use budget.disk");
                return fail("world.disk", t.c_str());
            }
        }
    }
    return true;
}

void world_footprint(Config& cfg, WorldFootprint* out) {
    *out = WorldFootprint{};
    out->soup = key_peek_str(cfg, "world.model").eq("soup");
    out->nodes = static_cast<double>(key_peek_u64(cfg, "world.nodes"));
    out->soup_size = static_cast<double>(key_peek_u64(cfg, "world.soup_size"));
    out->in_file = key_peek_str(cfg, "disk.path").n > 0;
    if (!out->soup)
        return;
    // Disk tiers (docs/world-plan.md W4): the archive shares the disk tier's
    // storage, on top of world.disk (a quarter more by default). (The salvage
    // stamp is assumed off when salvage.ratio isn't set, as before:
    // docs/results/registry.md.)
    const u64 fast = key_peek_u64(cfg, "world.disk");
    out->disk = static_cast<double>(fast);
    out->archive = static_cast<double>(key_u32(cfg, "world.archive", key_default(static_cast<double>(fast / 4))));
    const bool salvage = key_peek_f64_assuming(cfg, "salvage.ratio", 0.0) > 0;
    const double tier = out->nodes * (out->disk + out->archive) * disk_bytes_per_codon(salvage);
    out->ram = out->nodes * out->soup_size * kRamPerCodon + (out->in_file ? 0.0 : tier);
    out->disk_file = out->in_file ? tier : 0.0;
}

} // namespace nova
