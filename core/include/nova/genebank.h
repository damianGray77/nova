#pragma once

// Tierra-style genebank (design.md 7.1 H; Phase 1b, design.md 6.6).
//
// Tracks the living count of each genotype it is shown, and saves a genotype
// once, the first time its count reaches the threshold, instead of logging
// every individual (Tierra's rule). A saved genotype gets a Tierra-style name:
// its length as four digits, then three letters in order of saving among
// genotypes of that length ("0008aaa", "0008aab", ...). An unsaved genotype is
// forgotten when its count drops to zero; a saved one stays known, so it is
// never saved twice.
//
// The replicator detector (lineage.h) feeds it every lineage member, with the
// member's parent genotype and lineage, and writes each save to genebank.txt.
#include "nova/hashmap.h"
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

// Genotype identity: a hash of the codon sequence.
u64 genotype_hash(const u8* codons, usize n);

// What is known about a genotype from its first sighting.
struct GenotypeMeta {
    u64 parent;     // the parent's genotype (0 = none: a lineage founder)
    u64 first_tick; // tick of the first sighting
    u64 lineage;    // the lineage it first appeared in (world-unique id)
    u32 len;        // codons
    u32 pad;
};

class Genebank {
  public:
    static constexpr u32 kNoName = 0xFFFFFFFFu;

    explicit Genebank(u32 threshold = 10) : threshold_(threshold ? threshold : 1) {}

    // Forgets everything and sets a new threshold.
    void clear(u32 threshold);

    // One more living instance. Returns true exactly once per genotype: when its
    // count first reaches the threshold (the "save it now" moment). meta is kept
    // from the first sighting only.
    bool observe(u64 genotype, const GenotypeMeta& meta);
    bool observe(u64 genotype) { return observe(genotype, GenotypeMeta{}); }
    // One instance died. Unsaved genotypes are forgotten when their count hits 0;
    // saved ones stay, so they are never "saved" twice.
    void release(u64 genotype);

    u32 count(u64 genotype) const;
    bool is_saved(u64 genotype) const;
    // The saved genotype's name index among genotypes of its length (0 = "aaa"),
    // or kNoName if it is not saved.
    u32 name_index(u64 genotype) const;
    // Meta from the first sighting, or nullptr if the genotype is not tracked.
    const GenotypeMeta* meta(u64 genotype) const;

    usize tracked() const { return map_.size(); }
    usize saved() const { return saved_; }
    u32 threshold() const { return threshold_; }

    // Writes a Tierra-style name: "0008aab" for length 8, index 1. Lengths past
    // 9999 print in full; indices past 17575 ("zzz") gain letters.
    static void format_name(u32 len, u32 index, TextBuf& out);

  private:
    struct Entry {
        GenotypeMeta meta;
        u32 count;
        u32 saved;
        u32 name;
        u32 pad;
    };
    HashMap<Entry> map_;
    HashMap<u32> next_name_; // per length: the next name index
    u32 threshold_;
    usize saved_ = 0;
};

} // namespace nova
