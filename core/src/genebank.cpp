#include "nova/genebank.h"

#include "nova/hash.h"

namespace nova {

u64 genotype_hash(const u8* codons, usize n) {
    return hash_bytes(codons, n, 0x6E6F76612D67656Eull); // "nova-gen"
}

void Genebank::clear(u32 threshold) {
    map_.clear();
    next_name_.clear();
    threshold_ = threshold ? threshold : 1;
    saved_ = 0;
}

bool Genebank::observe(u64 genotype, const GenotypeMeta& meta) {
    bool inserted = false;
    Entry& e = map_.get_or_insert(genotype, &inserted);
    if (inserted) {
        e.meta = meta;
        e.name = kNoName;
    }
    ++e.count;
    if (!e.saved && e.count >= threshold_) {
        e.saved = 1;
        ++saved_;
        // e stays valid: get_or_insert on next_name_ touches the other map.
        u32& next = next_name_.get_or_insert(e.meta.len);
        e.name = next++;
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

u32 Genebank::name_index(u64 genotype) const {
    const Entry* e = map_.find(genotype);
    return e && e->saved ? e->name : kNoName;
}

const GenotypeMeta* Genebank::meta(u64 genotype) const {
    const Entry* e = map_.find(genotype);
    return e ? &e->meta : nullptr;
}

void Genebank::format_name(u32 len, u32 index, TextBuf& out) {
    char digits[16];
    int n = 0;
    u32 v = len;
    do {
        digits[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    for (int i = n; i < 4; ++i)
        out.append_char('0');
    while (n > 0)
        out.append_char(digits[--n]);
    // Base-26 letters, at least three: aaa, aab, ... zzz, then baaa, ...
    char letters[16];
    int m = 0;
    u32 x = index;
    do {
        letters[m++] = static_cast<char>('a' + x % 26);
        x /= 26;
    } while (x);
    for (int i = m; i < 3; ++i)
        out.append_char('a');
    while (m > 0)
        out.append_char(letters[--m]);
}

} // namespace nova
