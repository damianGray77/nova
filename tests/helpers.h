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
