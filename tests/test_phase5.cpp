// Phase 5 (design.md 6.11): the real GPU and NPU.
//   async    asynchronous accelerator ops on the CPU reference: issue, gather,
//            complete, timing, result slots, ownership, every op against its
//            instant twin, snapshots
//   devices  the GPU and NPU backends against the CPU reference
#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/accel.h"
#include "nova/accelop.h"
#include "nova/rng.h"
#include "nova/snapshot.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

// A quiet soup (1024 codons) with GPU and NPU units, a program at 100 holding
// 500 millicores and units of both, in instant or async mode.
struct AccFix {
    SoupParams p;
    Soup s;
    u32 b = kNone, c = kNone;
    bool init(bool async, const char* prog = "GPU INC INC INC INC INC INC INC") {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 1000\nworld.gpus = 50\nworld.npus = 50\n");
        cfg.append("gpu.elements = 4096\nnpu.macs = 300000\n"); // the same capacity both ways
        cfg.append(async ? "accel.mode = async\naccel.latency = 1\n" : "");
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        s.init(&p, 11, 0); // the same random memory both ways
        b = plant(s, 100, prog, 500);
        if (b == kNone)
            return false;
        c = s.body(b).cpus;
        s.begin_tick(0);
        s.claim_gpus(b, 20);
        s.claim_npus(b, 20);
        s.prepare_gpu();
        return true;
    }
    Cpu& cpu() { return s.cpu_mut(c); }
    void regs(u32 ip, u32 ax, u32 bx, u32 cx, u32 rh, u32 wh) {
        Cpu& k = cpu();
        k.ip = ip, k.ax = ax, k.bx = bx, k.cx = cx, k.rh = rh, k.wh = wh;
    }
    // Async: gather this soup's issued ops and complete them all now.
    void settle() {
        Array<AccelOp> ops;
        Array<u8> in, out;
        Array<u32> addr;
        s.accel_gather(&ops, &in, &addr);
        for (usize i = 0; i < ops.size(); ++i) {
            out.resize(ops[i].out_n);
            const u32 r = accel_reference(ops[i], in.data() + ops[i].in_off, out.data());
            s.accel_complete(ops[i], out.data(), r, addr.data() + ops[i].addr_off);
        }
    }
};

} // namespace

void suite_async() {
    // Issue: checked, sized, charged and queued; nothing written until the
    // barrier's completion. Results land in the CPU's slot (SENSE 18), ops
    // pending count down (SENSE 19).
    {
        AccFix f;
        CHECK(f.init(true, "GPU SENSE ALLOC INC INC INC INC INC"));
        CHECK(f.p.sel.sense_n == 20);
        f.regs(102, 0, 0, 64, 0, 0); // ALLOC 64 codons: the write head goes to them
        f.s.run_cpu(f.c, 1);
        const u32 dst = f.cpu().wh;
        u8 before[64];
        memcpy(before, f.s.mem() + dst, 64);
        f.regs(100, 0, kAccCopy, 8, 100, dst); // copy my 8 codons into the block
        const i64 cost = f.s.run_cpu(f.c, 1);
        CHECK(cost == 32); // the high-volume profile's setup: 32 + 8/1024
        CHECK(f.cpu().err == 0 && f.cpu().cx == 0 && f.cpu().rh == 100 && f.cpu().wh == dst + 8);
        CHECK(f.s.accel_issued() == 1 && memcmp(f.s.mem() + dst, before, 64) == 0);
        f.regs(101, 0, 19, 0, 0, 0); // SENSE 19: one op pending
        f.s.run_cpu(f.c, 1);
        CHECK(f.cpu().cx == 1);
        f.settle();
        CHECK(memcmp(f.s.mem() + dst, f.s.mem() + 100, 8) == 0 && f.s.accel_pending(f.c) == 0);
        // A reduction's result goes to the slot.
        f.regs(100, f.s.mem()[100] & 63, kAccCount, 8, 100, dst);
        f.s.run_cpu(f.c, 1);
        f.settle();
        u32 expect = 0;
        for (u32 i = 0; i < 8; ++i)
            expect += (f.s.mem()[100 + i] & 63) == (f.s.mem()[100] & 63);
        f.regs(101, 0, 18, 0, 0, 0);
        f.s.run_cpu(f.c, 1);
        CHECK(f.cpu().cx == expect && expect >= 1);
        CHECK(soup_ok(f.s));
    }
    // Every op, instant against async from identical soups: the same memory
    // and the same result (CX in instant mode, the slot in async mode).
    {
        u32 ops_ok = 0, ops_run = 0;
        for (u32 kind = 0; kind <= kAccArgmax; ++kind) {
            AccFix a, b;
            CHECK(a.init(false, "GPU SENSE ALLOC INC INC INC INC INC"));
            CHECK(b.init(true, "GPU SENSE ALLOC INC INC INC INC INC"));
            // Both claim the same 512-codon block for operands and results.
            a.regs(102, 0, 0, 512, 0, 0);
            a.s.run_cpu(a.c, 1);
            b.regs(102, 0, 0, 512, 0, 0);
            b.s.run_cpu(b.c, 1);
            const u32 blk = a.cpu().wh;
            CHECK(blk == b.cpu().wh);
            // n: elements, or k for the NPU (matvec 8: M 64 codons; matmul 4).
            u32 n = 24, rh = blk, wh = blk + 256, ax = blk + 100;
            if (kind == kAccMatvec)
                n = 8;
            if (kind == kAccMatmul)
                n = 4;
            if (kind == kAccCount || kind == kAccFind || kind == kAccFill)
                ax = a.s.mem()[blk + 7] & 63; // a key that occurs
            const u32 bx = kind < kAccDot ? kind : 16 + (kind - kAccDot);
            a.regs(100, ax, bx, n, rh, wh);
            b.regs(100, ax, bx, n, rh, wh);
            a.s.run_cpu(a.c, 1);
            b.s.run_cpu(b.c, 1);
            b.settle();
            ++ops_run;
            const bool scalar = accel_has_scalar(kind);
            b.regs(101, 0, 18, 0, 0, 0);
            b.s.run_cpu(b.c, 1);
            const bool same_mem = memcmp(a.s.mem() + blk, b.s.mem() + blk, 512) == 0;
            const bool same_result = !scalar || a.cpu().cx == b.cpu().cx;
            if (a.cpu().err == 0 && same_mem && same_result)
                ++ops_ok;
            else
                fprintf(stderr, "  async: kind %u differs (instant err %u, memory %s, result %u vs %u)\n", kind,
                        a.cpu().err, same_mem ? "same" : "differs", a.cpu().cx, b.cpu().cx);
        }
        CHECK(ops_ok == ops_run && ops_run == 20);
    }
    // Ownership: an op's writes land only where the issuer still owns them,
    // and a result for a CPU slot that changed hands is dropped.
    {
        AccFix f;
        CHECK(f.init(true, "GPU SENSE ALLOC INC INC INC INC INC"));
        f.regs(102, 0, 0, 64, 0, 0);
        f.s.run_cpu(f.c, 1);
        const u32 dst = f.cpu().wh;
        f.regs(100, 9, kAccFill, 16, 100, dst);
        f.s.run_cpu(f.c, 1);
        // Before the barrier the block goes away (RELEASE-like: kill the body).
        u8 before[16];
        memcpy(before, f.s.mem() + dst, 16);
        for (u32 c = f.s.body(f.b).cpus; c != kNone; c = f.s.cpu(c).next)
            f.s.cpu_mut(c).halted = 1;
        Array<AccelOp> ops;
        Array<u8> in, out;
        Array<u32> addr;
        f.s.accel_gather(&ops, &in, &addr);
        f.s.death_sweep();
        out.resize(ops[0].out_n);
        const u32 r = accel_reference(ops[0], in.data() + ops[0].in_off, out.data());
        f.s.accel_complete(ops[0], out.data(), r, addr.data() + ops[0].addr_off);
        CHECK(memcmp(f.s.mem() + dst, before, 16) == 0); // nobody owns it now: not written
        CHECK(soup_ok(f.s));
    }
    // In a world: results land accel.latency - 1 barriers after the issue.
    // The program claims 8 codons, issues a GPU fill of them with codon 0, and
    // then idles (YIELD in a loop), staying alive.
    for (u32 latency = 1; latency <= 3; ++latency) {
        TextBuf cfg;
        cfg.append("seed = 5\nworld.model = soup\nworld.nodes = 1\nworld.soup_size = 1024\nworld.millicores = 1000\n"
                   "world.gpus = 50\nseed.gpus = 10\nseed.at = 100\nseed.millicores = 500\naccel.mode = async\n");
        cfg.append(kQuiet);
        cfg.append("seed.genome = ZERO INC SHL SHL SHL ALLOC ZERO INC INC INC INC INC ROTB ZERO INC SHL SHL SHL GPU "
                   "NOP1 NOP0 YIELD JMP NOP0 NOP1\n");
        cfg.append("accel.latency = ");
        cfg.append_u64(latency);
        cfg.append_char('\n');
        Run run;
        CHECK(make_run(run, cfg.c_str(), 1));
        const Soup& s = run.world().node_soup(0);
        u32 filled_at = 0;
        for (u32 t = 0; t < 6 && !filled_at; ++t) {
            run.step();
            const u32 body = s.body_at(0);
            const u32 blk = s.block(s.body(body).blocks).next;
            if (blk == kNone)
                continue;
            bool all = true;
            for (u32 i = 0; i < 8; ++i)
                all = all && s.mem()[s.block(blk).start + i] == 0;
            if (all)
                filled_at = t + 1; // steps taken
        }
        CHECK(filled_at == latency); // issued in tick 0, landed at the barrier ending tick latency - 1
    }
    // Snapshot resume with ops in flight, and thread counts, give the same world.
    {
        TextBuf cfg;
        cfg.append(kSoupWorldAccel);
        cfg.append("accel.mode = async\naccel.latency = 3\n");
        Run straight;
        CHECK(make_run(straight, cfg.c_str(), 1));
        straight.run_until(80);
        Run first;
        CHECK(make_run(first, cfg.c_str(), 20));
        u64 elements = 0, macs = 0;
        for (u32 t = 0; t < 79; ++t) { // stop at a tick with ops in flight
            first.step();
            elements += first.world().last_soup_tick().gpu_elements;
            macs += first.world().last_soup_tick().npu_macs;
            if (t >= 20 && first.world().accel_in_flight() > 0)
                break;
        }
        printf("  async: %llu GPU element-ops and %llu NPU MACs issued by tick %llu\n",
               static_cast<unsigned long long>(elements), static_cast<unsigned long long>(macs),
               static_cast<unsigned long long>(first.world().tick()));
        CHECK(first.world().accel_in_flight() > 0);
        ByteWriter bytes;
        first.snapshot(bytes);
        Run resumed;
        TextBuf err;
        CHECK(resumed.init_from_snapshot(bytes.data(), bytes.size(), 4, &err));
        CHECK_EQ_U64(resumed.world().hash(), first.world().hash());
        resumed.run_until(80);
        CHECK_EQ_U64(resumed.world().hash(), straight.world().hash());
        printf("  async: GPU + NPU world, latency 3: snapshot with %llu ops in flight resumes exactly\n",
               static_cast<unsigned long long>(first.world().accel_in_flight()));
    }
}

namespace {

// A random batch: `count` ops of random kinds and sizes (from tiny to the
// high-volume limits), with random codons as inputs.
void random_batch(Rng& r, u32 count, Array<AccelOp>* ops, Array<u8>* in) {
    ops->clear();
    in->clear();
    for (u32 i = 0; i < count; ++i) {
        AccelOp op;
        memset(&op, 0, sizeof op);
        op.kind = static_cast<u32>(r.next() % (kAccArgmax + 1));
        const u32 big = r.next() % 4 == 0;
        u32 n = big ? 1 + static_cast<u32>(r.next() % 6000) : static_cast<u32>(r.next() % 40);
        if (op.kind == kAccMatvec)
            n = 1 + static_cast<u32>(r.next() % (big ? 256 : 16));
        if (op.kind == kAccMatmul)
            n = 1 + static_cast<u32>(r.next() % (big ? 64 : 8));
        op.n = n;
        op.key = static_cast<u32>(r.next() % 64);
        const bool two = op.kind == kAccCompare || (op.kind >= kAccAdd && op.kind <= kAccDot);
        op.src_n = op.kind == kAccFill ? 0 : (op.kind == kAccMatmul ? n * n : n);
        op.dst_n = two ? n : 0;
        op.mat_n = op.kind >= kAccMatvec && op.kind <= kAccMatmul ? n * n : 0;
        op.out_n = accel_out_codons(op.kind, n);
        op.in_off = static_cast<u32>(in->size());
        const u32 reads = op.src_n + op.dst_n + op.mat_n;
        // Mostly uniform codons; some ops with few distinct values, so count,
        // find and argmax meet ties and repeats.
        const u32 spread = r.next() % 3 == 0 ? 3 : 64;
        for (u32 j = 0; j < reads; ++j)
            in->push(static_cast<u8>(r.next() % spread));
        ops->push(op);
    }
}

} // namespace

// The GPU backend against the CPU reference: every op kind on random batches,
// bit for bit, and whole async worlds whose hash doesn't depend on where the
// ops ran. Skipped when the machine has no hardware GPU.
void suite_devices() {
    AccelDevice devs[16];
    const u32 nd = accel_list(devs, 16);
    u32 gpu = kNone;
    for (u32 i = 0; i < nd && gpu == kNone; ++i)
        if (devs[i].gpu && devs[i].hardware)
            gpu = i;
    if (gpu == kNone) {
        printf("  devices: no hardware GPU, skipped\n");
        return;
    }
    {
        GpuBackend b;
        TextBuf err;
        CHECK(b.open(gpu, &err));
        if (!b.is_open()) {
            fprintf(stderr, "  devices: %s\n", err.c_str());
            return;
        }
        Rng r(77, 0, 0);
        Array<AccelOp> ops;
        Array<u8> in, got, want;
        Array<u32> pick, scal;
        u32 batches_ok = 0, ops_checked = 0, wrong = 0;
        const u32 kBatches = 12;
        for (u32 bi = 0; bi < kBatches; ++bi) {
            random_batch(r, bi == 0 ? 1 : 40 + bi * 20, &ops, &in);
            pick.clear();
            u32 out_total = 0;
            for (usize i = 0; i < ops.size(); ++i) {
                pick.push(static_cast<u32>(i));
                out_total += ops[i].out_n;
            }
            got.resize(out_total ? out_total : 1);
            scal.resize(ops.size());
            const int t = b.submit(ops.data(), pick.data(), static_cast<u32>(pick.size()), in.data());
            CHECK(t >= 0);
            if (t < 0 || !b.collect(t, got.data(), scal.data()))
                continue;
            u32 off = 0;
            bool all = true;
            for (usize i = 0; i < ops.size(); ++i) {
                const AccelOp& op = ops[i];
                want.resize(op.out_n ? op.out_n : 1);
                const u32 s = accel_reference(op, in.data() + op.in_off, want.data());
                const bool same = (!accel_has_scalar(op.kind) || s == scal[i]) &&
                                  (op.out_n == 0 || memcmp(want.data(), got.data() + off, op.out_n) == 0);
                if (!same) {
                    if (wrong < 5)
                        fprintf(stderr, "  devices: batch %u op %u kind %u n %u differs (scalar %u vs %u)\n", bi,
                                static_cast<u32>(i), op.kind, op.n, scal[i], s);
                    ++wrong;
                    all = false;
                }
                ++ops_checked;
                off += op.out_n;
            }
            batches_ok += all;
        }
        CHECK(batches_ok == kBatches && wrong == 0);
        // Several batches in flight at once, collected out of order.
        int tk[3];
        Array<u8> ins[3];
        Array<AccelOp> opss[3];
        for (u32 k = 0; k < 3; ++k) {
            random_batch(r, 30, &opss[k], &ins[k]);
            pick.clear();
            for (u32 i = 0; i < 30; ++i)
                pick.push(i);
            tk[k] = b.submit(opss[k].data(), pick.data(), 30, ins[k].data());
            CHECK(tk[k] >= 0);
        }
        bool inflight_ok = true;
        for (u32 kk = 0; kk < 3; ++kk) {
            const u32 k = 2 - kk;
            u32 out_total = 0;
            for (u32 i = 0; i < 30; ++i)
                out_total += opss[k][i].out_n;
            got.resize(out_total ? out_total : 1);
            scal.resize(30);
            inflight_ok = inflight_ok && b.collect(tk[k], got.data(), scal.data());
            u32 off = 0;
            for (u32 i = 0; i < 30; ++i) {
                const AccelOp& op = opss[k][i];
                want.resize(op.out_n ? op.out_n : 1);
                const u32 s = accel_reference(op, ins[k].data() + op.in_off, want.data());
                inflight_ok = inflight_ok && (!accel_has_scalar(op.kind) || s == scal[i]) &&
                              (op.out_n == 0 || memcmp(want.data(), got.data() + off, op.out_n) == 0);
                off += op.out_n;
            }
        }
        CHECK(inflight_ok);
        printf("  devices: %s: %u random ops in %u batches, and 3 batches in flight at once, match the reference\n",
               b.name(), ops_checked, kBatches);
    }
    // The NPU backend (its open runs its own known-answer self-test), on
    // random batches of its kinds.
    u32 npu = kNone;
    for (u32 i = 0; i < nd && npu == kNone; ++i)
        if (devs[i].npu && devs[i].hardware)
            npu = i;
    if (npu == kNone) {
        printf("  devices: no NPU, its backend skipped\n");
    } else {
        NpuBackend b;
        TextBuf err;
        double ms = 0;
        const bool opened = b.open(npu, &err, &ms);
        if (!opened)
            fprintf(stderr, "  devices: NPU: %s\n", err.c_str());
        CHECK(opened);
        Rng r(91, 0, 0);
        Array<AccelOp> all, ops;
        Array<u8> in, got, want;
        Array<u32> pick, scal;
        u32 checked = 0, wrong = 0;
        for (u32 bi = 0; opened && bi < 6; ++bi) {
            random_batch(r, 60 + bi * 40, &all, &in);
            ops.clear();
            for (usize i = 0; i < all.size(); ++i)
                if (accel_is_npu(all[i].kind))
                    ops.push(all[i]);
            pick.clear();
            u32 outs = 0;
            for (usize i = 0; i < ops.size(); ++i) {
                pick.push(static_cast<u32>(i));
                outs += ops[i].out_n;
            }
            got.resize(outs ? outs : 1);
            scal.resize(ops.size() ? ops.size() : 1);
            const int t = b.submit(ops.data(), pick.data(), static_cast<u32>(pick.size()), in.data());
            CHECK(t >= 0);
            if (t < 0 || !b.collect(t, got.data(), scal.data()))
                continue;
            u32 off = 0;
            for (usize i = 0; i < ops.size(); ++i) {
                const AccelOp& op = ops[i];
                want.resize(op.out_n ? op.out_n : 1);
                const u32 s = accel_reference(op, in.data() + op.in_off, want.data());
                const bool same = (!accel_has_scalar(op.kind) || s == scal[i]) &&
                                  (op.out_n == 0 || memcmp(want.data(), got.data() + off, op.out_n) == 0);
                if (!same && wrong++ < 5)
                    fprintf(stderr, "  devices: NPU batch %u op %u kind %u n %u differs (scalar %u vs %u)\n", bi,
                            static_cast<u32>(i), op.kind, op.n, scal[i], s);
                ++checked;
                off += op.out_n;
            }
        }
        CHECK(wrong == 0 && checked > 0);
        printf("  devices: %s: self-test passed (start-up %.0f ms); %u random NPU ops match the reference\n",
               b.name(), ms, checked);
    }
    // Whole worlds: the same hash with the ops on the CPU reference, on the
    // GPU and NPU (threads 20), on the GPU alone, and with every batch
    // verified, at latency 1 and 2.
    for (u32 latency = 1; latency <= 2; ++latency) {
        u64 hashes[4] = {};
        u64 device_ops = 0, npu_ops = 0, verified = 0, mismatches = 0;
        const char* where[4] = {"accel.gpu = cpu\naccel.npu = cpu\n", "accel.gpu = auto\n",
                                "accel.gpu = auto\naccel.npu = gpu\n", "accel.gpu = auto\naccel.verify = 1\n"};
        for (u32 w = 0; w < 4; ++w) {
            TextBuf cfg;
            cfg.append(kSoupWorldAccel);
            cfg.append("accel.mode = async\naccel.latency = ");
            cfg.append_u64(latency);
            cfg.append_char('\n');
            cfg.append(where[w]);
            Run run;
            CHECK(make_run(run, cfg.c_str(), w == 1 ? 20 : 1));
            run.run_until(120);
            hashes[w] = run.world().hash();
            if (w == 1) {
                device_ops = run.world().accel_device_ops();
                npu_ops = run.world().accel_npu_ops();
            }
            if (w == 3) {
                verified = run.world().accel_verified();
                mismatches = run.world().accel_mismatches();
            }
            if (run.world().accel_notes().n)
                fprintf(stderr, "  devices: notes: %.*s", static_cast<int>(run.world().accel_notes().n),
                        run.world().accel_notes().p);
        }
        CHECK_EQ_U64(hashes[1], hashes[0]);
        CHECK_EQ_U64(hashes[2], hashes[0]);
        CHECK_EQ_U64(hashes[3], hashes[0]);
        CHECK(device_ops > 0 && verified > 0 && mismatches == 0);
        CHECK(npu == kNone || npu_ops > 0);
        printf("  devices: async world, latency %u, 120 ticks: %llu ops on devices (%llu on the NPU), %llu verified, "
               "same hash on the CPU, GPU + NPU and GPU alone\n",
               latency, static_cast<unsigned long long>(device_ops), static_cast<unsigned long long>(npu_ops),
               static_cast<unsigned long long>(verified));
    }
}
