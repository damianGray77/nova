// The data side's genebank and compressibility estimate (design.md 7.1 H,
// 6.4 #24), built for real in Phase 1b.
#include "check.h"
#include "nova/compress.h"
#include "nova/genebank.h"
#include "nova/rng.h"

using namespace nova;

namespace {

void test_genebank() {
    const u8 g1[] = {1, 2, 3, 4};
    const u8 g2[] = {1, 2, 3, 5};
    u64 a = genotype_hash(g1, sizeof g1);
    u64 b = genotype_hash(g2, sizeof g2);
    CHECK(a != b);
    CHECK(a == genotype_hash(g1, sizeof g1));

    Genebank bank(3);
    CHECK(!bank.observe(a, GenotypeMeta{0, 10, 1, 4}));
    CHECK(!bank.observe(a, GenotypeMeta{0, 99, 7, 4})); // meta is kept from the first sighting
    CHECK(bank.name_index(a) == Genebank::kNoName);
    CHECK(bank.observe(a));  // reaches the threshold: save now
    CHECK(!bank.observe(a)); // only once
    CHECK(bank.count(a) == 4 && bank.is_saved(a) && bank.saved() == 1);
    CHECK(bank.name_index(a) == 0 && bank.meta(a)->first_tick == 10 && bank.meta(a)->lineage == 1);

    CHECK(!bank.observe(b, GenotypeMeta{a, 20, 1, 4}));
    bank.release(b); // unsaved and extinct: forgotten
    CHECK(bank.count(b) == 0 && bank.tracked() == 1 && bank.meta(b) == nullptr);

    for (int i = 0; i < 4; ++i)
        bank.release(a); // saved genotypes stay known at count 0
    CHECK(bank.count(a) == 0 && bank.is_saved(a) && bank.tracked() == 1);
    CHECK(!bank.observe(a) && !bank.observe(a) && !bank.observe(a)); // never saved twice

    // Names count per length: a second length-4 genotype is "aab", the first
    // length-5 one "aaa" again.
    const u8 g3[] = {9, 9, 9, 9, 9};
    u64 c = genotype_hash(g3, sizeof g3);
    for (int i = 0; i < 3; ++i)
        bank.observe(b, GenotypeMeta{a, 30, 1, 4});
    for (int i = 0; i < 3; ++i)
        bank.observe(c, GenotypeMeta{0, 30, 2, 5});
    CHECK(bank.name_index(b) == 1 && bank.name_index(c) == 0);
    TextBuf names;
    Genebank::format_name(4, bank.name_index(b), names);
    names.append_char(' ');
    Genebank::format_name(5, bank.name_index(c), names);
    CHECK(names.view().eq("0004aab 0005aaa"));

    Genebank zero(0);
    CHECK(zero.threshold() == 1 && zero.observe(b));
    zero.clear(5);
    CHECK(zero.tracked() == 0 && zero.threshold() == 5 && zero.saved() == 0);
}

void test_compressibility() {
    // Uniformly random codons: about 1.
    Array<u8> soup;
    soup.resize(65536);
    Rng r(1, 2, 3);
    for (usize i = 0; i < soup.size(); ++i)
        soup[i] = static_cast<u8>(r.next() & 63);
    Compressibility c = estimate_compressibility(soup.data(), soup.size());
    CHECK(c.measured && c.ratio > 0.95 && c.ratio < 1.05);

    // A soup full of copies of one 8-codon genome: far below 1.
    const u8 genome[8] = {26, 28, 2, 20, 34, 0, 26, 30};
    for (usize i = 0; i < soup.size(); ++i)
        soup[i] = genome[i % 8];
    Compressibility rep = estimate_compressibility(soup.data(), soup.size());
    CHECK(rep.measured && rep.ratio < 0.02 && rep.matches > 0);

    // Half random, half copies: in between, ordered.
    for (usize i = 0; i < soup.size() / 2; ++i)
        soup[i] = static_cast<u8>(r.next() & 63);
    Compressibility half = estimate_compressibility(soup.data(), soup.size());
    CHECK(half.ratio > rep.ratio && half.ratio < c.ratio && half.ratio > 0.4 && half.ratio < 0.6);

    // Degenerate inputs.
    CHECK(estimate_compressibility(soup.data(), 0).ratio == 1.0);
    CHECK(estimate_compressibility(soup.data(), 3).measured);
}

} // namespace

void suite_genebank() {
    test_genebank();
    test_compressibility();
}
