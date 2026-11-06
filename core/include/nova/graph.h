#pragma once

// Procedural world generation (Phase 3; design.md 4.1, 4.4a G, 6.9).
//
// Nodes sit in a hidden 2D space, the unit square, that organisms never see.
// One coherent noise field per property gives biomes with gradients; a
// ruggedness field cuts or slows links, leaving plains, chokepoints and
// isolated pockets; a few long links join distant places.
//
//   Placement   jittered grid: ceil(sqrt(N))^2 cells, N of them chosen at
//               random, one node in the middle 80% of each; node ids follow the
//               cells in row-major order.
//   Links       each node to its `links` nearest (ties by id), made symmetric.
//   Ruggedness  the ruggedness field at a link's midpoint, as a rank u among the
//               proximity links (0 smoothest, 1 roughest); links with
//               u >= 1 - ruggedness are cut.
//   Long links  round(long_links x N) extra links, each from a random node to
//               the farthest of 8 random others, never cut.
//   Edges       latency 1 + floor(distance x latency_scale) ticks; bandwidth
//               max(1, floor(bandwidth x (1 - 0.75 u))) codons per tick.
//
// Noise is hand-rolled 2D gradient noise (Perlin's, with 8 gradients) summed
// over octaves (fBm), each field seeded from the world seed and its field id.
// Node properties use a field rank-normalised over the nodes, so every value
// spans -1..1 evenly whatever the raw noise's spread.
//
// Everything is a pure function of the seed and the parameters (and runs
// serially at init), so the graph needs no snapshot.
#include "nova/array.h"
#include "nova/types.h"

namespace nova {

class Noise2 {
  public:
    void init(u64 seed, u32 field);
    // One octave of gradient noise, roughly -1..1 (0 on the integer lattice).
    double at(double x, double y) const;
    // `octaves` octaves from frequency `freq`, each half the amplitude of the
    // last, normalised by the amplitude sum.
    double fbm(double x, double y, u32 octaves = 4, double freq = 3.0) const;

  private:
    u8 perm_[512] = {};
};

// Noise field ids (each its own permutation).
enum NoiseField : u32 {
    kFieldRugged = 0,
    kFieldSoupSize,
    kFieldMillicores,
    kFieldGpus,
    kFieldNpus,
    kFieldCool,
    kFieldSparkRate,
    kFieldRadiation,
    // Phase 4 (new fields go last, so the earlier ones keep their seeds)
    kFieldDisk,
    kFieldVolatility,
    kFieldEndurance,
    kFieldNoise,  // the signal noise floor
    kFieldLand,   // the drifting land field (the lifecycle)
    // Phase 7 landscapes (landscape.h)
    kFieldTerrain, // land and water
    kFieldWarpX,   // the coastline's wander
    kFieldWarpY,
    // Phase 9c
    kFieldEcpu,    // E millicores' share of a site's (landscapes: res.ecpu)
    kFieldCount
};

struct GraphParams {
    u32 nodes = 100;
    u32 links = 4;              // world.links: proximity links per node
    double ruggedness = 0.2;    // world.ruggedness: fraction of proximity links cut
    double long_links = 0.02;   // world.long_links: extra long links per node
    double latency_scale = 20;  // edge.latency_scale: ticks per unit of distance
    u32 bandwidth = 32;         // edge.bandwidth: codons per tick on smooth ground
};

struct ProcGraph {
    Array<double> x, y;     // hidden positions, per node
    Array<u64> keys;        // directed edges (src << 32 | dst), strictly increasing
    Array<u32> latency;     // per directed edge, in key order
    Array<u32> bandwidth;
    Array<double> dist;
    u32 cut = 0;            // proximity links removed by ruggedness
    u32 long_added = 0;     // long links added
    // Kept for rebuilding among live sites (Phase 4 lifecycle): the
    // ruggedness ranks of the generation's proximity links, the cut threshold
    // they gave (a link is cut when its ruggedness key reaches it, so a cut is
    // local and doesn't move as far-off sites come and go), and the long-link
    // site pairs (lo << 32 | hi), active while both ends are live.
    Array<u64> rug_sorted;
    u64 rug_cut = ~0ull;
    Array<u64> long_pairs;
};

void generate_graph(u64 seed, const GraphParams& gp, ProcGraph* out);

// Rebuilds the edges among the live sites (live[i] != 0; nullptr = all):
// each live site to its k nearest live sites, cut at the generation's
// threshold, plus the long pairs whose ends are both live. With every site
// live it gives exactly generate_graph's edges.
void assemble_graph(u64 seed, const GraphParams& gp, ProcGraph* g, const u8* live);

// 3D gradient noise (12 gradients), the third axis being time for the
// drifting fields of the lifecycle (Phase 4).
class Noise3 {
  public:
    void init(u64 seed, u32 field);
    double at(double x, double y, double z) const;
    // Octaves as Noise2::fbm; z is in lattice units at every octave's base.
    double fbm(double x, double y, double z, u32 octaves = 4, double freq = 3.0) const;

  private:
    u8 perm_[512] = {};
};

// node_field over Noise3 at time coordinate z.
void node_field3(u64 seed, u32 field, const Array<double>& x, const Array<double>& y, double z,
                 Array<double>* out);

// Field `field` sampled at every node and rank-normalised: the lowest node
// gets -1, the highest +1, evenly between (a single node gets 0).
void node_field(u64 seed, u32 field, const Array<double>& x, const Array<double>& y, Array<double>* out);

} // namespace nova
