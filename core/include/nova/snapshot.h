#pragma once

// Versioned binary snapshots (design.md 7.1 H).
//
// Format v2 (v1 is read too, see kSnapshotVersion), all integers little-endian:
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
inline constexpr u32 kSnapshotVersion = 2;
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
