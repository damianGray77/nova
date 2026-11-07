#pragma once

// The live novelty meter (docs/analysis-plan.md A2, the live part;
// docs/results/viewer.md): an observer in the run, never in the world.
//
// The run feeds it every genebank save as it drains the detectors, in
// genebank.txt's order (tick, then node, then save), and every replicator gate
// event. Each new genotype goes through NoveltyMeter (analysis.h) exactly as
// `nova analyse novelty` streams genebank.txt: op symbols in codon order, a
// 4-codon model, surprise measured before it learns, a genotype saved again on
// another node not scored again. So a window's figures are the offline tool's.
//
// Readers ("taps": stats.csv's rows, the viewer's charts) each take their own
// window, and each window gives:
//   novelty        innovation: the mean surprise of its new genotypes, bits per codon
//   novel_share    the share of them at or above novelty.threshold
//   novel_success  how many of those novel ones belong to a lineage that has met
//                  the gate by the window's end
// The offline tool counts a lineage that meets the gate after the window too (it
// knows the future); `nova analyse novelty --as-live` counts as this does.
//
// Not hashed and not snapshotted: a resumed run starts a fresh model (and says
// so), as detection itself starts afresh.
#include "nova/analysis.h"
#include "nova/hashmap.h"
#include "nova/isa.h"

namespace nova {

// One tap's window: the counts behind the three columns.
struct NoveltyTake {
    u64 fresh = 0;         // new genotypes
    u64 novel = 0;         // ... at or above the threshold
    u64 novel_success = 0; // ... of them, in a lineage that has met the gate by the take
    double surprise = 0;   // summed over the new genotypes, bits per codon each
    double innovation() const { return fresh ? surprise / static_cast<double>(fresh) : 0.0; }
    double novel_share() const { return fresh ? static_cast<double>(novel) / static_cast<double>(fresh) : 0.0; }
};

class LiveNovelty {
  public:
    enum : u32 { kTapStats, kTapView, kTaps };

    // The run's codon table (op symbols in codon order, as OpAlphabet makes
    // them) and novelty.threshold. False if the model refuses the alphabet.
    bool init(const CodonTable& t, double threshold);
    double threshold() const { return threshold_; }

    // A replicator gate event: the lineage met the gate.
    void gated(u64 lineage) { gated_.get_or_insert(lineage); }
    bool is_gated(u64 lineage) const { return gated_.contains(lineage); }
    u64 gated_count() const { return gated_.size(); }

    // One genebank save, in genebank.txt's order: its surprise in bits per
    // codon, or -1 for a genotype already scored (saved again elsewhere).
    double save(u64 tick, u64 genotype, u64 lineage, const u8* codons, u32 len);

    // A reader's window starts now (opened taps then always keep one).
    void open_tap(u32 tap);
    bool tap_open(u32 tap) const { return tap < kTaps && taps_[tap].open; }
    // The window since the tap's last take, its novel_success counted against
    // the gated lineages now; starts the next window. Zeros for a closed tap.
    NoveltyTake take(u32 tap);

    // Since the start: saves seen, new genotypes scored.
    u64 saves() const { return meter_.total().saves; }
    u64 fresh() const { return meter_.total().fresh; }

  private:
    struct Tap {
        bool open = false;
        NoveltyTake t;
        Array<u64> novel_lineage; // each novel genotype's lineage this window
    };

    NoveltyMeter meter_;
    HashMap<u8> gated_;
    u8 sym_[64] = {};
    u32 alphabet_ = 0;
    double threshold_ = 3.0;
    Array<u8> buf_;
    Tap taps_[kTaps];
};

} // namespace nova
