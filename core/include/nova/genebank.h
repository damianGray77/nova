#pragma once

// Tierra-style genebank (design.md 7.1 H) -- Phase 0 STUB.
//
// What exists: per-genotype population counts, and the moment a genotype first
// reaches the save threshold (Tierra saves a genotype once, when its count
// passes a threshold, instead of logging every individual).
//
// What Phase 1 adds: actually writing the saved genome to disk, genotype naming,
// and genotype-to-genotype lineage. There are no genomes to save until the
// interpreter exists.
#include "nova/hashmap.h"
#include "nova/types.h"

namespace nova {

// Genotype identity: a hash of the codon sequence.
u64 genotype_hash(const u8* codons, usize n);

class Genebank {
  public:
    explicit Genebank(u32 threshold = 10) : threshold_(threshold ? threshold : 1) {}

    // One more living instance. Returns true exactly once per genotype: when its
    // count first reaches the threshold (the "save it now" moment).
    bool observe(u64 genotype);
    // One instance died. Unsaved genotypes are forgotten when their count hits 0;
    // saved ones stay, so they are never "saved" twice.
    void release(u64 genotype);

    u32 count(u64 genotype) const;
    bool is_saved(u64 genotype) const;
    usize tracked() const { return map_.size(); }
    usize saved() const { return saved_; }
    u32 threshold() const { return threshold_; }

  private:
    struct Entry {
        u32 count;
        u32 saved;
    };
    HashMap<Entry> map_;
    u32 threshold_;
    usize saved_ = 0;
};

} // namespace nova
