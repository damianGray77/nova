#pragma once

// The settings older files ran on (docs/results/integration.md).
//
// Features that are on in the engine's defaults weren't there, or were off,
// when older snapshots and replay logs were written. Such a file names only
// the settings it was given, so on today's defaults it would resume or replay
// a different world. So a snapshot before format v4, or a replay log before
// format v3, takes each setting it doesn't name from kLegacyFormatDefaults
// below -- the old value of every default changed since -- not from the
// registry. Snapshots (Run::init_from_snapshot) and replay logs
// (ReplayLog::replay_config) both read it from here, each by its own format
// version. The test runner's legacy fallback (tests/test_main.cpp) has the
// same lines among its own.
//
// ADDING A FEATURE THAT IS ON BY DEFAULT: add its "off" line to the text,
// with the plan it comes from (as the disasters and the memory tiers did).
// That holds while the build that bumped
// both formats (snapshot v4, replay v3) is being put together. Once runs have
// been recorded with it, a later default change needs another bump of both
// formats and a text of its own for the files between.
#include "nova/config.h"
#include "nova/types.h"

namespace nova {

inline constexpr u32 kSnapshotLegacyBefore = 4; // snapshots before format v4 take the text below
inline constexpr u32 kReplayLegacyBefore = 3;   // and replay logs before format v3

inline constexpr const char* kLegacyFormatDefaults = //
    "spark.activity = 0\n"                           // sparks by activity (docs/world-plan.md W5)
    "substrate.enabled = false\n"                    // substrates (W2)
    "world.archive = 0\n"                            // the archive, the disk tiers' second tier (W4)
    "disaster.enabled = false\n"                     // the disasters (docs/disasters-plan.md X1-X11)
    "mem.fast_share = 0\n"                           // memory tiers (docs/world-plan.md W3)
    "profile.enabled = false\n"                     // observers start disabled for old files
    "novelty.enabled = false\n";

// Sets each line of kLegacyFormatDefaults that cfg doesn't name, where the
// world will read it, so no unused entry is added: every line is a soup
// world's setting, and one whose registry entry needs a choice
// (substrate.enabled: world.graph=procedural|landscape) is set only where
// that choice is made.
void apply_legacy_defaults(Config& cfg);

} // namespace nova
