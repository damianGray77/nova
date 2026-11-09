#pragma once

// Shared test helpers: a busy demo world and a one-call Run setup.
#include <cstdio>

#include "nova/run.h"

// A small world tuned so that every order-sensitive path fires: outboxes fill
// (refusals), destinations fill (bounces), and codons get overwritten on arrival.
inline const char* const kBusyWorld = "seed = 42\n"
                                      "world.nodes = 200\n"
                                      "world.chords = 100\n"
                                      "world.soup_size = 1024\n"
                                      "world.pool_capacity = 1000\n"
                                      "world.pool_initial = 600\n"
                                      "world.outbox_capacity = 2\n"
                                      "demo.send_rate = 0.7\n"
                                      "demo.sends_per_tick = 6\n"
                                      "demo.mutations_per_tick = 32\n"
                                      "demo.max_parcel = 250\n";

// The Phase 1b rule changes switched back to Phase 1a: no memory upkeep, and
// INJECT's 0/0 = 1 contest and truncated payloads. Appended to a config, it
// reproduces Phase 1a exactly.
inline const char* const kPhase1aRules = "mem.upkeep = 0\n"
                                         "contact.inject_room = 0\n";

// A busy soup world: random sparks, a seeded sketch replicator on every node,
// hazards well above baseline and heat gains high enough that units and pages
// get released. Every path of the Phase 1a interpreter fires in it under the
// Phase 1a rules (add kPhase1aRules), so the determinism gate is not vacuous.
// Under the Phase 1b defaults its 8-millicore sparks cannot pay their upkeep;
// kSoupWorld1b is the busy world for those rules. 96 nodes x at most 200
// instructions per tick.
inline const char* const kSoupWorld = "seed = 42\n"
                                      "world.model = soup\n"
                                      "world.nodes = 96\n"
                                      "world.chords = 48\n"
                                      "world.soup_size = 2048\n"
                                      "world.millicores = 400\n"
                                      "cpu.instr_per_core = 500\n"
                                      "spark.initial = 30\n"
                                      "spark.rate = 0.5\n"
                                      "spark.grant = 8\n"
                                      "spark.min_len = 8\n"
                                      "spark.max_len = 48\n"
                                      "seed.genome = NOP0 SENSE ALLOC NOP1 COPY DJNZ NOP0 INC DIVIDE NOP0\n"
                                      "seed.at = 1000\n"
                                      "seed.millicores = 200\n"
                                      "hazard.copy = 0.005\n"
                                      "hazard.flaw = 0.001\n"
                                      "hazard.rot = 0.00005\n"
                                      "heat.cpu_gain = 36\n"
                                      "heat.fetch_gain = 0.06\n"
                                      // (1.5, was 0.5: with INJECT landing only through wounds
                                      // (docs/bites-plan.md) there are fewer writes, and pages
                                      // must still get hot enough to let codons go)
                                      "heat.write_gain = 1.5\n";

// A busy soup world under the Phase 1b rules: memory upkeep (the default), CPU
// wear, sparks last, the doubled-invasion table, and on every node a seeded
// COPY replicator sealed by NOPs. Sparks are funded well enough to pay their
// upkeep, and hazards and heat run hot as in kSoupWorld. (Its gate events came
// from INJECT copies; INJECT moves a program now, docs/inject-plan.md, and at
// this heat the COPY replicator doesn't establish.) 24 nodes x at most 1000
// instructions per tick.
inline const char* const kSoupWorld1b = "seed = 42\n"
                                        "world.model = soup\n"
                                        "world.nodes = 24\n"
                                        "world.chords = 12\n"
                                        "world.soup_size = 2048\n"
                                        "world.millicores = 400\n"
                                        "cpu.instr_per_core = 2500\n"
                                        "cpu.wear = 0.002\n"
                                        "isa.table = nova_v1_inv2\n"
                                        "spark.timing = last\n"
                                        "spark.initial = 20\n"
                                        "spark.rate = 0.5\n"
                                        "spark.grant = 20\n"
                                        "spark.min_len = 8\n"
                                        "spark.max_len = 24\n"
                                        // (a COPY replicator sealed by NOPs: open ends bleed, and the keyed
                                        // INJECT the 1b ancestor re-infected its relatives with is gone,
                                        // INJECT landing only through wounds now: docs/bites-plan.md)
                                        "seed.genome = NOP0 ZERO ROTB SENSE ALLOC NOP1 COPY DJNZ NOP0 ZERO INC "
                                        "ROTB SENSE DIVIDE NOP0\n"
                                        "seed.at = 1000\n"
                                        "seed.millicores = 200\n"
                                        "hazard.copy = 0.005\n"
                                        "hazard.flaw = 0.001\n"
                                        "hazard.rot = 0.00005\n"
                                        "heat.cpu_gain = 36\n"
                                        "heat.fetch_gain = 0.06\n"
                                        "heat.write_gain = 0.5\n";

// A hand-written GPU replicator (a test, never seeded in a trial; give it GPU
// units with seed.gpus): it ALLOCs a block its own size, GPU-copies itself
// into it (op 4, copy) and divides with a share of its length / 64, which
// passes on that share of its millicores and GPU units. It never claims GPU
// units, so a lineage's GPU thins out over the generations.
inline const char* const kGpuReplicator =
    "SENSE PUSH ALLOC ZERO INC SHL SHL ROTB POP GPU ZERO ROTB SENSE DIVIDE";

// A hand-written COPY replicator that also uses the NPU (a test; give it NPU
// units with seed.npus): each cycle it takes the dot product of its own code
// with its fresh daughter block (accelerator BX 16, n = its length, so both
// heads come back round), then copies itself with a COPY loop and divides.
inline const char* const kNpuReplicator =
    "SENSE PUSH ALLOC ZERO INC SHL SHL SHL SHL ROTB POP GPU ZERO ROTB SENSE NOP1 COPY DJNZ NOP0 "
    "SENSE DIVIDE";

// A busy soup world with the in-world GPU (Phase 2): kSoupWorld1b's rules
// (default heat gains, so the seeded replicator isn't cooked) plus 200 GPU
// units per node with wear, and the GPU replicator seeded on every node, so
// GPU claims, copies, heat, DIVIDE shares and the gpu_replicator event all
// happen. 24 nodes.
inline const char* const kSoupWorldGpu = "seed = 42\n"
                                         "world.model = soup\n"
                                         "world.nodes = 24\n"
                                         "world.chords = 12\n"
                                         "world.soup_size = 2048\n"
                                         "world.millicores = 400\n"
                                         "world.gpus = 200\n"
                                         "gpu.wear = 0.002\n"
                                         "cpu.instr_per_core = 2500\n"
                                         "cpu.wear = 0.002\n"
                                         "spark.timing = last\n"
                                         "spark.initial = 20\n"
                                         "spark.rate = 0.5\n"
                                         "spark.grant = 20\n"
                                         "spark.min_len = 8\n"
                                         "spark.max_len = 24\n"
                                         "seed.genome = SENSE PUSH ALLOC ZERO INC SHL SHL ROTB POP "
                                         "GPU ZERO ROTB SENSE DIVIDE\n"
                                         "seed.at = 1000\n"
                                         "seed.millicores = 40\n"
                                         "seed.gpus = 100\n"
                                         "hazard.copy = 0.005\n"
                                         "hazard.flaw = 0.001\n"
                                         "hazard.rot = 0.00005\n";

// The same with both accelerators (Phase 2b): 200 GPU and 200 NPU units per
// node, with wear, and the NPU replicator seeded on every node, so NPU claims,
// dot products, heat, shares and the npu_user event all happen. 24 nodes.
inline const char* const kSoupWorldAccel = "seed = 42\n"
                                           "world.model = soup\n"
                                           "world.nodes = 24\n"
                                           "world.chords = 12\n"
                                           "world.soup_size = 2048\n"
                                           "world.millicores = 400\n"
                                           "world.gpus = 200\n"
                                           "world.npus = 200\n"
                                           "gpu.wear = 0.002\n"
                                           "npu.wear = 0.002\n"
                                           "cpu.instr_per_core = 2500\n"
                                           "cpu.wear = 0.002\n"
                                           "spark.timing = last\n"
                                           "spark.initial = 20\n"
                                           "spark.rate = 0.5\n"
                                           "spark.grant = 20\n"
                                           "spark.min_len = 8\n"
                                           "spark.max_len = 24\n"
                                           "seed.genome = NOP0 SENSE PUSH ALLOC ZERO INC SHL SHL SHL SHL ROTB "
                                           "POP GPU ZERO ROTB SENSE NOP1 COPY DJNZ NOP0 SENSE DIVIDE NOP0\n"
                                           "seed.at = 1000\n"
                                           "seed.millicores = 160\n"
                                           "seed.npus = 100\n"
                                           "hazard.copy = 0.005\n"
                                           "hazard.flaw = 0.001\n"
                                           "hazard.rot = 0.00005\n";

// A busy procedural world (Phase 3): 24 nodes from the generator, every
// property varied over its noise field, dispersal on, kSoupWorld1b's rules
// with default heat, sparks everywhere, and on every node a seeded replicator
// that copies itself and DIVIDEs to its nearest neighbour -- so departures,
// transit, arrivals, full edges and colonist events all happen.
inline const char* const kSoupWorldProc = "seed = 42\n"
                                          "world.model = soup\n"
                                          "world.graph = procedural\n"
                                          "world.nodes = 24\n"
                                          "world.soup_size = 2048\n"
                                          "world.millicores = 600\n"
                                          "vary.soup_size = 0.5\n"
                                          "vary.millicores = 0.5\n"
                                          "vary.cool = 0.5\n"
                                          "vary.spark_rate = 0.5\n"
                                          "vary.radiation = 0.5\n"
                                          "edge.bandwidth = 8\n"
                                          "cpu.instr_per_core = 2500\n"
                                          "cpu.wear = 0.002\n"
                                          "spark.timing = last\n"
                                          "spark.initial = 20\n"
                                          "spark.rate = 0.5\n"
                                          "spark.grant = 20\n"
                                          "spark.min_len = 8\n"
                                          "spark.max_len = 24\n"
                                          "seed.genome = NOP0 ZERO ROTB SENSE ALLOC NOP1 COPY DJNZ NOP0 ZERO INC "
                                          "ROTB SENSE DIVIDE NOP0\n" // (NOP caps: open ends bleed, bites plan B6)
                                          "seed.at = 600\n"
                                          "seed.millicores = 200\n"
                                          "hazard.copy = 0.005\n"
                                          "hazard.flaw = 0.001\n"
                                          "hazard.rot = 0.00005\n";

// The procedural world with every Phase 4 feature on, at rates high enough
// that each fires within a 300-tick gate run: the ambient leak, transit
// corruption and link failures, the disk and volatility, the salvage rule,
// signals, and the lifecycle with drifting land (appended to kSoupWorldProc).
inline const char* const kPhase4All = "heat.ambient = 0.05\n"
                                      "edge.corruption = 0.01\n"
                                      "edge.failure = 0.005\n"
                                      "edge.downtime = 20\n"
                                      "world.disk = 1024\n"
                                      "disk.rot = 0.0001\n"
                                      "mem.volatility = 0.0002\n"
                                      "salvage.ratio = 2\n" // (2, was 4: so remnants still garble in 300 ticks)
                                      "signal.channels = 8\n"
                                      "node.endurance = 3000\n"
                                      "node.fallow_min = 20\n"
                                      "node.recover_every = 10\n"
                                      "world.land = 0.9\n"
                                      "world.drift = 0.001\n";

// Parses cfg_text into run's config and initializes it; prints why on failure.
inline bool make_run(nova::Run& run, const char* cfg_text, unsigned threads) {
    nova::Config::ParseError pe;
    if (!run.config().parse(cfg_text, &pe)) {
        fprintf(stderr, "  make_run: config: %s\n", pe.msg);
        return false;
    }
    nova::TextBuf err;
    if (!run.init(threads, &err)) {
        fprintf(stderr, "  make_run: init: %s\n", err.c_str());
        return false;
    }
    return true;
}
