#pragma once

// Versioned binary snapshots (design.md 7.1 H).
//
// Format v6 (v5 back to v1 are read too, see kSnapshotVersion), all integers little-endian:
//   "NOVASNAP"   8-byte magic
//   u32          format version (kSnapshotVersion)
//   u32          flags (0)
//   u64          seed       } duplicated from the world section so a file can be
//   u64          tick       } identified without parsing the rest
//   str          config text: the *effective* config, knob changes included
//   ...          world section (World::save_state)
//   u64          checksum: hash of every preceding byte
//
// No RNG state is stored: the RNG is counter-based, so seed + tick are enough
// to continue every stream exactly. Snapshots are taken at the tick barrier,
// when every outbox is empty.
#include "nova/config.h"
#include "nova/io.h"
#include "nova/world.h"

namespace nova {

// v2 (Phase 10 profiling): each soup also keeps its pages' heat inputs carried
// into the next tick (Soup::save). v1 is still read, those inputs as zero (as
// v1 builds resumed it: the world then drifts from the run that wrote it).
// v3 (docs/dormancy-plan.md D7): the lifecycle section is kept only with wear
// on (node.endurance > 0); before, every map world had one for its starvation
// meter, which v2 and v1 snapshots still carry and Soup::load drops.
// v4 (docs/world-plan.md W9; one bump shared by the varied world's features
// and the disasters):
//   - substrates (W2): with substrates on, each node's substrate levels after
//     the lifecycle section (checked against those the config gives);
//   - disk tiers (W4): each soup's disk section holds the archive after the
//     fast disk (contents, owners, extents and stamps in the same arrays),
//     and a cargo carries its archive share;
//   - disasters (docs/disasters-plan.md X9): with disasters on, each soup's
//     offline cores after its page heat, and the world's reboots, symptom
//     episodes and storms at the end of the world section; a dormant
//     program's burnout flags ride in Dormant (soup.h).
// Snapshots before v4 resume with substrates, sparks by activity, the archive
// and the disasters off (substrate.enabled = false, spark.activity = 0,
// world.archive = 0, disaster.enabled = false) unless their config or an edit
// names them: the legacy defaults, kept in one place for snapshots and replay
// logs alike (legacy.h; Run::init_from_snapshot).
// v5 (docs/lifehistory-plan.md W5): with wipe warnings on (mem.wipe_warning >
// 0), each soup's pending wipes after its offline cores. v4 files load with
// none pending (a v4 run had no warnings: the key came with v5).
// v6 (2026-11-08) changes no section: rays came on by default, so a v5
// snapshot or older resumes with hazard.rays = false unless its config names
// it (legacy.h).
inline constexpr u32 kSnapshotVersion = 6;
inline constexpr u32 kSnapshotOldest = 1;

void snapshot_write(const World& world, const Config& cfg, ByteWriter& out);

struct SnapshotHeader {
    u32 version = 0;
    u64 seed = 0;
    u64 tick = 0;
    Str config_text;        // view into the snapshot bytes
    usize world_offset = 0; // world section: [world_offset, world_offset + world_size)
    usize world_size = 0;
};

// Checks magic, version, structure and checksum. Does not build a world.
bool snapshot_parse(const u8* data, usize n, SnapshotHeader* h, TextBuf* err);

} // namespace nova
