// n-gram counts with Witten-Bell smoothing (docs/analysis-plan.md A0, for A2).
#include <cmath> // log2

#include "nova/learn.h"

namespace nova {

bool NgramModel::init(u32 alphabet, u32 order) {
    if (alphabet < 2 || alphabet > kMaxAlphabet || order < 1 || order > kMaxOrder)
        return false;
    alphabet_ = alphabet;
    order_ = order;
    learned_ = 0;
    usize contexts = 1;
    for (u32 k = 0; k < kMaxOrder; ++k) {
        if (k < order) {
            count_[k].resize(contexts * alphabet);
            count_[k].fill(0);
            total_[k].resize(contexts);
            total_[k].fill(0);
            types_[k].resize(contexts);
            types_[k].fill(0);
        } else {
            count_[k].resize(0);
            total_[k].resize(0);
            types_[k].resize(0);
        }
        contexts *= alphabet;
    }
    return true;
}

double NgramModel::prob(const u8* ctx, u32 ctx_len, u32 sym) const {
    NOVA_ASSERT(sym < alphabet_);
    const u32 V = alphabet_;
    double p = 1.0 / V;
    const u32 kmax = ctx_len < order_ - 1 ? ctx_len : order_ - 1;
    for (u32 k = 0; k <= kmax; ++k) {
        usize h = 0; // the last k symbols of ctx
        for (u32 i = ctx_len - k; i < ctx_len; ++i)
            h = h * V + ctx[i];
        const u64 N = total_[k].data()[h];
        if (N == 0)
            break; // a longer context ending in an unseen one is unseen too
        const u32 T = types_[k].data()[h];
        const u32 c = count_[k].data()[h * V + sym];
        p = (static_cast<double>(c) + static_cast<double>(T) * p) / (static_cast<double>(N) + T);
    }
    return p;
}

double NgramModel::bits(const u8* s, u32 len) const {
    double b = 0;
    for (u32 i = 0; i < len; ++i)
        b -= log2(prob(s, i, s[i]));
    return b;
}

void NgramModel::learn(const u8* s, u32 len) {
    const u32 V = alphabet_;
    for (u32 i = 0; i < len; ++i) {
        NOVA_ASSERT(s[i] < V);
        const u32 kmax = i < order_ - 1 ? i : order_ - 1;
        for (u32 k = 0; k <= kmax; ++k) {
            usize h = 0;
            for (u32 j = i - k; j < i; ++j)
                h = h * V + s[j];
            u32& c = count_[k].data()[h * V + s[i]];
            if (c == 0)
                ++types_[k].data()[h];
            if (c != 0xFFFFFFFFu) // saturates: never above its total
                ++c;
            ++total_[k].data()[h];
        }
    }
    ++learned_;
}

} // namespace nova
