// Test entry point: `nova_tests` runs every suite; `nova_tests <suite>` runs one.
#include <cstdio>
#include <cstring>

#include "check.h"

void suite_containers();
void suite_str();
void suite_rng();
void suite_config();
void suite_pool();
void suite_world();
void suite_gate();
void suite_snapshot();
void suite_stats();
void suite_replay();
void suite_stubs();

namespace {

struct Suite {
    const char* name;
    void (*fn)();
};

const Suite kSuites[] = {
    {"containers", suite_containers},
    {"str", suite_str},
    {"rng", suite_rng},
    {"config", suite_config},
    {"pool", suite_pool},
    {"world", suite_world},
    {"gate", suite_gate},
    {"snapshot", suite_snapshot},
    {"stats", suite_stats},
    {"replay", suite_replay},
    {"stubs", suite_stubs},
};

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        for (const Suite& s : kSuites) {
            if (strcmp(argv[1], s.name) == 0) {
                s.fn();
                return ntest::summary(s.name);
            }
        }
        fprintf(stderr, "unknown suite '%s'\n", argv[1]);
        return 2;
    }
    for (const Suite& s : kSuites)
        s.fn();
    return ntest::summary("all");
}
