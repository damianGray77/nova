// The settings registry's table (docs/config-plan.md C1): one entry for every
// setting the engine reads. nova/keys.h explains the macros, how to add a
// setting and how to read it; keys.cpp includes this file once per macro
// set. Keep entries in their group's section, in the order the page shows them.
//
//   NOVA_FLAG  (name, default,                 flags, group, label, needs, help)
//   NOVA_WHOLE (name, default, min, max,       flags, group, label, needs, help)
//   NOVA_NUMBER(name, default, min, max,       flags, group, label, needs, help)
//   NOVA_CHOICE(name, default, "opt1 opt2 ..", flags, group, label, needs, help)
//   NOVA_TEXT  (name, default,                 flags, group, label, needs, help)
//   NOVA_MEMBER(set, member, label, group)     a family's member (names with <set>)
//   NOVA_DEFAULT(name, default)                one family member's own default
//
// A computed default (kKeyComputed) is written as it comes out with every
// other setting at its own default; the call site supplies the real one.

// ---------------------------------------------------------------------------
// Run and output (app/main.cpp read_run_keys)
// ---------------------------------------------------------------------------
NOVA_WHOLE("run.ticks", 1000, 0, kKeyNoMax, 0, "Run and output", "Ticks", "",
           "Stop after this many ticks (0 = no tick limit).")
NOVA_NUMBER("run.seconds", 0, 0, kKeyNoMax, 0, "Run and output", "Seconds", "",
            "Stop at the first tick boundary after this much wall-clock time, in seconds (0 = no limit).")
NOVA_WHOLE("run.after_replicator", -1, -1, kKeyNoMax, 0, "Run and output", "Ticks after a replicator", "",
           "Stop this many ticks after the first replicator is detected (-1 = keep going).")
NOVA_WHOLE("run.threads", 0, 0, 1024, 0, "Run and output", "Threads", "",
           "Worker threads (0 = as budget.threads says; --threads overrides).")
NOVA_WHOLE("stats.window", 100, 1, 4294967295.0, 0, "Run and output", "Stats window", "",
           "Ticks summed into each row of the stats CSV.")
NOVA_WHOLE("stats.nodes_every", 1, 1, 1000000000, 0, "Run and output", "Node rows every", "output.nodes",
           "Write the per-node rows every this many stats windows.")
NOVA_WHOLE("replay.checkpoint_every", 100, 0, kKeyNoMax, 0, "Run and output", "Checkpoint every", "",
           "Ticks between world hashes in the replay log (and in nova resume's output).")
NOVA_TEXT("output.csv", "", 0, "Run and output", "Stats CSV", "",
          "Path of the stats CSV, one row per stats window (empty = off).")
NOVA_TEXT("output.nodes", "", 0, "Run and output", "Node rows", "output.csv",
          "Path of the per-node stats rows (empty = off).")
NOVA_TEXT("output.signals", "", 0, "Run and output", "Signal rows", "output.csv",
          "Path of the signal channels' rows, per window and channel (empty = off).")
NOVA_TEXT("output.replay", "", 0, "Run and output", "Replay log", "",
          "Path of the replay log: the config, checkpoint hashes and events (empty = off).")
NOVA_TEXT("output.snapshot", "", 0, "Run and output", "Snapshot", "",
          "Path of the snapshot written when the run ends (empty = off).")
NOVA_WHOLE("output.snapshot_every", 0, 0, kKeyNoMax, 0, "Run and output", "Snapshot every", "output.snapshot",
           "Also write <output.snapshot>.<tick> every this many ticks (0 = off).")
NOVA_TEXT("output.genebank", "", 0, "Run and output", "Genebank", "",
          "Path of the genebank: every genotype that reached detect.genebank_threshold living copies (empty = off).")
NOVA_TEXT("output.lineages", "", 0, "Run and output", "Lineages", "",
          "Path of the lineages CSV (empty = off).")
NOVA_TEXT("output.result", "", 0, "Run and output", "Result", "",
          "Path of result.txt, what a harness needs to know about the finished run (empty = off).")
// Behaviour profiles (docs/analysis-plan.md A3): their two files.
NOVA_TEXT("output.profiles", "", 0, "Run and output", "Profiles", "profile.enabled",
          "Path of profiles.csv, one row per behaviour sample: what a followed body did (empty = off).")
NOVA_TEXT("output.arrivals", "", 0, "Run and output", "Arrivals", "profile.arrivals",
          "Path of arrivals.csv, the arrivals placed during each sampling round (empty = off).")

// ---------------------------------------------------------------------------
// World and land
// ---------------------------------------------------------------------------
NOVA_WHOLE("seed", 1, 0, kKeyNoMax, 0, "World and land", "Seed", "",
           "The world's seed: everything random in the run follows from it (and the map too, unless world.seed "
           "is set).")
NOVA_TEXT("world.seed", "", 0, "World and land", "Map seed", "",
          "The map's own seed, like a game's world seed: where the nodes are, their paths and bridges, the land, "
          "the substrates and how resources and sparks are spread. A number, or any text (hashed to one); empty: "
          "the map follows seed. Set, seed varies what happens on the same map.")
NOVA_CHOICE("world.model", "demo", "demo soup", 0, "World and land", "Model", "",
            "soup: organisms in memory, the real model; demo: the Phase 0 test world of parcels.")
NOVA_WHOLE("world.nodes", 64, 1, 1000000, 0, "World and land", "Nodes", "",
           "Nodes in the world: sites that each run a soup.")
NOVA_CHOICE("world.graph", "ring", "ring procedural landscape", 0, "World and land", "Graph", "",
            "How nodes connect: a ring plus random chords, a procedural graph of near neighbours, or a landscape "
            "generated like a game level.")
NOVA_WHOLE("world.chords", 32, 0, 10000000, kKeyComputed, "World and land", "Chords", "world.graph=ring",
           "Random chords added to the ring (default: half the nodes).")
NOVA_WHOLE("world.links", 4, 1, 16, 0, "World and land", "Links", "world.graph=procedural",
           "Proximity links each node makes to its nearest neighbours.")
NOVA_NUMBER("world.ruggedness", 0.2, 0, 0.9, 0, "World and land", "Ruggedness", "world.graph=procedural",
            "The fraction of proximity links cut.")
NOVA_NUMBER("world.long_links", 0.02, 0, 1, 0, "World and land", "Long links", "world.graph=procedural",
            "Extra long-distance links, per node.")
NOVA_NUMBER("land.aspect", 1.6, 0.1, 10, 0, "World and land", "Aspect", "world.graph=landscape",
            "The landscape's width over its height.")
NOVA_NUMBER("land.fraction", 0.45, 0.01, 1, 0, "World and land", "Land fraction", "world.graph=landscape",
            "The share of the area that is land.")
NOVA_NUMBER("land.scale", 2.5, 0.1, 100, 0, "World and land", "Feature scale", "world.graph=landscape",
            "Features across the height: higher gives more, smaller islands.")
NOVA_NUMBER("land.warp", 0.3, 0, 2, 0, "World and land", "Coast warp", "world.graph=landscape",
            "How far coastlines wander.")
NOVA_NUMBER("land.border", 0.6, 0, 4, 0, "World and land", "Border", "world.graph=landscape",
            "How strongly the edges sink into the sea.")
NOVA_WHOLE("land.resolution", 256, 16, 8192, 0, "World and land", "Resolution", "world.graph=landscape",
           "Raster cells across the landscape's height.")
NOVA_NUMBER("world.land", 0.8, 0.01, 1, 0, "World and land", "Land share", "world.graph=procedural",
            "The share of sites that are land (live nodes) at once, the top of a drifting land field (1 = off).")
NOVA_NUMBER("world.drift", 0.00001, 0, 1, kKeyLog, "World and land", "Land drift", "world.graph=procedural",
            "How fast the land field drifts through its noise, per tick.")
NOVA_WHOLE("world.budget", 0, 0, 1000000, kKeyMaxComputed, "World and land", "Live node budget",
           "world.graph=procedural|landscape",
           "Live nodes recovery tops up to (0 = the live count at the start; at most world.nodes).")
NOVA_NUMBER("node.endurance", 0, 0, 1e18, kKeyLog | kKeyComputed, "World and land", "Node endurance", "",
            "Codon writes a node takes before it wears out and collapses (0 = never; default 1e9 in procedural "
            "and landscape worlds, else 0).")
NOVA_NUMBER("node.decline", 4, 0, 1000000, 0, "World and land", "Node decline", "node.endurance",
            "Hazards rise by 1 + decline x wear^2 as a node wears.")
NOVA_WHOLE("node.recover_every", 100, 1, 1000000000, 0, "World and land", "Recover every",
           "world.graph=procedural|landscape",
           "Ticks between recovery passes, when fallow land sites can become nodes again.")
NOVA_WHOLE("node.fallow_min", 2000, 0, kKeyNoMax, 0, "World and land", "Fallow at least",
           "world.graph=procedural|landscape",
           "Ticks a collapsed site lies fallow before it can recover.")
// The demo world (world.model = demo): Phase 0's parcels, for tests.
NOVA_WHOLE("world.pool_capacity", 1000, 1, 1000000000, kKeyHidden, "World and land", "Pool capacity",
           "world.model=demo", "The demo world: each node's pool holds at most this much.")
NOVA_WHOLE("world.pool_initial", 500, 0, 1000000000, kKeyHidden | kKeyComputed | kKeyMaxComputed,
           "World and land", "Pool at start", "world.model=demo",
           "The demo world: each node's pool at the start (default half the capacity, at most all of it).")
NOVA_WHOLE("world.outbox_capacity", 2, 1, 1024, kKeyHidden, "World and land", "Outbox capacity", "world.model=demo",
           "The demo world: parcels an edge's outbox holds (read in every world, used only there).")
NOVA_NUMBER("demo.send_rate", 0.5, 0, 1, kKeyHidden, "World and land", "Send rate", "world.model=demo",
            "The demo world: the chance a node sends a parcel, per send (a live knob).")
NOVA_WHOLE("demo.sends_per_tick", 4, 0, 1024, kKeyHidden, "World and land", "Sends a tick", "world.model=demo",
           "The demo world: send attempts per node per tick (a live knob).")
NOVA_WHOLE("demo.mutations_per_tick", 4, 0, 1000000, kKeyHidden, "World and land", "Mutations a tick",
           "world.model=demo", "The demo world: random codon changes per node per tick (a live knob).")
NOVA_WHOLE("demo.max_parcel", 100, 1, 1000000000, kKeyHidden, "World and land", "Largest parcel",
           "world.model=demo", "The demo world: the largest parcel sent (a live knob).")

// ---------------------------------------------------------------------------
// Paths and bridges
// ---------------------------------------------------------------------------
NOVA_FLAG("world.dispersal", false, kKeyComputed, "Paths and bridges", "Dispersal", "",
          "DIVIDE can send a daughter to a neighbouring node (default on in procedural and landscape worlds).")
NOVA_NUMBER("edge.latency_scale", 20, 0, 1000000, 0, "Paths and bridges", "Latency scale",
            "world.graph=procedural|landscape", "Ticks per unit of length along a path.")
NOVA_WHOLE("edge.bandwidth", 32, 1, 1u << 24, 0, "Paths and bridges", "Bandwidth", "",
           "Codons per tick a path carries on smooth ground.")
NOVA_WHOLE("edge.capacity", 2, 1, 1024, 0, "Paths and bridges", "Capacity", "world.dispersal",
           "Cargoes in transit at once on a path, per direction.")
NOVA_NUMBER("edge.corruption", 0.0002, 0, 1, kKeyLog, "Paths and bridges", "Transit corruption", "world.dispersal",
            "The chance per codon per tick in transit of a bit flip.")
NOVA_NUMBER("edge.failure", 0.0001, 0, 1, kKeyLog, "Paths and bridges", "Path failures", "world.dispersal",
            "The chance per path per tick that it goes down; cargo on it is lost.")
NOVA_NUMBER("edge.downtime", 1000, 1, 1000000000, 0, "Paths and bridges", "Downtime", "edge.failure",
            "Ticks a failed path stays down, on average.")
NOVA_NUMBER("paths.mean", 3.0, 0, 64, 0, "Paths and bridges", "Paths per node", "world.graph=landscape",
            "Paths a node wants, on average.")
NOVA_NUMBER("paths.spread", 0.5, 0, 4, 0, "Paths and bridges", "Path spread", "world.graph=landscape",
            "How unevenly nodes want paths.")
NOVA_NUMBER("paths.isolated", 0.01, 0, 1, 0, "Paths and bridges", "Hermits", "world.graph=landscape",
            "The fraction of nodes that want no paths (hermits).")
NOVA_NUMBER("paths.hubs", 0.02, 0, 1, 0, "Paths and bridges", "Hubs", "world.graph=landscape",
            "The fraction of nodes that are hubs, wanting paths.max with roads to their neighbours' neighbours.")
NOVA_WHOLE("paths.max", 12, 1, 64, 0, "Paths and bridges", "Most paths", "world.graph=landscape",
           "The most paths a node has.")
NOVA_FLAG("paths.connected", false, 0, "Paths and bridges", "Connected landmasses", "world.graph=landscape",
          "Keep each landmass in one piece.")
NOVA_NUMBER("paths.long", 0.01, 0, 1, 0, "Paths and bridges", "Long links", "world.graph=landscape",
            "Long links within a landmass, per node.")
NOVA_WHOLE("bridge.count", 1, 0, 64, 0, "Paths and bridges", "Bridges", "world.graph=landscape",
           "Bridges per pair of neighbouring landmasses.")
NOVA_NUMBER("bridge.isolated", 0.0, 0, 1, 0, "Paths and bridges", "Unbridged", "world.graph=landscape",
            "The fraction of landmasses with no bridge.")
NOVA_NUMBER("bridge.max_length", 0.25, 0, 100, 0, "Paths and bridges", "Longest bridge", "world.graph=landscape",
            "The longest a bridge can be, in units of the landscape's height.")
NOVA_NUMBER("bridge.latency_scale", 100, 0, 1000000, 0, "Paths and bridges", "Bridge latency",
            "world.graph=landscape", "Ticks per unit of length across a bridge.")
NOVA_WHOLE("bridge.bandwidth", 8, 1, 1u << 24, 0, "Paths and bridges", "Bridge bandwidth", "world.graph=landscape",
           "Codons per tick a bridge carries.")
NOVA_WHOLE("bridge.open_every", 20000, 0, 1000000000, 0, "Paths and bridges", "Open every",
           "world.graph=landscape", "Bridges open for bridge.open_ticks of every this many ticks (0 = always open).")
NOVA_WHOLE("bridge.open_ticks", 1000, 0, 1000000000, 0, "Paths and bridges", "Open for", "world.graph=landscape",
           "Ticks a bridge stays open each time.")
NOVA_CHOICE("bridge.phase", "staggered", "together staggered random", 0, "Paths and bridges", "Bridge phase",
            "world.graph=landscape", "Whether bridges open together, staggered through the cycle, or at random.")

// ---------------------------------------------------------------------------
// Resources: the distributions over a landscape, node variation, vents
// ---------------------------------------------------------------------------
NOVA_MEMBER("spread", "spark", "sparks", "Sparks")
NOVA_MEMBER("spread", "res.memory", "memory", "Resources")
NOVA_MEMBER("spread", "res.cpu", "CPU", "Resources")
NOVA_MEMBER("spread", "res.gpu", "GPU units", "Resources")
NOVA_MEMBER("spread", "res.npu", "NPU units", "Resources")
NOVA_MEMBER("spread", "res.disk", "disk", "Resources")
NOVA_MEMBER("spread", "res.ecpu", "E cores", "Resources")
NOVA_MEMBER("spread", "res.archive", "archive", "Resources") // disk tiers (docs/world-plan.md W4)
NOVA_NUMBER("<spread>.spread", 0, 0, 4, 0, "", "Spread of <spread>", "world.graph=landscape",
            "How unevenly <spread> are shared out over the nodes: 0 even; higher, richer and poorer nodes.")
NOVA_NUMBER("<spread>.smooth", 0, 0, 1, 0, "", "Smoothness of <spread>", "world.graph=landscape",
            "0: each node on its own; 1: rich and poor regions.")
NOVA_NUMBER("<spread>.barren", 0, 0, 1, 0, "", "Barren for <spread>", "world.graph=landscape",
            "The fraction of nodes with (almost) none.")
NOVA_WHOLE("<spread>.hotspots", 0, 0, 10000, 0, "", "Hotspots of <spread>", "world.graph=landscape",
           "Hotspots: places where there is much more.")
NOVA_NUMBER("<spread>.hotspot_boost", 2, 0, 1000, 0, "", "Hotspot boost for <spread>", "world.graph=landscape",
            "How much more a hotspot has at its centre.")
NOVA_CHOICE("<spread>.against", "none", "none spark memory cpu gpu npu disk ecpu archive", 0, "",
            "<spread> against", "world.graph=landscape",
            "Rich where another distribution is poor (its noise field the other way round); not itself.")
// vary.*: node properties varied by noise fields (procedural worlds; in a
// landscape the resources follow their distributions above instead).
NOVA_MEMBER("vres", "soup_size", "memory", "Resources")
NOVA_MEMBER("vres", "millicores", "CPU", "Resources")
NOVA_MEMBER("vres", "gpus", "GPU units", "Resources")
NOVA_MEMBER("vres", "npus", "NPU units", "Resources")
NOVA_MEMBER("vres", "spark_rate", "spark rate", "Resources")
NOVA_MEMBER("vres", "disk", "disk", "Resources")
NOVA_NUMBER("vary.<vres>", 0, 0, 1, 0, "", "Vary <vres>", "world.graph=procedural",
            "How much <vres> varies from node to node: each node gets 1 + this x its noise field (0 = all the "
            "same).")
NOVA_MEMBER("vprop", "cool", "cooling", "Resources")
NOVA_MEMBER("vprop", "radiation", "radiation", "Resources")
NOVA_MEMBER("vprop", "volatility", "volatility", "Resources")
NOVA_MEMBER("vprop", "endurance", "endurance", "Resources")
NOVA_MEMBER("vprop", "noise", "signal noise", "Resources")
NOVA_NUMBER("vary.<vprop>", 0, 0, 1, 0, "", "Vary <vprop>", "world.graph=procedural|landscape",
            "How much <vprop> varies from node to node: each node gets 1 + this x its noise field (0 = all the "
            "same).")
NOVA_WHOLE("vent.spacing", 4096, 0, 1u << 24, 0, "Resources", "Vent spacing", "",
           "Codons of soup per vent (0 = no vents). Vents write fresh random codons into waste near them.")
NOVA_WHOLE("vent.rate", 64, 0, 1u << 20, 0, "Resources", "Vent rate", "vent.spacing",
           "Random codons each vent writes a tick.")
NOVA_WHOLE("vent.radius", 256, 1, 1u << 24, 0, "Resources", "Vent radius", "vent.spacing",
           "How far from its vent a codon lands, at most (nearer more often).")

// ---------------------------------------------------------------------------
// Substrates (docs/world-plan.md W1, W2): how well each resource performs
// ---------------------------------------------------------------------------
NOVA_FLAG("substrate.enabled", true, 0, "Substrates", "Substrates", "world.graph=procedural|landscape",
          "Each node's P cores, E cores, GPU, NPU, fast and slow memory and fast and slow disk perform at one of "
          "five levels (the floor, 0.5, 1, 1.5 or 2 times), from smooth noise fields (a ring: every level 1).")
NOVA_NUMBER("substrate.smooth", 1, 0, 1, 0, "Substrates", "Smoothness", "substrate.enabled",
            "0: each node's levels on their own; 1: following the noise fields, so neighbours share them.")
NOVA_NUMBER("substrate.scale", 1.5, 0.1, 100, 0, "Substrates", "Feature scale", "substrate.enabled",
            "Features of the substrate fields across the height (like land.scale): higher gives smaller regions.")
NOVA_NUMBER("substrate.floor", 0.1, 0.01, 1, 0, "Substrates", "Floor", "substrate.enabled",
            "The lowest level's multiplier: the resource is there but nearly useless.")
NOVA_NUMBER("substrate.share_floor", 10, 0, 100, 0, "Substrates", "Share at the floor", "substrate.enabled",
            "The share of nodes at the lowest level, for each type, by rank (relative to the other four shares).")
NOVA_NUMBER("substrate.share_half", 20, 0, 100, 0, "Substrates", "Share at 0.5x", "substrate.enabled",
            "The share of nodes at 0.5 times, for each type (relative to the other four shares).")
NOVA_NUMBER("substrate.share_one", 40, 0, 100, 0, "Substrates", "Share at 1x", "substrate.enabled",
            "The share of nodes at 1 times, for each type (relative to the other four shares).")
NOVA_NUMBER("substrate.share_one_half", 20, 0, 100, 0, "Substrates", "Share at 1.5x", "substrate.enabled",
            "The share of nodes at 1.5 times, for each type (relative to the other four shares).")
NOVA_NUMBER("substrate.share_two", 10, 0, 100, 0, "Substrates", "Share at 2x", "substrate.enabled",
            "The share of nodes at 2 times, for each type, by rank (relative to the other four shares).")

// ---------------------------------------------------------------------------
// Sparks, origins and the seeded genome
// ---------------------------------------------------------------------------
NOVA_NUMBER("spark.rate", 0, 0, 1000000, kKeyLog, "Sparks", "Spark rate", "",
            "Expected sparks per node per tick: a random run of waste gets a CPU.")
NOVA_WHOLE("spark.initial", 0, 0, 10000000, 0, "Sparks", "Initial sparks", "", "Sparks at tick 0, per node.")
NOVA_WHOLE("spark.grant", 10, 1, 10000000, 0, "Sparks", "Spark grant", "",
           "Millicores a spark draws from the free pool.")
NOVA_WHOLE("spark.min_len", 8, 1, 1u << 24, kKeyMaxComputed, "Sparks", "Shortest spark", "",
           "The shortest run of waste a spark animates, in codons (at most the node's size).")
NOVA_WHOLE("spark.max_len", 128, 1, 1u << 24, kKeyComputed | kKeyMaxComputed, "Sparks", "Longest spark", "",
           "The longest run a spark animates (default 128, or the node's size if smaller).")
NOVA_CHOICE("spark.timing", "first", "first last", 0, "Sparks", "Spark timing", "",
            "Whether sparks fire before the bodies run each tick, or after them.")
NOVA_NUMBER("spark.activity", 2, 0, 100, 0, "Sparks", "Sparks by activity", "",
            "Sparks fade as a node fills: the rate x (1 - occupancy)^this, occupancy being the share of its memory "
            "held by living bodies (0 = sparks follow time alone, as before).")
NOVA_WHOLE("spark.gpu_grant", 8, 0, 10000000, kKeyComputed, "Sparks", "Spark GPU grant", "gpu.warps",
           "GPU units a spark draws (default 8 with warps, else 0).")
NOVA_NUMBER("spark.gpu_born", 0.5, 0, 1, 0, "Sparks", "Born on the GPU", "gpu.warps",
            "The chance a spark holding both CPU and GPU starts on the GPU, as a warp.")
NOVA_CHOICE("world.origins", "all", "all one", 0, "Sparks", "Origins", "world.model=soup",
            "Sparks on every node, or only on node 0.")
NOVA_TEXT("seed.genome", "", 0, "Sparks", "Seeded genome", "",
          "A genome placed at tick 0, as instruction names (SENSE ALLOC NOP1 ...); empty = none.")
NOVA_TEXT("seed.node", "all", 0, "Sparks", "Seeded node", "seed.genome",
          "The node the genome is placed on: all, or a node id.")
NOVA_WHOLE("seed.at", 0, 0, (1u << 24) - 1, kKeyMaxComputed, "Sparks", "Seed address", "seed.genome",
           "Where in the soup the genome is placed (it must fit before the end).")
NOVA_WHOLE("seed.millicores", 1000, 0, 10000000, 0, "Sparks", "Seed millicores", "seed.genome",
           "Millicores the seeded genome holds.")
NOVA_WHOLE("seed.gpus", 0, 0, 10000000, 0, "Sparks", "Seed GPU units", "seed.genome",
           "GPU units the seeded genome holds.")
NOVA_WHOLE("seed.npus", 0, 0, 10000000, 0, "Sparks", "Seed NPU units", "seed.genome",
           "NPU units the seeded genome holds.")
NOVA_CHOICE("seed.form", "cpu", "cpu gpu", 0, "Sparks", "Seed form", "seed.genome",
            "Whether the seeded genome starts on a CPU or on the GPU as a warp (needs gpu.warps).")

// ---------------------------------------------------------------------------
// CPU and cores, and the instruction set they run
// ---------------------------------------------------------------------------
NOVA_WHOLE("world.millicores", 4000, 1, 10000000, 0, "CPU and cores", "Millicores", "",
           "CPU units per node: 1,000 millicores are one core.")
NOVA_WHOLE("cpu.instr_per_core", 1000, 1, 100000000, 0, "CPU and cores", "Instructions a tick", "",
           "Instructions a tick 1,000 millicores give at full speed (with a timed cost table, this many plain adds).")
NOVA_WHOLE("cpu.halt_after", 8, 1, 255, 0, "CPU and cores", "Halt after errors", "",
           "Consecutive failed instructions that halt a CPU.")
NOVA_NUMBER("cpu.wear", 0.001, 0, 1, kKeyLog, "CPU and cores", "CPU wear", "",
            "The chance per held millicore per tick that it fails back to the free pool.")
NOVA_FLAG("cpu.threads", true, 0, "CPU and cores", "Threads", "",
          "Threads: each runs on one core at a time, and programs can start more (BX 21).")
NOVA_WHOLE("cpu.slices", 8, 1, 64, 0, "CPU and cores", "Slices", "cpu.threads",
           "Turns a body's threads take each tick.")
NOVA_WHOLE("cpu.max_threads", 8, 1, 64, 0, "CPU and cores", "Most threads", "cpu.threads",
           "Threads (CPUs) a body may hold.")
NOVA_NUMBER("cpu.e_share", 0.5, 0, 1, kKeyComputed, "CPU and cores", "E-core share", "cost.table=home_v1",
            "The share of a node's millicores that are efficiency cores (default 0.5 with a timed cost table, "
            "else 0; needs one).")
NOVA_CHOICE("isa.table", "nova_v2", "nova_v1 nova_v1_plain nova_v1_inv2 nova_v2 tierra0 custom", 0,
            "CPU and cores", "Codon table", "",
            "The instruction set: which instruction each of the 64 codons is (nova_v2: codon 0 is INERT, what "
            "dead matter decays to).")
NOVA_TEXT("isa.codons", "", 0, "CPU and cores", "Custom codons", "isa.table=custom",
          "The custom table: 64 instruction names, codon 0 first.")
NOVA_CHOICE("isa.selectors", "nearest", "nearest shuffled", 0, "CPU and cores", "Selectors", "",
            "Selector numbering: nearest-to-self, or a shuffle drawn per run (the control).")
NOVA_WHOLE("isa.selector_seed", 0, 0, kKeyNoMax, 0, "CPU and cores", "Selector seed", "isa.selectors=shuffled",
           "The shuffle's seed (0 = the world seed).")
NOVA_CHOICE("isa.membrane", "private", "private open", 0, "CPU and cores", "Membrane", "",
            "private: a program reads and runs only its own code; open: anywhere (light Tierra mode).")
NOVA_WHOLE("isa.stack_depth", 8, 1, 16, 0, "CPU and cores", "Stack depth", "", "A CPU's stack depth.")
NOVA_WHOLE("isa.search_limit", 400, 1, 1u << 24, kKeyMaxComputed, "CPU and cores", "Search limit", "",
           "How far a template search looks, each way (at most the node's size).")
NOVA_WHOLE("isa.max_template", 8, 1, 64, 0, "CPU and cores", "Longest template", "",
           "The longest template or surface considered.")
NOVA_WHOLE("tierra.divide_share", 32, 0, 64, 0, "CPU and cores", "Tierra divide share", "isa.table=tierra0",
           "tierra0's DIVIDE: the daughter's share of the CPU, in 64ths.")

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
NOVA_WHOLE("world.soup_size", 256, 1, 1u << 24, 0, "Memory", "Soup size", "",
           "Memory per node, in codons.")
NOVA_NUMBER("mem.upkeep", 2, 0, 1e9, 0, "Memory", "Upkeep", "",
            "Instructions per owned codon per tick, at 64 codons: holding memory costs time (0 = free).")
NOVA_NUMBER("mem.upkeep_exp", 4.0 / 3.0, 0.5, 4, 0, "Memory", "Upkeep exponent", "mem.upkeep",
            "How upkeep grows with size: 1 linear, 4/3 square-cube.")
NOVA_NUMBER("mem.decay", 0.0015625, -1, 1, kKeyLog | kKeyComputed, "Memory", "Decay", "",
            "The chance per waste codon per tick that it loses a set bit, fading toward INERT (default Ray's "
            "background rot rate with nova_v2, else 0; below 0 also means the default).")
// memory tiers (docs/world-plan.md W3)
NOVA_NUMBER("mem.fast_share", 0.125, 0, 1, 0, "Memory", "Fast memory share", "",
            "The share of each node's memory, from its start, that is fast memory (whole pages; the rest is slow, "
            "one address space): with a timed cost table every load, store, search and bulk move costs its tier's "
            "measured price, and ALLOC's resource 7 claims fast memory only (0 = one tier, as before).")
NOVA_NUMBER("mem.fast_volatility", 4, 0, 1000000, 0, "Memory", "Fast memory volatility", "mem.fast_share",
            "A fast memory page's chance of a wipe, as a multiple of a slow page's (mem.volatility).")

// ---------------------------------------------------------------------------
// Disk
// ---------------------------------------------------------------------------
NOVA_WHOLE("world.disk", 4096, 0, 1u << 24, 0, "Disk", "Disk size", "",
           "Disk codons per node (0 = no disk tier).")
NOVA_TEXT("disk.path", "", 0, "Disk", "Disk folder", "world.disk",
          "A folder for the disk tier's files (empty = in RAM); each run uses a folder of its own there.")
NOVA_WHOLE("disk.cost_base", 4, 0, 1000000, 0, "Disk", "DISK cost", "world.disk",
           "Instructions a DISK op costs.")
NOVA_NUMBER("disk.cost_per_codon", 0.25, 0, 1000000, 0, "Disk", "DISK cost per codon", "world.disk",
            "... plus this per codon moved.")
NOVA_NUMBER("disk.rot", 1e-8, 0, 1, kKeyLog | kKeyComputed, "Disk", "Disk rot", "world.disk",
            "The chance per disk codon per tick of a bit flip (default 1e-8 with nova_v2, 1e-6 with other tables).")
NOVA_NUMBER("disk.decay", 1e-6, 0, 1, kKeyLog | kKeyComputed, "Disk", "Fossil decay", "world.disk",
            "The chance per free disk codon (a fossil) per tick that it loses a set bit (default 1e-6 with nova_v2, "
            "else 0).")
// Disk tiers (docs/world-plan.md W4): the archive, a slow, near-permanent
// second tier on every node.
NOVA_WHOLE("world.archive", 1024, 0, 1u << 24, kKeyComputed, "Disk", "Archive size", "",
           "Archive codons per node: a slow, near-permanent second disk tier (default a quarter of world.disk; 0 = "
           "no archive).")
NOVA_NUMBER("archive.rot", 1e-10, 0, 1, kKeyLog | kKeyComputed, "Disk", "Archive rot", "world.archive",
            "The chance per archive codon per tick of a bit flip (default a hundredth of disk.rot).")
NOVA_NUMBER("archive.decay", 1e-8, 0, 1, kKeyLog | kKeyComputed, "Disk", "Archive fossil decay", "world.archive",
            "The chance per free archive codon (a fossil) per tick that it loses a set bit (default a hundredth of "
            "disk.decay).")
NOVA_NUMBER("archive.slow", 10, 1, 1000000, 0, "Disk", "Archive slowness", "world.archive",
            "Each DISK op on the archive costs this many times the fast disk's, for its access and per codon.")

// ---------------------------------------------------------------------------
// GPU: the in-world GPU, warps, and the real devices
// ---------------------------------------------------------------------------
NOVA_WHOLE("world.gpus", 400, 0, 10000000, 0, "GPU", "GPU units", "", "GPU units per node (0 = no GPU).")
NOVA_WHOLE("gpu.elements", 4096, 1, 100000000, kKeyComputed, "GPU", "Elements a tick", "world.gpus",
           "Element-ops a GPU unit does per tick at full speed (default 4096 with async devices, else 32; a timed "
           "cost table sets it).")
NOVA_WHOLE("gpu.setup", 32, 0, 1000000, kKeyComputed, "GPU", "GPU op setup", "world.gpus",
           "CPU instructions a GPU op costs to start (default 32 with async devices, else 4).")
NOVA_NUMBER("gpu.cpu_per_element", 1.0 / 1024.0, 0, 1000000, kKeyComputed, "GPU", "CPU per element", "world.gpus",
            "CPU instructions per element done (default 1/1024 with async devices, else 1/16).")
NOVA_NUMBER("gpu.wear", 0, 0, 1, kKeyLog, "GPU", "GPU wear", "world.gpus",
            "The chance per held GPU unit per tick that it fails back to the pool.")
NOVA_FLAG("gpu.warps", true, 0, "GPU", "Warps", "",
          "Warps: programs that run on the GPU in lockstep lanes.")
NOVA_WHOLE("warp.lanes", 32, 1, 64, 0, "GPU", "Lanes", "gpu.warps", "Lanes per warp.")
NOVA_WHOLE("warp.max", 4, 1, 64, 0, "GPU", "Most warps", "gpu.warps", "Warps a body may hold.")
NOVA_WHOLE("warp.capacity", 256, 1, 100000000, 0, "GPU", "Warp capacity", "gpu.warps",
           "Warp work a GPU unit gives per tick at full speed (a timed cost table sets it).")
NOVA_WHOLE("warp.step_cost", 16, 0, 1000000, 0, "GPU", "Step cost", "gpu.warps",
           "Warp work a step costs (a timed cost table sets it).")
NOVA_WHOLE("warp.lane_cost", 1, 0, 1000000, 0, "GPU", "Lane cost", "gpu.warps",
           "... plus this for each lane it runs (a timed cost table sets it to 0).")
NOVA_WHOLE("warp.rounds", 1, 1, 64, 0, "GPU", "Rounds a tick", "gpu.warps",
           "GPU rounds a tick: warps that waited on a host instruction go again.")
NOVA_CHOICE("warp.device", "auto", "auto gpu cpu mixed", 0, "GPU", "Warp device", "gpu.warps",
            "Where warps run: auto shares each round between the GPU and the CPU; gpu, cpu (the reference); mixed "
            "(a test). Every choice gives the same world.")
NOVA_WHOLE("warp.verify", 0, 0, 1000000000, 0, "GPU", "Verify warps every", "gpu.warps",
           "Check every Nth GPU round against the CPU reference (0 = off).")
NOVA_CHOICE("warp.kernel", "dxc", "dxc dxctiers fxc", 0, "GPU", "Warp kernel", "gpu.warps",
            "dxc: the kernel compiled at build time where it can run (every record in video memory, the long "
            "bodies first), else FXC's; dxctiers: DXC's with small records in groupshared tiers; fxc: FXC's always.")
NOVA_WHOLE("warp.groups", 1, 1, 2, 0, "GPU", "Overlap groups", "gpu.warps",
           "2: a tick's GPU round in two node groups, one group's CPU work while the other's warps are on the GPU "
           "(quicker with warp.device = gpu; auto already keeps the CPU busy during the round); 1: every node in one "
           "round. Both give the same world.")
NOVA_CHOICE("accel.mode", "async", "instant async", 0, "GPU", "Accelerator mode", "",
            "instant: accelerator ops finish at once (Phase 4); async: they run on the real devices and land some "
            "ticks later.")
NOVA_WHOLE("accel.latency", 2, 1, 64, 0, "GPU", "Accelerator latency", "accel.mode=async",
           "Barriers until an async result lands (1 = the same barrier; a timed table uses each device's own).")
NOVA_TEXT("accel.gpu", "auto", 0, "GPU", "GPU device", "accel.mode=async",
          "Where GPU ops run: auto (the first hardware GPU), cpu (the reference), or part of a device's name.")
NOVA_WHOLE("accel.verify", 0, 0, 1000000000, 0, "GPU", "Verify ops every", "accel.mode=async",
           "Check every Nth batch of accelerator ops against the CPU reference (0 = off).")

// ---------------------------------------------------------------------------
// NPU
// ---------------------------------------------------------------------------
NOVA_WHOLE("world.npus", 200, 0, 10000000, 0, "NPU", "NPU units", "", "NPU units per node (0 = no NPU).")
NOVA_WHOLE("npu.macs", 65536, 1, 1000000000, kKeyComputed, "NPU", "MACs a tick", "world.npus",
           "Multiply-accumulates an NPU unit does per tick at full speed (default 65536 with async devices, else "
           "256; a timed cost table sets it).")
NOVA_NUMBER("npu.cpu_per_mac", 1.0 / 16384.0, 0, 1000000, kKeyComputed, "NPU", "CPU per MAC", "world.npus",
            "CPU instructions per multiply-accumulate done (default 1/16384 with async devices, else 1/64).")
NOVA_NUMBER("npu.wear", 0, 0, 1, kKeyLog, "NPU", "NPU wear", "world.npus",
            "The chance per held NPU unit per tick that it fails back to the pool.")
NOVA_CHOICE("accel.npu", "auto", "auto cpu gpu", 0, "NPU", "NPU device", "accel.mode=async",
            "Where NPU ops run: auto (the NPU if it passes its self-test, else the GPU), cpu (the reference) or gpu.")

// ---------------------------------------------------------------------------
// Heat
// ---------------------------------------------------------------------------
NOVA_FLAG("heat.enabled", true, 0, "Heat", "Heat", "",
          "Heat: busy units warm up, err more as they pass warm and slow down past hot.")
NOVA_NUMBER("heat.cool", 0.1, 0.0001, 1, 0, "Heat", "Cooling", "heat.enabled",
            "The fraction of its heat a unit loses per tick (the node's climate; vary.cool).")
NOVA_NUMBER("heat.cpu_gain", 6, 0, 1e9, 0, "Heat", "CPU heating", "heat.enabled",
            "Heat a CPU unit gains per tick at full use.")
NOVA_NUMBER("heat.fetch_gain", 0.001, 0, 1e9, kKeyLog, "Heat", "Fetch heating", "heat.enabled",
            "Heat a memory page gains per instruction fetched from it.")
NOVA_NUMBER("heat.write_gain", 0.01, 0, 1e9, kKeyLog, "Heat", "Write heating", "heat.enabled",
            "Heat a memory page gains per codon written into it.")
NOVA_NUMBER("heat.gpu_gain", 9, 0, 1e9, 0, "Heat", "GPU heating", "heat.enabled",
            "Heat a GPU unit gains per tick at full use (it runs hot).")
NOVA_NUMBER("heat.npu_gain", 9, 0, 1e9, 0, "Heat", "NPU heating", "heat.enabled",
            "Heat an NPU unit gains per tick at full use.")
NOVA_NUMBER("heat.e_gain", 0.4, 0, 1000, 0, "Heat", "E-core heating", "heat.enabled",
            "An E millicore's heat gain against a P millicore's.")
NOVA_NUMBER("heat.ambient", 0.05, 0, 1000000, 0, "Heat", "Ambient leak", "heat.enabled",
            "Each tick every unit on a node gains this x the node's mean CPU heat input (0 = off).")
NOVA_MEMBER("unit", "cpu", "CPU", "Heat")
NOVA_MEMBER("unit", "mem", "memory", "Heat")
NOVA_MEMBER("unit", "gpu", "GPU", "Heat")
NOVA_MEMBER("unit", "npu", "NPU", "Heat")
NOVA_NUMBER("heat.<unit>.warm", 40, 0, 1e9, 0, "", "<unit> warm", "heat.enabled",
            "The heat at which a <unit> unit's error rate starts rising (below hot).")
NOVA_NUMBER("heat.<unit>.hot", 60, 0, 1e9, 0, "", "<unit> hot", "heat.enabled",
            "The heat at which a <unit> unit starts slowing down (below max).")
NOVA_NUMBER("heat.<unit>.max", 100, 0, 1e9, 0, "", "<unit> max", "heat.enabled",
            "The heat at which a <unit> unit is released, keeping its heat.")
NOVA_NUMBER("heat.<unit>.speed_min", 0.25, 0.001, 1, 0, "", "<unit> slowest", "heat.enabled",
            "A <unit> unit's speed at max heat.")
NOVA_NUMBER("heat.<unit>.err_max", 10, 1, 1000000, 0, "", "<unit> error multiplier", "heat.enabled",
            "A <unit> unit's error multiplier at max heat.")
NOVA_DEFAULT("heat.mem.hot", 70)

// ---------------------------------------------------------------------------
// Hazards
// ---------------------------------------------------------------------------
NOVA_NUMBER("hazard.copy", 5e-4, 0, 1, kKeyLog, "Hazards", "Copy errors", "",
            "The chance per codon copied of an error (Ray's baseline: about 1 in 2,000).")
NOVA_NUMBER("hazard.flaw", 1e-4, 0, 1, kKeyLog, "Hazards", "Flaws", "",
            "The chance per instruction executed of a flaw (Ray's baseline: about 1 in 10,000).")
NOVA_NUMBER("hazard.rot_scale", 0.01, 0, 1000000, kKeyComputed, "Hazards", "Cosmic ray scale", "",
            "Bit rot (cosmic rays) as a share of Ray's background rate (default 0.01 with decay on, else 1).")
NOVA_NUMBER("hazard.rot", 1.5625e-5, 0, 1, kKeyLog | kKeyComputed, "Hazards", "Bit rot", "",
            "The chance per codon per tick of a random bit flip, living or dead (default Ray's background rate x "
            "hazard.rot_scale).")
// Rays (docs/lifehistory-plan.md R)
NOVA_FLAG("hazard.rays", false, 0, "Hazards", "Rays", "",
          "Cosmic rays, storm static and lightning travel as rays: each starts outside allocated space and loses "
          "energy to each codon it passes, NOPs most, dead matter next, living code least (off: independent flips, "
          "as before).")
NOVA_NUMBER("hazard.ray_cosmic", 512, 0.001, 1e9, 0, "Hazards", "Cosmic ray energy", "hazard.rays",
            "A cosmic ray's energy (with hazard.ray_absorb = 1: the living codons it passes); they arrive so that "
            "unshielded code takes hazard.rot's flips.")
NOVA_NUMBER("hazard.ray_flip", 0.03125, 0, 1, 0, "Hazards", "Ray flip chance", "hazard.rays",
            "The chance a ray flips one bit of each codon it passes (cosmic rays: x the page's error multiplier) ...")
NOVA_NUMBER("hazard.ray_flip_nop", 0.0078125, 0, 1, 0, "Hazards", "Ray flip chance, NOPs", "hazard.rays",
            "... and of each NOP: NOPs resist, without being immune.")
NOVA_NUMBER("hazard.ray_absorb", 1, 0.000001, 1e9, 0, "Hazards", "Ray absorption, living", "hazard.rays",
            "The energy a ray loses to each codon of living code it passes (the least) ...")
NOVA_NUMBER("hazard.ray_absorb_waste", 2, 0.000001, 1e9, 0, "Hazards", "Ray absorption, dead", "hazard.rays",
            "... to each codon of dead matter, waste and INERT (more) ...")
NOVA_NUMBER("hazard.ray_absorb_nop", 8, 0.000001, 1e9, 0, "Hazards", "Ray absorption, NOPs", "hazard.rays",
            "... and to each NOP, living or dead (the most), so NOP padding shields.")
NOVA_NUMBER("mem.volatility", 0.00001, 0, 1, kKeyLog, "Hazards", "Volatility", "",
            "The chance per memory page per tick that a wipe starts there, turning whole pages to random codons.")
NOVA_WHOLE("mem.wipe_pages", 4, 1, 1u << 18, 0, "Hazards", "Wipe size", "mem.volatility",
           "A wipe covers 1 to this many pages.")

// ---------------------------------------------------------------------------
// Costs
// ---------------------------------------------------------------------------
NOVA_CHOICE("cost.table", "home_v1", "classic home_v1", 0, "Costs", "Cost table", "",
            "classic: every instruction costs 1; home_v1: measured costs in picoseconds on this machine's cores "
            "(a timed table; nova costs prints it).")
NOVA_WHOLE("engulf.codons_per_tick", 8, 1, 1u << 24, 0, "Costs", "ENGULF codons a tick", "cost.table=home_v1",
           "The prey's codons an ENGULF takes a tick, with a timed table.")
NOVA_WHOLE("inject.codons_per_tick", 16, 1, 1u << 24, 0, "Costs", "INJECT codons a tick", "cost.table=home_v1",
           "Codons an INJECT writes a tick, with a timed table.")

// ---------------------------------------------------------------------------
// Contact and eating
// ---------------------------------------------------------------------------
NOVA_WHOLE("contact.range", 256, 0, 1u << 24, kKeyMaxComputed, "Contact and eating", "Contact range", "",
           "The farthest from the attacker's IP an ENGULF or INJECT reaches a surface (at most the node's size).")
NOVA_WHOLE("contact.cost_base", 8, 0, 1000000, 0, "Contact and eating", "Contact cost", "",
           "Instructions an ENGULF or INJECT attempt costs, success or not.")
NOVA_NUMBER("contact.cost_per_codon", 1.0, 0, 1000000, 0, "Contact and eating", "Contact cost per codon", "",
            "... plus this per codon moved.")
NOVA_FLAG("contact.inject_key", true, 0, "Contact and eating", "INJECT needs a key", "",
          "INJECT needs a key: a template matching a host surface.")
NOVA_NUMBER("contact.inject_room", 1.0, 0, 1000000, 0, "Contact and eating", "INJECT room", "",
            "INJECT fails unless the host block is at least this x the payload (0 = off).")
NOVA_FLAG("engulf.bite", true, 0, "Contact and eating", "ENGULF bites", "",
          "ENGULF takes a bite of CX codons, of raw material or of a body; else it swallows whole prey (Phase 9).")
NOVA_NUMBER("salvage.ratio", 4.0, 0, 1000000, 0, "Contact and eating", "Salvage ratio", "",
            "A program takes another's remnants intact only up to this x its own genome length; bigger code comes "
            "through in fragments (0 = off).")

// ---------------------------------------------------------------------------
// Signals
// ---------------------------------------------------------------------------
NOVA_WHOLE("signal.channels", 16, 0, 256, 0, "Signals", "Channels", "",
           "Signal channels (0 = off: EMIT and LISTEN fail).")
NOVA_WHOLE("signal.cost_base", 2, 0, 1000000, 0, "Signals", "EMIT cost", "signal.channels",
           "Instructions an EMIT costs.")
NOVA_NUMBER("signal.cost_per_amp", 1.0 / 32.0, 0, 1000000, 0, "Signals", "EMIT cost per amplitude",
            "signal.channels", "... plus this per unit of amplitude.")
NOVA_NUMBER("signal.floor", 1.0, 0, 1e9, 0, "Signals", "Noise floor", "signal.channels",
            "Contributions below this are dropped, and every channel carries noise within it (vary.noise).")
NOVA_NUMBER("signal.attenuation", 0.25, 0, 1, 0, "Signals", "Attenuation", "signal.channels",
            "A signal's amplitude is multiplied by this at every hop.")
NOVA_WHOLE("signal.reach", 8, 0, 64, 0, "Signals", "Reach", "signal.channels", "The most hops a signal travels.")
NOVA_WHOLE("signal.sustained", 100, 1, 1000000000, 0, "Signals", "Signaller ticks", "signal.channels",
           "The signaller event: a gated lineage's members emitted on this many distinct ticks.")

// ---------------------------------------------------------------------------
// Life and death
// ---------------------------------------------------------------------------
NOVA_FLAG("life.threads_end", true, 0, "Life and death", "Halted threads end", "",
          "A thread that halts leaves its body; a body with no thread left dies.")
NOVA_WHOLE("life.frozen_ticks", 200, 0, 1000000000, 0, "Life and death", "Frozen ticks", "",
           "A body none of whose threads ran an instruction for this long dies, or goes dormant (0 = never).")
NOVA_WHOLE("life.gestation", 0, 0, 1000000000, 0, "Life and death", "Gestation", "",
           "A daughter can be divided off only this many ticks after the ALLOC that made her block; an earlier "
           "DIVIDE waits, idle (0 = at once, as before; docs/lifehistory-plan.md G).")
NOVA_NUMBER("life.gestation_per_codon", 0, 0, 1000000, 0, "Life and death", "Gestation per codon", "life.gestation",
            "... plus this many ticks per codon of her block.")

// ---------------------------------------------------------------------------
// Dormancy
// ---------------------------------------------------------------------------
NOVA_FLAG("life.dormancy", true, 0, "Dormancy", "Dormancy", "",
          "A frozen body with reserves goes dormant instead of dying, drifts from node to node and wakes where a "
          "core of its kind is free.")
NOVA_WHOLE("dormant.mem_loss", 1, 0, 1u << 24, 0, "Dormancy", "Memory reserve loss", "life.dormancy",
           "Codons of its memory reserve a dormant program loses a tick.")
NOVA_WHOLE("dormant.disk_loss", 1, 0, 1u << 24, 0, "Dormancy", "Disk reserve loss", "life.dormancy",
           "Codons of its disk reserve it loses a tick, once its memory reserve is gone.")
NOVA_WHOLE("dormant.dwell", 15, 1, 1000000000, 0, "Dormancy", "Dwell", "life.dormancy",
           "Ticks a resting program stays on a node before it moves on.")

// ---------------------------------------------------------------------------
// Disasters (docs/disasters-plan.md X8; core/src/disaster.cpp): set before a
// run, fixed for it. Intervals are world-wide mean ticks (0 = never).
// ---------------------------------------------------------------------------
NOVA_FLAG("disaster.enabled", true, 0, "Disasters", "Disasters", "",
          "Rare random disasters: power surges, OS crashes, burnouts, disk failures and lightning storms.")
// X2: power surges
NOVA_FLAG("disaster.surge", true, 0, "Disasters", "Power surges", "disaster.enabled",
          "A region collapses: every live node within a few hops of a random one (needs a map world).")
NOVA_WHOLE("disaster.surge_every", 50000, 0, kKeyNoMax, 0, "Disasters", "Surge interval", "disaster.surge",
           "Mean ticks between power surges anywhere in the world (0 = none).")
NOVA_WHOLE("disaster.surge_hops_min", 1, 0, 64, 0, "Disasters", "Surge reach, least", "disaster.surge",
           "A surge's reach in hops along paths, drawn per surge from here ...")
NOVA_WHOLE("disaster.surge_hops_max", 2, 0, 64, 0, "Disasters", "Surge reach, most", "disaster.surge",
           "... to here.")
NOVA_WHOLE("disaster.surge_lead_min", 1000, 1, 1000000000, 0, "Disasters", "Surge warning, least", "disaster.surge",
           "Ticks of brownout before a surge (its symptoms), drawn per surge from here ...")
NOVA_WHOLE("disaster.surge_lead_max", 5000, 1, 1000000000, 0, "Disasters", "Surge warning, most", "disaster.surge",
           "... to here.")
// X3: OS crashes
NOVA_FLAG("disaster.crash", true, 0, "Disasters", "OS crashes", "disaster.enabled",
          "A node reboots: nothing runs and nothing arrives meanwhile; some of its programs are deleted and the rest "
          "restart.")
NOVA_WHOLE("disaster.crash_every", 5000, 0, kKeyNoMax, 0, "Disasters", "Crash interval", "disaster.crash",
           "Mean ticks between OS crashes anywhere in the world (0 = none).")
NOVA_WHOLE("disaster.reboot_min", 20, 1, 1000000000, 0, "Disasters", "Reboot, shortest", "disaster.crash",
           "Ticks a crashed node takes to reboot, drawn per crash from here ...")
NOVA_WHOLE("disaster.reboot_max", 100, 1, 1000000000, 0, "Disasters", "Reboot, longest", "disaster.crash",
           "... to here.")
NOVA_NUMBER("disaster.crash_share_min", 0.1, 0, 1, 0, "Disasters", "Crash deletes, least", "disaster.crash",
            "The share of a crashed node's running programs deleted (ordinary deaths), drawn per crash from here ...")
NOVA_NUMBER("disaster.crash_share_max", 0.6, 0, 1, 0, "Disasters", "Crash deletes, most", "disaster.crash",
            "... to here.")
NOVA_WHOLE("disaster.crash_lead_min", 200, 1, 1000000000, 0, "Disasters", "Crash warning, least", "disaster.crash",
           "Ticks of instability before a crash (its symptoms), drawn per crash from here ...")
NOVA_WHOLE("disaster.crash_lead_max", 1000, 1, 1000000000, 0, "Disasters", "Crash warning, most", "disaster.crash",
           "... to here.")
// X4: burnouts
NOVA_FLAG("disaster.burnout", true, 0, "Disasters", "Burnouts", "disaster.enabled",
          "Whole programs' cores go offline for repair and the programs go dormant, resting at home first.")
NOVA_WHOLE("disaster.burnout_every", 2000, 0, kKeyNoMax, 0, "Disasters", "Burnout interval",
           "disaster.burnout", "Mean ticks between burnouts anywhere in the world (0 = none).")
NOVA_NUMBER("disaster.burnout_share_min", 0.1, 0, 1, 0, "Disasters", "Burnout share, least", "disaster.burnout",
            "Programs are picked until they hold this share of the node's cores (millicores and GPU units), drawn "
            "per burnout from here ...")
NOVA_NUMBER("disaster.burnout_share_max", 0.4, 0, 1, 0, "Disasters", "Burnout share, most", "disaster.burnout",
            "... to here.")
NOVA_WHOLE("disaster.repair_min", 200, 1, 1000000000, 0, "Disasters", "Repair, shortest", "disaster.burnout",
           "Ticks burnt cores spend offline before they return to the pool, cool, drawn per burnout from here ...")
NOVA_WHOLE("disaster.repair_max", 2000, 1, 1000000000, 0, "Disasters", "Repair, longest", "disaster.burnout",
           "... to here.")
NOVA_NUMBER("disaster.grace_chance", 0.5, 0, 1, 0, "Disasters", "Grace chance", "disaster.burnout",
            "The chance a burnout victim with no reserves gets a grace reserve; the rest die.")
NOVA_WHOLE("disaster.grace", 32, 0, 1u << 24, 0, "Disasters", "Grace reserve", "disaster.burnout",
           "The grace reserve, in codons of memory reserve (about that many ticks at the default clock).")
NOVA_WHOLE("disaster.burnout_lead_min", 200, 1, 1000000000, 0, "Disasters", "Burnout warning, least",
           "disaster.burnout", "Ticks of fever before a burnout (its symptoms), drawn per burnout from here ...")
NOVA_WHOLE("disaster.burnout_lead_max", 1000, 1, 1000000000, 0, "Disasters", "Burnout warning, most",
           "disaster.burnout", "... to here.")
// X5: disk failures
NOVA_FLAG("disaster.diskfail", true, 0, "Disasters", "Disk failures", "disaster.enabled",
          "A node's disk is lost: every codon INERT, every holding released, resting programs' disk reserves gone.")
NOVA_WHOLE("disaster.diskfail_every", 10000, 0, kKeyNoMax, 0, "Disasters", "Disk failure interval",
           "disaster.diskfail", "Mean ticks between disk failures anywhere in the world (0 = none).")
NOVA_WHOLE("disaster.diskfail_lead_min", 500, 1, 1000000000, 0, "Disasters", "Disk warning, least",
           "disaster.diskfail", "Ticks of faster disk rot before a disk failure (its symptoms), drawn from here ...")
NOVA_WHOLE("disaster.diskfail_lead_max", 2000, 1, 1000000000, 0, "Disasters", "Disk warning, most",
           "disaster.diskfail", "... to here.")
// X6: lightning storms
NOVA_FLAG("disaster.storm", true, 0, "Disasters", "Lightning storms", "disaster.enabled",
          "Storms travel over the map with a heading, striking the node they're over: runs of random codons.")
NOVA_WHOLE("disaster.storm_every", 5000, 0, kKeyNoMax, 0, "Disasters", "Storm interval", "disaster.storm",
           "Mean ticks between storms forming anywhere in the world (0 = none).")
NOVA_WHOLE("disaster.storm_life_min", 2000, 1, 1000000000, 0, "Disasters", "Storm life, shortest", "disaster.storm",
           "A storm's lifetime in ticks, drawn per storm from here ...")
NOVA_WHOLE("disaster.storm_life_max", 10000, 1, 1000000000, 0, "Disasters", "Storm life, longest", "disaster.storm",
           "... to here.")
NOVA_WHOLE("disaster.storm_move_min", 200, 1, 1000000000, 0, "Disasters", "Storm moves, soonest", "disaster.storm",
           "Ticks between a storm's moves along a path, drawn per move from here ...")
NOVA_WHOLE("disaster.storm_move_max", 800, 1, 1000000000, 0, "Disasters", "Storm moves, latest", "disaster.storm",
           "... to here.")
NOVA_NUMBER("disaster.storm_turn", 30, 0, 180, 0, "Disasters", "Storm turn", "disaster.storm",
            "The most a storm's heading turns at a move, in degrees, either way.")
NOVA_WHOLE("disaster.strike_every_min", 100, 1, 1000000000, 0, "Disasters", "Strikes, most often", "disaster.storm",
           "A storm's mean ticks between strikes, drawn per storm from here ...")
NOVA_WHOLE("disaster.strike_every_max", 400, 1, 1000000000, 0, "Disasters", "Strikes, least often",
           "disaster.storm", "... to here.")
NOVA_WHOLE("disaster.strike_min", 64, 1, 1u << 24, 0, "Disasters", "Strike size, least", "disaster.storm",
           "Codons a strike randomises at a random place in memory, drawn per strike from here ...")
NOVA_WHOLE("disaster.strike_max", 512, 1, 1u << 24, 0, "Disasters", "Strike size, most", "disaster.storm",
           "... to here.")
// X10: symptoms
NOVA_FLAG("disaster.symptoms", true, 0, "Disasters", "Symptoms", "disaster.enabled",
          "Every disaster is preceded by symptoms that ramp up over its warning time in quantities programs already "
          "read or suffer.")
NOVA_NUMBER("disaster.false_alarms", 0.25, 0, 0.9, 0, "Disasters", "False alarms", "disaster.symptoms",
            "The share of symptom episodes (and storms: dry ones) that fade without a disaster.")
NOVA_NUMBER("disaster.brownout", 1.0, 0, 1000000, 0, "Disasters", "Brownout heat", "disaster.symptoms",
            "A surge's warning: extra heat input on every unit and page in its region, a tick, at its peak.")
NOVA_NUMBER("disaster.noise", 4.0, 0, 1000000, 0, "Disasters", "Static noise", "disaster.symptoms",
            "Surges and storms raise the signal noise to (1 + this) x the floor at their peak.")
NOVA_NUMBER("disaster.crash_faults", 5e-4, 0, 1, kKeyLog, "Disasters", "Instability faults", "disaster.symptoms",
            "A crash's warning: spurious instruction faults per instruction on its node, at the peak.")
NOVA_NUMBER("disaster.crash_halts", 0.1, 0, 1, 0, "Disasters", "Stray halts", "disaster.symptoms",
            "The share of those faults that halt their thread outright.")
NOVA_NUMBER("disaster.fever", 2.0, 0, 1000000, 0, "Disasters", "Burnout fever", "disaster.symptoms",
            "A burnout's warning: extra heat a tick on the cores about to burn, at the peak.")
NOVA_NUMBER("disaster.disk_rot", 2e-4, 0, 1, kKeyLog, "Disasters", "Failing disk rot", "disaster.symptoms",
            "A disk failure's warning: extra disk rot per codon per tick on its node, at the peak.")
NOVA_NUMBER("disaster.static", 1e-4, 0, 1, kKeyLog, "Disasters", "Storm static", "disaster.symptoms",
            "A storm's warning: bit flips per memory codon per tick under it, building on the node it moves to next.")
NOVA_NUMBER("disaster.static_energy", 32, 0.001, 1e9, 0, "Disasters", "Static energy", "hazard.rays",
            "With hazard.rays, static arrives as rays of this energy (low: a few NOPs stop one), so that unshielded "
            "code takes disaster.static's flips.")
NOVA_NUMBER("disaster.strike_energy", 1, 0.000001, 1e6, 0, "Disasters", "Strike energy", "hazard.rays",
            "With hazard.rays, a strike is a ray that randomises each codon it passes, with its drawn size x this x "
            "hazard.ray_absorb of energy: unshielded code loses the drawn size, and NOP padding shortens it.")
// X11: sounds
NOVA_FLAG("disaster.sounds", true, 0, "Disasters", "Sounds", "disaster.enabled",
          "Each disaster has a voice in the signal field, on its own band of channels (silent without signals).")
NOVA_WHOLE("disaster.surge_band", 13, 0, 255, 0, "Disasters", "Surge band", "disaster.sounds",
           "A surge's first channel (3 wide): its hum and its bang.")
NOVA_NUMBER("disaster.surge_loud", 256, 0, 1e9, 0, "Disasters", "Surge hum", "disaster.sounds",
            "The hum's amplitude at its peak, from every node of the region.")
NOVA_NUMBER("disaster.bang", 4096, 0, 1e9, 0, "Disasters", "Surge bang", "disaster.sounds",
            "The bang's amplitude, from the epicentre, at the surge.")
NOVA_WHOLE("disaster.crash_band", 8, 0, 255, 0, "Disasters", "Crash band", "disaster.sounds",
           "A crash's first channel (2 wide): its stuttering clicks.")
NOVA_NUMBER("disaster.crash_loud", 64, 0, 1e9, 0, "Disasters", "Crash clicks", "disaster.sounds",
            "A click's amplitude.")
NOVA_WHOLE("disaster.burnout_band", 6, 0, 255, 0, "Disasters", "Burnout band", "disaster.sounds",
           "A burnout's first channel (2 wide): its crackle.")
NOVA_NUMBER("disaster.burnout_loud", 32, 0, 1e9, 0, "Disasters", "Burnout crackle", "disaster.sounds",
            "A crackle's typical amplitude (half to one and a half times this).")
NOVA_WHOLE("disaster.diskfail_band", 5, 0, 255, 0, "Disasters", "Disk band", "disaster.sounds",
           "A failing disk's channel: its grinding, a pulse that speeds up.")
NOVA_NUMBER("disaster.diskfail_loud", 16, 0, 1e9, 0, "Disasters", "Disk grinding", "disaster.sounds",
            "A grinding pulse's amplitude.")
NOVA_WHOLE("disaster.storm_band", 10, 0, 255, 0, "Disasters", "Storm band", "disaster.sounds",
           "A storm's first channel (3 wide): its rumble and its thunder.")
NOVA_NUMBER("disaster.rumble", 48, 0, 1e9, 0, "Disasters", "Storm rumble", "disaster.sounds",
            "The rumble's largest amplitude, wherever a storm is, every tick.")
NOVA_NUMBER("disaster.thunder", 2048, 0, 1e9, 0, "Disasters", "Thunder", "disaster.sounds",
            "Thunder's amplitude at each strike: it carries further than static.")

// ---------------------------------------------------------------------------
// Detection and lineages
// ---------------------------------------------------------------------------
NOVA_FLAG("detect.enabled", true, 0, "Detection and lineages", "Detection", "",
          "The lineage detector: inheritance links, lineages, the replicator gate and the genebank.")
NOVA_NUMBER("detect.similarity", 0.75, 0, 1, kKeyOpenMin, "Detection and lineages", "Similarity",
            "detect.enabled", "How closely a daughter must match her parent's block for an inheritance link.")
NOVA_WHOLE("detect.shift", 8, 0, 64, 0, "Detection and lineages", "Alignment shift", "detect.enabled",
           "A DIVIDE daughter is also compared with her parent shifted by up to this many codons either way, and "
           "rotated where her first 4 codons appear in it (descent, not likeness: disasters plan X12).")
NOVA_WHOLE("detect.genebank_threshold", 2, 1, 1000000, 0, "Detection and lineages", "Genebank threshold",
           "detect.enabled", "A genotype is saved to the genebank when this many copies of it live at once.")
NOVA_FLAG("detect.inject", true, 0, "Detection and lineages", "INJECT links", "detect.enabled",
          "An INJECT that leaves the victim matching the injector is an inheritance link.")
NOVA_FLAG("detect.written", true, 0, "Detection and lineages", "Written codons only", "detect.enabled",
          "A DIVIDE link counts only the codons the parent wrote into the daughter since her ALLOC.")
NOVA_WHOLE("detect.gate_depth", 2, 1, 1000000, 0, "Detection and lineages", "Gate depth", "detect.enabled",
           "The replicator gate: a lineage with a member this many generations deep ...")
NOVA_WHOLE("detect.gate_alive", 2, 1, 1000000, 0, "Detection and lineages", "Gate alive", "detect.enabled",
           "... and this many members alive at once.")
NOVA_WHOLE("detect.defended", 3, 1, 1000000000, 0, "Detection and lineages", "Defended", "detect.enabled",
           "The defended milestone: a gated lineage whose members made this many attacks fail by changing in time.")
// Behaviour profiles (docs/analysis-plan.md A3; profile.h): an observer like the
// detector, so it never changes a world (World::init reads these).
NOVA_FLAG("profile.enabled", true, 0, "Detection and lineages", "Behaviour profiles", "",
          "Follow a few bodies per node now and then and record what they do: profiles.csv and the role columns. "
          "Observe-only.")
NOVA_WHOLE("profile.every", 1000, 1, 1000000000, 0, "Detection and lineages", "Profile every", "profile.enabled",
           "Ticks between sampling rounds.")
NOVA_WHOLE("profile.per_node", 4, 1, 4096, 0, "Detection and lineages", "Profiles per node", "profile.enabled",
           "Bodies each node picks to follow every round, from the observer's own random stream.")
NOVA_WHOLE("profile.ticks", 50, 1, 1000000000, 0, "Detection and lineages", "Profile length", "profile.enabled",
           "Ticks each picked body is followed, or until it dies or goes dormant (at most profile.every).")
NOVA_FLAG("profile.arrivals", true, 0, "Detection and lineages", "Arrivals log", "profile.enabled",
          "Log the arrivals placed during each round's ticks (output.arrivals), for the moving-together measure.")
// The live novelty meter (docs/analysis-plan.md A2): an observer, read by World::init, used by the run.
NOVA_FLAG("novelty.enabled", true, 0, "Detection and lineages", "Live novelty meter", "detect.enabled",
          "Measure each new genotype the genebank saves against a 4-codon model of every one before it, for the "
          "stats columns novelty, novel_share and novel_success (it only observes: no world changes).")
NOVA_NUMBER("novelty.threshold", 3.0, 0, 64, 0, "Detection and lineages", "Novel at", "novelty.enabled",
            "Bits per codon of surprise at or above which a new genotype counts as novel (nova analyse novelty's "
            "--threshold).")

// ---------------------------------------------------------------------------
// Budgets: settings from the machine (budget.h)
// ---------------------------------------------------------------------------
NOVA_TEXT("budget.memory", "off", 0, "Budgets", "Memory budget", "",
          "off, a percentage of the RAM available (25%) or an amount (24GB): sets world.soup_size, the mean per "
          "node.")
NOVA_TEXT("budget.disk", "off", 0, "Budgets", "Disk budget", "disk.path",
          "off, a percentage of the free space at disk.path (20%) or an amount (100GB) for the whole disk tier, "
          "salvage stamps included: sets world.disk so it and its archive (world.archive) fit.")
NOVA_TEXT("budget.gpu", "off", 0, "Budgets", "GPU budget", "",
          "off or a percentage (50%): a calibration sizes gpu.elements (and warp.capacity, with warps) to keep the "
          "GPU about that busy.")
NOVA_TEXT("budget.npu", "off", 0, "Budgets", "NPU budget", "accel.mode=async",
          "off or a percentage (50%): a calibration sizes npu.macs to keep the NPU about that busy.")
NOVA_MEMBER("budget", "memory", "memory", "Budgets")
NOVA_MEMBER("budget", "disk", "disk", "Budgets")
NOVA_MEMBER("budget", "gpu", "GPU", "Budgets")
NOVA_MEMBER("budget", "npu", "NPU", "Budgets")
NOVA_TEXT("budget.<budget>_was", "", kKeyHidden, "", "<budget> budget asked", "",
          "The <budget> budget's request, kept for the record once the run has resolved it.")
NOVA_TEXT("budget.threads", "all_but_one", 0, "Budgets", "Threads", "",
          "Threads when run.threads is 0: all, all_but_one, a count, or a percentage of the logical processors.")
NOVA_CHOICE("budget.priority", "below_normal", "below_normal normal idle", 0, "Budgets", "Priority", "",
            "The run's process priority.")
