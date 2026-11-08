// The warp kernel (Phase 8, design.md 6.14; docs/dxc-plan.md D1): the warp
// interpreter, warp_isa.inc itself (the very source the CPU reference runs as
// C++), between a head that binds its macros to HLSL and a main that picks the
// body record: one thread group per record, one thread per lane.
//
// Two compilers take this file. The Windows SDK's DXC compiles it at build
// time for 32 lanes (core/CMakeLists.txt, embedded in nova.exe); FXC compiles
// it at run time for any other lane count, or when warp.kernel = fxc
// (accel_dx.cpp serves the two includes). Both get W_LANES (lanes a warp) and
// W_LDS (0: the record in video memory; else the most words of a record kept
// in groupshared memory) as defines.

#define INOUT(T) inout T
#if W_LDS
// The body's record and the constants in groupshared memory (a record of at
// most W_LDS words): every access a few cycles instead of a trip to memory.
#define IMG(i) gs_img[(i) - g_rec]
#define CST(i) gs_cst[(i)]
#define ATOMIC_ADD(i, v) InterlockedAdd(gs_img[(i) - g_rec], (v))
#else
#define IMG(i) g_img[g_base + (i)]
#define CST(i) g_cst[(i)]
#define ATOMIC_ADD(i, v) InterlockedAdd(g_img[g_base + (i)], (v))
#endif
#define LANES(i) g_lanes[g_lbase + (i)]
#define SYNC GroupMemoryBarrierWithGroupSync();
#define ONCE_BEGIN if (g_tid == 0u) {
#define ONCE_END }
#define LANE_BEGIN(l0) { uint l = g_tid; {
#define LANE_END } }
#define LANES_LOAD(l0) LaneR L = lane_load((l0) + g_tid);
#define LANES_STORE(l0) lane_store((l0) + g_tid, L);
#define GS_MIN(x, v) InterlockedMin(x, v)
#define GS_ADD(x, v) InterlockedAdd(x, v)
#define GS_MAX(x, v) InterlockedMax(x, v)
#define W_LOOP [loop]
#define W_TRACE(wr, l, step, ip, op, L)
#define LANE_T LaneR
#define WARP_LANE0(wr, wi) IMG((wr) + WW_LANE0)
#define STACK_GET(L, i) g_lanes[g_lidx].stack[(i)]
#define STACK_SET(L, i, v) g_lanes[g_lidx].stack[(i)] = (v)
struct Lane {
    uint ax; uint bx; uint cx; uint dx;
    uint stack[16];
    uint ip; uint rh; uint wh; uint block; uint sp;
    uint err; uint errs; uint halted; uint yielded;
};
struct LaneR {
    uint ax; uint bx; uint cx; uint dx;
    uint ip; uint rh; uint wh; uint block; uint sp;
    uint err; uint errs; uint halted; uint yielded;
};
StructuredBuffer<uint> g_cst : register(t0);
StructuredBuffer<uint4> g_jobs : register(t1);
RWStructuredBuffer<uint> g_img : register(u0);
RWStructuredBuffer<Lane> g_lanes : register(u1);
cbuffer Params : register(b0) { uint g_njobs; uint g_row; uint g_p0; uint g_p1; };
static uint g_base;
static uint g_lbase;
static uint g_tid;
static uint g_rec;
static uint g_lidx; // this thread's lane in g_lanes
static uint g_seen; // this lane's template codons read and places searched this step (Phase 9c)
static uint g_tseen; // memory tiers (W3): ... their time, each at its tier
static uint g_tfast; // ... and its last template's codons in fast memory
// The step's shared work (warp_isa.inc, w_step_begin): thread-private, so
// each thread finds its own, as before the CPU reference shared them.
static uint g_c_skip;
static uint g_c_tn;
static uint g_c_t0;
static uint g_c_t1;
static uint g_c_tfast;
static uint g_c_after;
static uint g_c_sdone;
static uint g_c_to0;
static uint g_c_to1;
static uint g_c_to2;
static uint g_c_seen0;
static uint g_c_seen1;
static uint g_c_seen2;
static uint g_c_tseen0;
static uint g_c_tseen1;
static uint g_c_tseen2;
#if W_LDS
groupshared uint gs_img[W_LDS];
groupshared uint gs_cst[256];
#endif
LaneR lane_load(uint i) {
    LaneR r;
    g_lidx = g_lbase + i;
    r.ax = g_lanes[g_lidx].ax;
    r.bx = g_lanes[g_lidx].bx;
    r.cx = g_lanes[g_lidx].cx;
    r.dx = g_lanes[g_lidx].dx;
    r.ip = g_lanes[g_lidx].ip;
    r.rh = g_lanes[g_lidx].rh;
    r.wh = g_lanes[g_lidx].wh;
    r.block = g_lanes[g_lidx].block;
    r.sp = g_lanes[g_lidx].sp;
    r.err = g_lanes[g_lidx].err;
    r.errs = g_lanes[g_lidx].errs;
    r.halted = g_lanes[g_lidx].halted;
    r.yielded = g_lanes[g_lidx].yielded;
    return r;
}
void lane_store(uint i, LaneR r) {
    uint k = g_lbase + i;
    g_lanes[k].ax = r.ax;
    g_lanes[k].bx = r.bx;
    g_lanes[k].cx = r.cx;
    g_lanes[k].dx = r.dx;
    g_lanes[k].ip = r.ip;
    g_lanes[k].rh = r.rh;
    g_lanes[k].wh = r.wh;
    g_lanes[k].block = r.block;
    g_lanes[k].sp = r.sp;
    g_lanes[k].err = r.err;
    g_lanes[k].errs = r.errs;
    g_lanes[k].halted = r.halted;
    g_lanes[k].yielded = r.yielded;
}
groupshared uint gs_min;
groupshared uint gs_count;
groupshared uint gs_wa[W_LANES];
groupshared uint gs_wv[W_LANES];
groupshared uint gs_ctl[12];

#include "warp_layout.inc"
#include "warp_isa.inc"

[numthreads(W_LANES, 1, 1)]
void main(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
    uint j = g.y * g_row + g.x;
    uint i = 0u;
    if (j >= g_njobs)
        return;
    uint4 job = g_jobs[j];
    g_base = job.x;
    g_lbase = job.z;
    g_tid = t.x;
    g_rec = job.y;
#if W_LDS
    for (i = g_tid; i < job.w; i += W_LANES)
        gs_img[i] = g_img[g_base + job.y + i];
    for (i = g_tid; i < WC_COUNT; i += W_LANES)
        gs_cst[i] = g_cst[i];
    GroupMemoryBarrierWithGroupSync();
#endif
    w_run_body(job.y);
#if W_LDS
    GroupMemoryBarrierWithGroupSync();
    for (i = g_tid; i < job.w; i += W_LANES)
        g_img[g_base + job.y + i] = gs_img[i];
#endif
}
