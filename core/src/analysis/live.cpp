// The live novelty meter (docs/analysis-plan.md A2, the live part): the
// offline NoveltyMeter fed by the run as the genebank saves genotypes. It only
// observes: nothing here is read by the world, hashed or snapshotted.
#include "nova/novelty.h"

namespace nova {

bool LiveNovelty::init(const CodonTable& t, double threshold) {
    // Op symbols in codon order, exactly as OpAlphabet::init numbers them, so
    // a genome's codons give the symbols its genebank.txt op names give.
    u32 sym_of[kOpCount];
    for (u32 i = 0; i < kOpCount; ++i)
        sym_of[i] = 0xFFFFFFFFu;
    alphabet_ = 0;
    for (u32 c = 0; c < 64; ++c) {
        const u32 op = static_cast<u32>(t.op[c]);
        if (op < kOpCount && sym_of[op] == 0xFFFFFFFFu)
            sym_of[op] = alphabet_++;
    }
    for (u32 c = 0; c < 64; ++c) {
        const u32 op = static_cast<u32>(t.op[c]);
        sym_[c] = static_cast<u8>(op < kOpCount ? sym_of[op] : 0);
    }
    threshold_ = threshold;
    NoveltyOptions o;
    o.threshold = threshold;
    o.window = ~0ull; // one window for ever: the taps keep the windows
    o.top = 0;
    gated_.clear();
    for (u32 k = 0; k < kTaps; ++k) {
        taps_[k].open = false;
        taps_[k].t = NoveltyTake{};
        taps_[k].novel_lineage.clear();
    }
    return meter_.init(alphabet_, o);
}

double LiveNovelty::save(u64 tick, u64 genotype, u64 lineage, const u8* codons, u32 len) {
    if (len == 0)
        return -1; // (the offline tool skips an empty genome)
    buf_.resize(len);
    for (u32 i = 0; i < len; ++i)
        buf_[i] = sym_[codons[i] & 63];
    const double s = meter_.add(tick, genotype, buf_.data(), len, false);
    if (s < 0)
        return s;
    const bool novel = s >= threshold_;
    for (u32 k = 0; k < kTaps; ++k) {
        Tap& t = taps_[k];
        if (!t.open)
            continue;
        ++t.t.fresh;
        t.t.surprise += s; // summed from 0 each window, in save order: the offline tool's sum, bit for bit
        if (novel) {
            ++t.t.novel;
            t.novel_lineage.push(lineage);
        }
    }
    return s;
}

void LiveNovelty::open_tap(u32 tap) {
    if (tap >= kTaps)
        return;
    taps_[tap].open = true;
    taps_[tap].t = NoveltyTake{};
    taps_[tap].novel_lineage.clear();
}

NoveltyTake LiveNovelty::take(u32 tap) {
    if (tap >= kTaps || !taps_[tap].open)
        return NoveltyTake{};
    Tap& t = taps_[tap];
    NoveltyTake out = t.t;
    out.novel_success = 0;
    for (usize i = 0; i < t.novel_lineage.size(); ++i)
        out.novel_success += gated_.contains(t.novel_lineage[i]) ? 1 : 0;
    t.t = NoveltyTake{};
    t.novel_lineage.clear();
    return out;
}

} // namespace nova
