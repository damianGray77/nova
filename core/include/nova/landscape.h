#pragma once

// Landscape worlds (Phase 7, design.md 6.13; plan docs/phase7-plan.md C1-C6):
// one world generated like a game level, `world.graph = landscape`.
//
//   Land     a warped noise field over a rectangle (width land.aspect, height
//            1), sinking toward the borders, cut at land.fraction: land and
//            water on a raster. A flood fill labels each separate landmass.
//   Nodes    world.nodes points thrown at random onto land, none closer than a
//            spacing that shrinks until they all fit (Poisson-disc sampling),
//            numbered row by row.
//   Paths    a Delaunay triangulation of the nodes (Bowyer-Watson, built once)
//            gives each node its natural neighbours. Paths that cross water are
//            dropped. Each node wants a number of paths (paths.mean, spread;
//            hermits want none, hubs paths.max); a path is kept, shortest
//            first, while both ends still want one; hubs then add roads to
//            their neighbours' neighbours. paths.connected keeps each landmass
//            in one piece; paths.long adds a few long links within one.
//   Bridges  triangulation edges between two landmasses are the candidate
//            crossings; the shortest (up to bridge.count per pair, at most
//            bridge.max_length) become bridges, which open for
//            bridge.open_ticks of every bridge.open_every ticks.
//
// Everything is a pure function of the seed and the settings, generated once:
// nothing searches for neighbours while the world runs.
//
// Distributions (C6): each node's share of a total -- sparks, memory, CPU,
// GPU, NPU, disk -- from a skewed spread, optionally smoothed by a noise
// field, with barren nodes and hotspots, adding up exactly to the total.
//
// Substrates (docs/world-plan.md W2): each node's level of each resource
// type's performance, by rank over a smooth field per type (below).
#include "nova/array.h"
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

enum BridgePhase : u32 { kPhaseTogether, kPhaseStaggered, kPhaseRandom };

struct LandParams {
    u32 nodes = 1000;
    double aspect = 1.6;     // land.aspect: width over height
    double fraction = 0.45;  // land.fraction: the share of the area that is land
    double scale = 2.5;      // land.scale: features across the height (more = smaller islands)
    double warp = 0.3;       // land.warp: how far coastlines wander
    double border = 0.6;     // land.border: how strongly the edges sink into the sea
    u32 resolution = 256;    // land.resolution: raster cells across the height
    double paths_mean = 3.0;     // paths.mean: paths a node wants, on average
    double paths_spread = 0.5;   // paths.spread: how unevenly
    double paths_isolated = 0.01; // paths.isolated: hermits, wanting none
    double paths_hubs = 0.02;    // paths.hubs: hubs, wanting paths.max
    u32 paths_max = 12;          // paths.max
    bool paths_connected = false; // paths.connected: each landmass in one piece
    double paths_long = 0.01;    // paths.long: long links within a landmass, per node
    double latency_scale = 20;   // edge.latency_scale: ticks per unit of length
    u32 bandwidth = 32;          // edge.bandwidth: codons per tick on smooth ground
    u32 bridge_count = 1;        // bridge.count: per pair of neighbouring landmasses
    double bridge_isolated = 0;  // bridge.isolated: landmasses with no bridge
    double bridge_max_length = 0.25; // bridge.max_length
    double bridge_latency_scale = 100; // bridge.latency_scale
    u32 bridge_bandwidth = 8;    // bridge.bandwidth
    u32 open_every = 20000;      // bridge.open_every (0 = always open)
    u32 open_ticks = 1000;       // bridge.open_ticks
    u32 phase = kPhaseStaggered; // bridge.phase
};

struct Landscape {
    double width = 1;        // the world is width x 1
    u32 raster_w = 0, raster_h = 0;
    Array<u8> land;          // per raster cell (row by row): 1 land, 0 water
    Array<u32> cell_mass;    // per cell: its landmass, or 0xFFFFFFFF for water
    u32 masses = 0;          // landmasses
    double spacing = 0;      // the nodes' final minimum spacing
    Array<double> x, y;      // per node
    Array<u32> mass;         // per node: its landmass
    Array<u32> want;         // per node: the paths it wanted
    Array<u64> tri_edges;    // the triangulation's edges (lo << 32 | hi), sorted
    // Paths, both directions, sorted by (src << 32 | dst), with their
    // attributes in the same order.
    Array<u64> keys;
    Array<u32> latency, bandwidth, phase;
    Array<double> dist;
    Array<u8> bridge;        // 1 = a bridge between landmasses
    u32 hermits = 0, hubs = 0, bridges = 0, long_added = 0, dropped_water = 0;
};

// False (with the reason) if the nodes don't fit on the land.
bool generate_landscape(u64 seed, const LandParams& p, Landscape* out, TextBuf* err);

// The Delaunay triangulation of n points (Bowyer-Watson with a walking point
// location): triangles as counter-clockwise vertex triples.
void delaunay(const double* x, const double* y, u32 n, Array<u32>* tris);

// Is a bridge with this schedule open during tick t?
inline bool bridge_open(u64 t, u32 every, u32 ticks, u32 phase) {
    return every == 0 || (t + phase) % every < ticks;
}

// A distribution (C6): spread (0 even; the sigma of a lognormal), smooth (0
// independent per node, 1 following a noise field), the barren fraction, and
// hotspots with their boost.
struct Spread {
    double spread = 0, smooth = 0, barren = 0, boost = 2.0;
    u32 hotspots = 0;
    // Phase 8: follow another distribution's noise field the opposite way
    // (rich where it is poor), by that field's number; 0xFFFFFFFF = its own.
    u32 against = 0xFFFFFFFFu;
};

// Each node's weight, averaging 1 over the nodes (barren nodes 0). `field` is
// the noise field and stream to draw from.
void spread_weights(u64 seed, u32 field, const Spread& s, const Array<double>& x, const Array<double>& y,
                    double width, Array<double>* out);
// `total` split by the weights into whole shares that add up exactly, each at
// least `minimum` (barren nodes get the minimum).
void spread_total(const Array<double>& weights, u64 total, u64 minimum, Array<u64>* out);

// --- substrates (docs/world-plan.md W1, W2) ----------------------------------------
// How well each of a node's resources performs, by type: P cores, E cores,
// GPU, NPU, fast and slow memory, fast and slow disk. Each type is at one of
// five levels, multipliers of the floor (substrate.floor), 0.5, 1, 1.5 and 2.
// Each type has its own noise field (field id kFieldSubstrate + type, apart
// from graph.h's NoiseField ids and so from the resource fields), mixed with
// an independent draw per node by `smooth` as the distributions are (C6), at
// `scale` features across the height. The levels go by rank across the nodes,
// in exact shares (substrate.share_*). A pure function of the seed, the
// settings and the positions (and the time, where the fields drift).
inline constexpr u32 kSubstrateTypes = 8;
inline constexpr u32 kSubstrateLevels = 5;
enum SubstrateType : u32 { kSubP, kSubE, kSubGpu, kSubNpu, kSubMemFast, kSubMemSlow, kSubDiskFast, kSubDiskSlow };
inline constexpr u32 kFieldSubstrate = 256; // + type: each type's noise field
inline constexpr u8 kSubstrateOne = 2;      // the 1x level (every level without substrates)

// (The defaults are the registry's, keys.inc: smooth 1 and scale 1.5 since the
// integration, so substrates blend into each other; the approved 0.7 and 3
// made a patchy map. docs/results/world.md.)
struct SubstrateParams {
    double smooth = 1.0;     // substrate.smooth: 0 each node on its own, 1 following the field
    double scale = 1.5;      // substrate.scale: features across the height
    double floor_mult = 0.1; // substrate.floor: the lowest level's multiplier
    double share[kSubstrateLevels] = {10, 20, 40, 20, 10}; // substrate.share_*: relative shares
};

// A type's name for people ("P cores") and for columns ("p_cores").
const char* substrate_label(u32 type);
const char* substrate_column(u32 type);
// A level's multiplier: the floor, 0.5, 1, 1.5 or 2.
double substrate_multiplier(u32 level, double floor_mult);
// How many of n nodes take each level: the shares made exact (largest
// remainders; ties to the lower level), adding up to n.
void substrate_counts(const SubstrateParams& s, u32 n, u32* counts);
// Each node's level (0-4) of each type, out[node x kSubstrateTypes + type].
// time < 0: the static fields; otherwise the fields drifting through their
// noise, at that time coordinate (world.drift x the tick).
void substrate_levels(u64 seed, const SubstrateParams& s, const Array<double>& x, const Array<double>& y,
                      double time, Array<u8>* out);

} // namespace nova
