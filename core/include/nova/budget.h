#pragma once

// Budgets (Phase 7b, design.md 6.13; plan C7, C9): settings given as a share
// of the machine or a fixed amount, turned into ordinary settings before the
// world is built.
//
//   budget.memory = off | P% | <amount>   -> world.soup_size (the mean per node)
//       P% of the physical memory available at start; an amount in bytes, or
//       with KB, MB, GB or TB. The world's codons are real RAM: about
//       kRamPerCodon bytes each with their bookkeeping.
//   budget.disk = off | P% | <amount>     -> world.disk (the mean per node)
//       needs disk.path: the disk tier lives in files there (soup.h). P% of
//       the free space on that volume.
//   budget.gpu, budget.npu = off | P%     -> gpu.elements, npu.macs
//       async mode (the real devices): a short calibration measures the
//       device and the CPU, and sizes the world so a fully busy world keeps
//       the device about P% busy at the tick rate the CPU allows.
//
// A resolved key becomes `off`, the request is kept as budget.<x>_was, and
// the result is an ordinary setting (replacing any value it had): the run's
// config and replay log then rebuild exactly the same world, on any machine.
//
// Then the checks that keep a run from crashing or freezing the machine
// (C9): the world's RAM must fit in 90% of what is available, and a disk
// file in 95% of its volume's free space. A world that doesn't fit is
// refused with what it would need.
#include "nova/config.h"
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

// RAM a soup codon costs with its share of the bookkeeping, with headroom
// for full populations (measured at 15-19 in Phase 7a).
inline constexpr double kRamPerCodon = 20.0;

// The machine (Win32).
u64 memory_available(); // physical memory available now, bytes
u64 memory_physical();  // installed
// Free bytes on the volume holding `path` (a directory that exists).
bool volume_free(const char* path, u64* bytes);

// File bytes a disk codon costs in a disk file: its contents, owner and used
// bit, and its salvage stamp when the rule is on.
double disk_bytes_per_codon(bool salvage);

// An amount: "P%" (of `whole`), or bytes with an optional KB/MB/GB/TB.
bool parse_amount(Str s, double whole, double* bytes);

// Resolves budget.* in cfg (marking the keys used) and checks the world fits
// the machine. `threads` is what the run will use (for the device budgets'
// CPU estimate). `notes` gets one line per budget resolved.
bool resolve_budgets(Config& cfg, u32 threads, TextBuf* notes, TextBuf* err);

} // namespace nova
