// nova analyse and nova search: learning from the runs (docs/analysis-plan.md;
// docs/results/analysis.md). The work is in core (nova/analysis.h); this is
// the command line around it.
//
//   nova analyse novelty <run> [--window N] [--threshold BITS] [--top K] [--out DIR]
//   nova analyse tree <run> (--lineage N.K | --sample N) [--max N] [--neighbours N]
//                     [--pool N] [--seed S] [--threads N] [--out DIR]
//   nova analyse scores <run> [--body-floor N] [--threshold BITS]
//   nova analyse lineages <run> [--top K]
//   nova search <search file> [--dry-run]
//
// The tools only read a run's files, with shared access, so a live run can be
// analysed while it writes them. They never write into a run folder: output
// goes to --out, by default analysis\<run folder's name> under the current
// directory.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "app.h"
#include "nova/analysis.h"
#include "nova/io.h"

using namespace nova;

namespace {

int analyse_usage() {
    fprintf(stderr,
            "usage:\n"
            "  nova analyse novelty <run> [--window N] [--threshold BITS] [--top K] [--out DIR]\n"
            "  nova analyse tree <run> (--lineage N.K | --sample N) [--max N] [--neighbours N] [--pool N]\n"
            "                    [--seed S] [--threads N] [--out DIR]\n"
            "  nova analyse scores <run> [--body-floor N] [--threshold BITS]\n"
            "  nova analyse lineages <run> [--top K]   (the lineages with the most saved genotypes)\n"
            "  nova search <search file> [--dry-run]\n"
            "  (output goes to --out, by default analysis\\<run folder's name>; never into the run)\n");
    return 2;
}

// The output folder: --out, or analysis\<the run folder's last part>.
bool out_dir(const char* run, const char* given, TextBuf* out) {
    out->clear();
    if (given) {
        out->append(given);
    } else {
        Str r(run);
        while (r.n && (r.p[r.n - 1] == '\\' || r.p[r.n - 1] == '/'))
            --r.n;
        usize s = r.n;
        while (s > 0 && r.p[s - 1] != '\\' && r.p[s - 1] != '/' && r.p[s - 1] != ':')
            --s;
        out->append("analysis\\");
        out->append(r.sub(s, r.n - s).n ? r.sub(s, r.n - s) : Str("run"));
    }
    if (path_inside(out->c_str(), run)) {
        fprintf(stderr, "nova analyse: '%s' is inside the run folder; the analysis never writes into a run\n",
                out->c_str());
        return false;
    }
    if (!make_dirs(out->c_str())) {
        fprintf(stderr, "nova analyse: cannot create '%s'\n", out->c_str());
        return false;
    }
    return true;
}

bool write_out(TextBuf& dir, const char* leaf, Str text) {
    TextBuf p;
    path_join(p, dir.view(), leaf);
    if (!write_file(p.c_str(), text.p, text.n)) {
        fprintf(stderr, "nova analyse: cannot write '%s'\n", p.c_str());
        return false;
    }
    printf("nova analyse: wrote %s\n", p.c_str());
    return true;
}

struct Opts {
    const char* out = nullptr;
    u64 window = 10000, top = 5, max = 2000, neighbours = 20, pool = 20000, seed = 1, threads = 1, sample = 0;
    double threshold = 3.0, body_floor = 0;
    bool lineage_given = false, top_given = false;
    u64 lineage = 0;
};

bool parse_opts(int argc, char** argv, int from, Opts* o) {
    for (int i = from; i < argc; ++i) {
        if (i + 1 >= argc) {
            fprintf(stderr, "nova analyse: '%s' needs a value\n", argv[i]);
            return false;
        }
        const char* a = argv[i];
        const Str v(argv[++i]);
        bool ok = true;
        if (strcmp(a, "--out") == 0)
            o->out = v.p;
        else if (strcmp(a, "--window") == 0)
            ok = parse_u64(v, &o->window) && o->window > 0;
        else if (strcmp(a, "--top") == 0)
            ok = o->top_given = parse_u64(v, &o->top) && o->top <= 1000;
        else if (strcmp(a, "--threshold") == 0)
            ok = parse_f64(v, &o->threshold) && o->threshold >= 0;
        else if (strcmp(a, "--max") == 0)
            ok = parse_u64(v, &o->max) && o->max >= 2 && o->max <= kTreeMaxLeaves;
        else if (strcmp(a, "--neighbours") == 0)
            ok = parse_u64(v, &o->neighbours) && o->neighbours <= kTreeMaxLeaves;
        else if (strcmp(a, "--pool") == 0)
            ok = parse_u64(v, &o->pool) && o->pool <= 10000000;
        else if (strcmp(a, "--seed") == 0)
            ok = parse_u64(v, &o->seed);
        else if (strcmp(a, "--threads") == 0)
            ok = parse_u64(v, &o->threads) && o->threads >= 1 && o->threads <= 1024;
        else if (strcmp(a, "--sample") == 0)
            ok = parse_u64(v, &o->sample) && o->sample >= 2 && o->sample <= kTreeMaxLeaves;
        else if (strcmp(a, "--body-floor") == 0)
            ok = parse_f64(v, &o->body_floor) && o->body_floor >= 0;
        else if (strcmp(a, "--lineage") == 0)
            ok = o->lineage_given = parse_lineage_id(v, &o->lineage);
        else {
            fprintf(stderr, "nova analyse: unknown option '%s'\n", a);
            return false;
        }
        if (!ok) {
            fprintf(stderr, "nova analyse: bad value '%s' for %s\n", v.p, a);
            return false;
        }
    }
    return true;
}

int analyse_novelty(const char* run, const Opts& op) {
    NoveltyOptions o;
    o.window = op.window;
    o.threshold = op.threshold;
    o.top = static_cast<u32>(op.top);
    TextBuf dir;
    if (!out_dir(run, op.out, &dir))
        return 1;
    printf("nova analyse: novelty of %s (4-codon n-grams, windows of %llu ticks, novel at %.2f bits/codon)\n", run,
           static_cast<unsigned long long>(o.window), o.threshold);
    fflush(stdout);
    const double t0 = now_ms();
    NoveltyReport r;
    TextBuf err;
    if (!novelty_analyse(run, o, &r, &err)) {
        fprintf(stderr, "nova analyse: %s\n", err.c_str());
        return 1;
    }
    TextBuf csv, text;
    novelty_csv(r, csv);
    novelty_summary(r, text);
    fwrite(text.view().p, 1, text.size(), stdout);
    printf("nova analyse: %.1f s\n", (now_ms() - t0) / 1000.0);
    const bool ok = write_out(dir, "novelty.csv", csv.view()) && write_out(dir, "novelty.txt", text.view()) &&
                    (o.top == 0 || write_out(dir, "novelty_top.csv", r.top_csv.view()));
    return ok ? 0 : 1;
}

int analyse_tree(const char* run, const Opts& op) {
    if (op.lineage_given == (op.sample != 0)) {
        fprintf(stderr, "nova analyse tree: give one of --lineage N.K or --sample N\n");
        return 2;
    }
    TreeOptions o;
    o.by_lineage = op.lineage_given;
    o.lineage = op.lineage;
    o.sample = static_cast<u32>(op.sample);
    o.max = static_cast<u32>(op.max);
    o.neighbours = static_cast<u32>(op.neighbours);
    o.pool = static_cast<u32>(op.pool);
    o.seed = op.seed;
    o.threads = static_cast<u32>(op.threads);
    TextBuf dir;
    if (!out_dir(run, op.out, &dir))
        return 1;
    TextBuf stem;
    stem.append("tree_");
    if (o.by_lineage) {
        stem.append_u64(o.lineage >> 32);
        stem.append_char('_');
        stem.append_u64(o.lineage & 0xFFFFFFFFull);
    } else {
        stem.append("sample");
        stem.append_u64(o.sample);
    }
    printf("nova analyse: tree of %s (%s, at most %u genotypes)\n", run, stem.c_str() + 5, o.max);
    fflush(stdout);
    const double t0 = now_ms();
    TreeReport r;
    TextBuf err;
    if (!tree_analyse(run, o, &r, &err)) {
        fprintf(stderr, "nova analyse: %s\n", err.c_str());
        return 1;
    }
    printf("nova analyse: %u genotypes in the tree (%llu eligible%s)\n", r.leaves,
           static_cast<unsigned long long>(r.candidates),
           r.capped ? ", thinned to the cap: spread over the run, with parents kept beside children" : "");
    if (r.links)
        printf("nova analyse: tracker parent links in the tree: %u; the tree puts the parent nearest for %u "
               "(%.1f%%), among the 3 nearest for %u (%.1f%%); alignment alone, nearest for %u (%.1f%%); "
               "child-parent distance %.3f on average\n",
               r.links, r.agree, 100.0 * r.agree / r.links, r.agree3, 100.0 * r.agree3 / r.links, r.aligned_agree,
               100.0 * r.aligned_agree / r.links, r.parent_distance);
    else
        printf("nova analyse: no tracker parent link has both ends in the tree\n");
    printf("nova analyse: members with no parent (founders, and arrivals on other nodes): %u, %u linked to a likely "
           "ancestor (mean confidence %.2f)\n",
           r.founders, r.linked, r.mean_confidence);
    printf("nova analyse: %.1f s\n", (now_ms() - t0) / 1000.0);
    TextBuf leaf;
    leaf.append(stem.view());
    leaf.append(".nwk");
    TextBuf leaf2;
    leaf2.append(stem.view());
    leaf2.append("_links.csv");
    const bool ok = write_out(dir, leaf.c_str(), r.newick.view()) && write_out(dir, leaf2.c_str(), r.links_csv.view());
    return ok ? 0 : 1;
}

int analyse_scores(const char* run, const Opts& op) {
    ScoreOptions o;
    o.body_floor = op.body_floor;
    o.novelty_threshold = op.threshold;
    RunScores r;
    TextBuf err;
    const double t0 = now_ms();
    if (!score_run(run, o, &r, &err)) {
        fprintf(stderr, "nova analyse: %s\n", err.c_str());
        return 1;
    }
    printf("nova analyse: scores of %s over %llu ticks (%llu gated lineages, %llu genebank entries; body floor %g)\n",
           run, static_cast<unsigned long long>(r.ticks), static_cast<unsigned long long>(r.gated),
           static_cast<unsigned long long>(r.saves), r.body_floor);
    for (u32 i = 0; i < kScoreCount; ++i)
        printf("  %-12s %.6g\n", score_name(i), r.s[i]);
    printf("nova analyse: %.1f s\n", (now_ms() - t0) / 1000.0);
    return 0;
}

} // namespace

// nova analyse <task> <run folder> [options]
int cmd_analyse(int argc, char** argv) {
    if (argc < 4)
        return analyse_usage();
    Opts o;
    if (!parse_opts(argc, argv, 4, &o))
        return analyse_usage();
    const char* run = argv[3];
    if (!path_exists(run)) {
        fprintf(stderr, "nova analyse: no run folder '%s'\n", run);
        return 1;
    }
    if (strcmp(argv[2], "novelty") == 0)
        return analyse_novelty(run, o);
    if (strcmp(argv[2], "tree") == 0)
        return analyse_tree(run, o);
    if (strcmp(argv[2], "scores") == 0)
        return analyse_scores(run, o);
    if (strcmp(argv[2], "lineages") == 0) {
        TextBuf out, err;
        if (!top_lineages(run, static_cast<u32>(o.top_given ? o.top : 20), out, &err)) {
            fprintf(stderr, "nova analyse: %s\n", err.c_str());
            return 1;
        }
        fwrite(out.view().p, 1, out.size(), stdout);
        return 0;
    }
    return analyse_usage();
}

// nova search <search file> [--dry-run]
int cmd_search(int argc, char** argv) {
    if (argc < 3 || argc > 4 || (argc == 4 && strcmp(argv[3], "--dry-run") != 0))
        return analyse_usage();
    SearchSpec s;
    TextBuf err;
    if (!search_load(argv[2], &s, &err)) {
        fprintf(stderr, "nova search: %s\n", err.c_str());
        return 1;
    }
    char exe[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, exe, sizeof exe);
    if (n == 0 || n >= sizeof exe) {
        fprintf(stderr, "nova search: cannot find the nova program itself\n");
        return 1;
    }
    SearchOptions o;
    o.nova_exe = exe;
    o.dry_run = argc == 4;
    if (!search_run(s, o, &err)) {
        fprintf(stderr, "nova search: %s\n", err.c_str());
        return 1;
    }
    return 0;
}
