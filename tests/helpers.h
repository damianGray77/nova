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

// A busy soup world: random sparks, a seeded sketch replicator on every node,
// hazards well above baseline and heat gains high enough that units and pages
// get released. Every path of the Phase 1a interpreter fires in it, so the
// determinism gate is not vacuous. 96 nodes x at most 200 instructions per tick.
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
                                      "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 INC DIVIDE\n"
                                      "seed.at = 1000\n"
                                      "seed.millicores = 200\n"
                                      "hazard.copy = 0.005\n"
                                      "hazard.flaw = 0.001\n"
                                      "hazard.rot = 0.00005\n"
                                      "heat.cpu_gain = 36\n"
                                      "heat.fetch_gain = 0.06\n"
                                      "heat.write_gain = 0.5\n";

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
