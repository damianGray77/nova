#include "nova/genebank.h"

#include "nova/hash.h"

namespace nova {

u64 genotype_hash(const u8* codons, usize n) {
    return hash_bytes(codons, n, 0x6E6F76612D67656Eull); // "nova-gen"
}

bool Genebank::observe(u64 genotype) {
    Entry& e = map_.get_or_insert(genotype);
    ++e.count;
    if (!e.saved && e.count >= threshold_) {
        e.saved = 1;
        ++saved_;
        return true;
    }
    return false;
}

void Genebank::release(u64 genotype) {
    Entry* e = map_.find(genotype);
    NOVA_ASSERT(e && e->count > 0);
    if (!e || e->count == 0)
        return;
    if (--e->count == 0 && !e->saved)
        map_.erase(genotype);
}

u32 Genebank::count(u64 genotype) const {
    const Entry* e = map_.find(genotype);
    return e ? e->count : 0;
}

bool Genebank::is_saved(u64 genotype) const {
    const Entry* e = map_.find(genotype);
    return e && e->saved;
}

} // namespace nova
