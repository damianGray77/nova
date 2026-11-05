#include "check.h"
#include "nova/compress.h"
#include "nova/genebank.h"

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
    CHECK(!bank.observe(a));
    CHECK(!bank.observe(a));
    CHECK(bank.observe(a));  // reaches the threshold: save now
    CHECK(!bank.observe(a)); // only once
    CHECK(bank.count(a) == 4 && bank.is_saved(a) && bank.saved() == 1);

    CHECK(!bank.observe(b));
    bank.release(b); // unsaved and extinct: forgotten
    CHECK(bank.count(b) == 0 && bank.tracked() == 1);

    for (int i = 0; i < 4; ++i)
        bank.release(a); // saved genotypes stay known at count 0
    CHECK(bank.count(a) == 0 && bank.is_saved(a) && bank.tracked() == 1);
    CHECK(!bank.observe(a) && !bank.observe(a) && !bank.observe(a)); // never saved twice

    Genebank zero(0);
    CHECK(zero.threshold() == 1 && zero.observe(b));
}

void test_compressibility_stub() {
    u8 soup[64] = {};
    Compressibility c = estimate_compressibility(soup, sizeof soup);
    CHECK(!c.measured); // a stub must never pass for a reading
}

} // namespace

void suite_stubs() {
    test_genebank();
    test_compressibility_stub();
}
