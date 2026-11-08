#include "nova/legacy.h"

#include "nova/keys.h"

namespace nova {

namespace {

// Whether a soup world described by cfg reads `key`: always, unless its
// registry entry needs a choice ("world.graph=procedural|landscape"), which
// must then be one of those. (A switch it needs is left alone: an "off"
// value under it is read or harmless.)
bool world_reads(Config& cfg, Str key) {
    const Key* k = key_find(key);
    if (!k)
        return false;
    const Str needs(k->needs);
    const usize eq = needs.find('=');
    if (eq == needs.n)
        return true;
    const Str chosen = key_peek_str(cfg, needs.sub(0, eq));
    Str options = needs.sub(eq + 1, needs.n - eq - 1);
    while (options.n) {
        const usize bar = options.find('|');
        if (options.sub(0, bar).eq(chosen))
            return true;
        options = bar < options.n ? options.sub(bar + 1, options.n - bar - 1) : Str();
    }
    return false;
}

void apply_text(Config& cfg, const char* text) {
    if (!key_peek_str(cfg, "world.model").eq("soup")) // (every line is a soup world's setting)
        return;
    Config legacy;
    Config::ParseError pe;
    const bool parsed = legacy.parse(Str(text), &pe);
    NOVA_ASSERT(parsed);
    (void)parsed;
    for (usize i = 0; i < legacy.count(); ++i)
        if (!cfg.has(legacy.key(i)) && world_reads(cfg, legacy.key(i)))
            cfg.set(legacy.key(i), legacy.value(i));
}

} // namespace

void apply_legacy_defaults(Config& cfg) { apply_text(cfg, kLegacyFormatDefaults); }

void apply_format_defaults(Config& cfg, bool snapshot, u32 version) {
    if (version < (snapshot ? kSnapshotLegacyBefore : kReplayLegacyBefore))
        apply_text(cfg, kLegacyFormatDefaults);
    if (version < (snapshot ? kSnapshotRaysBefore : kReplayRaysBefore))
        apply_text(cfg, kLegacyRaysDefaults);
}

} // namespace nova
