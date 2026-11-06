// Phase 7b (design.md 6.13): budgets mapped to the machine, the disk tier in
// real files, and the checks that keep a run from swamping the machine.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/accel.h"
#include "nova/budget.h"

using namespace nova;

namespace {

const char* const kDir = "nova_test_disk_files";

// kSoupWorldProc with the disk tier, salvage stamps, volatility and the
// lifecycle (so later lives reuse their files).
void disk_world(TextBuf& t, bool in_files) {
    t.append(kSoupWorldProc);
    t.append(kPhase4All);
    if (in_files) {
        t.append("disk.path = ");
        t.append(kDir);
        t.append_char('\n');
    }
}

usize files_in(const char* dir) {
    char pattern[300];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    usize n = 0;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
            ++n;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

// The same world with its disk tier in RAM and in files: the same hash,
// tick for tick, through collapses, recoveries and a snapshot.
void test_disk_in_files() {
    CreateDirectoryA(kDir, nullptr);
    TextBuf ram_cfg, file_cfg;
    disk_world(ram_cfg, false);
    disk_world(file_cfg, true);
    u64 want = 0;
    {
        Run ram;
        CHECK(make_run(ram, ram_cfg.c_str(), 4));
        ram.run_until(300);
        want = ram.world().hash();
        CHECK(ram.world().collapses_total() > 0 && ram.world().disk_folder().n == 0);
    }
    ByteWriter snap;
    {
        Run file;
        CHECK(make_run(file, file_cfg.c_str(), 4));
        CHECK(file.world().disk_folder().n > 0);
        TextBuf folder;
        folder.append(file.world().disk_folder());
        // four files a node (contents, stamps, owners, the used bitmap)
        CHECK_EQ_U64(files_in(folder.c_str()), 4ull * file.world().node_count());
        file.run_until(150);
        file.snapshot(snap);
        file.run_until(300);
        CHECK_EQ_U64(file.world().hash(), want);
        CHECK(file.world().node_soup(0).disk().mem.in_file());
    }
    CHECK_EQ_U64(files_in(kDir), 0); // the run's folder and files are gone with it
    {
        Run resumed;
        TextBuf err;
        CHECK(resumed.init_from_snapshot(snap.data(), snap.size(), 2, &err));
        CHECK(resumed.world().disk_folder().n > 0);
        resumed.run_until(300);
        CHECK_EQ_U64(resumed.world().hash(), want);
    }
    RemoveDirectoryA(kDir);

    // A disk.path that isn't there is refused.
    TextBuf bad;
    disk_world(bad, false);
    bad.append("disk.path = nova_no_such_folder\\deeper\n");
    Run r;
    Config::ParseError pe;
    CHECK(r.config().parse(bad.view(), &pe));
    TextBuf err;
    CHECK(!r.init(1, &err) && strstr(err.c_str(), "disk.path") != nullptr);
}

void test_amounts() {
    double b = 0;
    CHECK(parse_amount("50%", 200, &b) && b == 100);
    CHECK(parse_amount("2GB", 0, &b) && b == 2147483648.0);
    CHECK(parse_amount("1.5kb", 0, &b) && b == 1536);
    CHECK(parse_amount(" 300 ", 0, &b) && b == 300);
    CHECK(!parse_amount("0%", 100, &b) && !parse_amount("120%", 100, &b) && !parse_amount("abc", 0, &b) &&
          !parse_amount("12PB", 0, &b) && !parse_amount("-5MB", 0, &b));
}

// budget.memory becomes world.soup_size, recorded so the same config rebuilds
// the same world; a world too big for the machine is refused.
void test_memory_budget() {
    TextBuf cfg;
    cfg.append(kSoupWorld1b);
    cfg.append("budget.memory = 64MB\n");
    Run run;
    Config::ParseError pe;
    CHECK(run.config().parse(cfg.view(), &pe));
    TextBuf err;
    CHECK(run.init(2, &err));
    const u64 want_size = static_cast<u64>(64.0 * 1024 * 1024 / kRamPerCodon / 24);
    CHECK_EQ_U64(run.config().get_u64("world.soup_size", 0), want_size);
    CHECK(run.config().get_str("budget.memory", "").eq("off"));
    CHECK(run.config().get_str("budget.memory_was", "").eq("64MB"));
    CHECK(run.world().node_params(0).size == want_size && run.budget_notes().n > 0);
    Str unused;
    CHECK(!run.config().first_unused(&unused)); // the record keys count as read
    run.run_until(20);
    // What a replay log or snapshot carries: the resolved config.
    TextBuf written;
    run.config().write(written);
    Run again;
    CHECK(again.config().parse(written.view(), &pe));
    CHECK(again.init(4, &err));
    again.run_until(20);
    CHECK_EQ_U64(again.world().hash(), run.world().hash());

    // Too little, too much, nonsense.
    const char* bads[] = {"budget.memory = 1KB\n", "budget.memory = 500%\n", "budget.memory = lots\n"};
    for (const char* b : bads) {
        TextBuf c;
        c.append(kSoupWorld1b);
        c.append(b);
        Run r;
        CHECK(r.config().parse(c.view(), &pe));
        TextBuf e;
        CHECK(!r.init(1, &e) && strstr(e.c_str(), "budget.memory") != nullptr);
    }
    // A world that can't fit in RAM is refused before anything is allocated.
    Run huge;
    CHECK(huge.config().parse("world.model = soup\nworld.nodes = 1000000\nworld.soup_size = 16000000\n", &pe));
    TextBuf e;
    CHECK(!huge.init(1, &e) && strstr(e.c_str(), "needs about") != nullptr);
}

void test_disk_budget() {
    CreateDirectoryA(kDir, nullptr);
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append("budget.disk = 8MB\nsalvage.ratio = 4\ndisk.path = ");
    cfg.append(kDir);
    cfg.append_char('\n');
    {
        Run run;
        Config::ParseError pe;
        CHECK(run.config().parse(cfg.view(), &pe));
        TextBuf err;
        CHECK(run.init(2, &err));
        const u64 want = static_cast<u64>(8.0 * 1024 * 1024 / disk_bytes_per_codon(true) / 24);
        CHECK_EQ_U64(run.config().get_u64("world.disk", 0), want);
        CHECK(run.config().get_str("budget.disk_was", "").eq("8MB"));
        CHECK(run.world().node_soup(0).disk().mem.in_file());
        run.run_until(30);
    }
    RemoveDirectoryA(kDir);
    // budget.disk needs disk.path.
    TextBuf c;
    c.append(kSoupWorldProc);
    c.append("budget.disk = 8MB\n");
    Run r;
    Config::ParseError pe;
    CHECK(r.config().parse(c.view(), &pe));
    TextBuf e;
    CHECK(!r.init(1, &e) && strstr(e.c_str(), "disk.path") != nullptr);
}

// budget.gpu and budget.npu: async only; with the devices, a calibration
// sizes gpu.elements and npu.macs.
void test_device_budgets() {
    TextBuf c;
    c.append(kSoupWorldAccel);
    c.append("budget.gpu = 50%\n");
    Run r;
    Config::ParseError pe;
    CHECK(r.config().parse(c.view(), &pe));
    TextBuf e;
    CHECK(!r.init(1, &e) && strstr(e.c_str(), "async") != nullptr);

    AccelDevice devs[16];
    const u32 n = accel_list(devs, 16);
    bool gpu = false;
    for (u32 i = 0; i < n; ++i)
        gpu = gpu || (devs[i].gpu && devs[i].hardware);
    if (!gpu) {
        printf("  budget: no hardware GPU, device budgets skipped\n");
        return;
    }
    TextBuf a;
    a.append(kSoupWorldAccel);
    a.append("accel.mode = async\nbudget.gpu = 50%\nbudget.npu = 25%\n");
    Run run;
    CHECK(run.config().parse(a.view(), &pe));
    TextBuf err;
    CHECK(run.init(4, &err));
    const u64 elements = run.config().get_u64("gpu.elements", 0), macs = run.config().get_u64("npu.macs", 0);
    CHECK(elements >= 1 && macs >= 1 && run.config().get_str("budget.gpu", "").eq("off"));
    CHECK(run.world().soup_params().gpu_elements == elements && run.world().soup_params().npu_macs == macs);
    printf("  budget: %.*s", static_cast<int>(run.budget_notes().n), run.budget_notes().p);
    run.run_until(10);

    // Phase 8: with warps (no async ops needed), budget.gpu sizes
    // warp.capacity from a calibration of the warp kernel on the GPU.
    TextBuf wc;
    wc.append(kSoupWorldGpu);
    wc.append("gpu.warps = true\nbudget.gpu = 50%\n");
    Run wr;
    CHECK(wr.config().parse(wc.view(), &pe));
    TextBuf werr;
    CHECK(wr.init(4, &werr));
    const u64 cap = wr.config().get_u64("warp.capacity", 0);
    CHECK(cap >= 1 && wr.world().soup_params().warp_capacity == cap &&
          wr.config().get_str("budget.gpu_was", "").eq("50%") && strstr(wr.budget_notes().p, "warp work") != nullptr);
    printf("  budget: %.*s", static_cast<int>(wr.budget_notes().n), wr.budget_notes().p);
    wr.run_until(5);
}

} // namespace

void suite_budget() {
    test_amounts();
    test_disk_in_files();
    test_memory_budget();
    test_disk_budget();
    test_device_budgets();
}
