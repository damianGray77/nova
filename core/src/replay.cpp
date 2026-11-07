#include "nova/replay.h"

#include "nova/legacy.h"

namespace nova {

static_assert(kReplayLegacyBefore <= kReplayVersion,
              "replay logs before kReplayLegacyBefore take the legacy defaults (legacy.h)");

namespace {

bool parse_hex64(Str s, u64* out) {
    if (s.n != 16)
        return false;
    u64 v = 0;
    for (usize i = 0; i < 16; ++i) {
        char c = s.p[i];
        u64 d;
        if (c >= '0' && c <= '9')
            d = static_cast<u64>(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = static_cast<u64>(c - 'a' + 10);
        else
            return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

// Splits "first rest" at the first space; rest is trimmed.
void split_word(Str s, Str* first, Str* rest) {
    usize sp = s.find(' ');
    *first = s.sub(0, sp);
    *rest = sp < s.n ? s.sub(sp + 1, s.n - sp - 1).trim() : Str();
}

bool split_key_value(Str s, Str* key, Str* value) {
    usize eq = s.find('=');
    if (eq == s.n)
        return false;
    *key = s.sub(0, eq).trim();
    *value = s.sub(eq + 1, s.n - eq - 1).trim();
    return Config::valid_key(*key);
}

void line_error(TextBuf* err, u32 line, const char* what) {
    err->append("replay: line ");
    err->append_u64(line);
    err->append(": ");
    err->append(what);
}

} // namespace

// --- writer ------------------------------------------------------------------

bool ReplayWriter::open(const char* path, const Config& initial) {
    if (!file_.open(path))
        return false;
    TextBuf out;
    out.append("nova-replay ");
    out.append_u64(kReplayVersion);
    out.append_char('\n');
    for (usize i = 0; i < initial.count(); ++i) {
        out.append("config ");
        out.append(initial.key(i));
        out.append(" = ");
        out.append(initial.value(i));
        out.append_char('\n');
    }
    file_.write(out.view());
    file_.flush();
    return true;
}

void ReplayWriter::knob(u64 tick, Str key, Str value) {
    TextBuf out;
    out.append("knob ");
    out.append_u64(tick);
    out.append_char(' ');
    out.append(key);
    out.append(" = ");
    out.append(value);
    out.append_char('\n');
    file_.write(out.view());
    file_.flush();
}

void ReplayWriter::event(u64 tick, Str text) {
    TextBuf out;
    out.append("event ");
    out.append_u64(tick);
    out.append_char(' ');
    out.append(text);
    out.append_char('\n');
    file_.write(out.view());
    file_.flush();
}

void ReplayWriter::tick_hash(const char* kind, u64 tick, u64 hash) {
    TextBuf out;
    out.append(kind);
    out.append_char(' ');
    out.append_u64(tick);
    out.append_char(' ');
    out.append_hex64(hash);
    out.append_char('\n');
    file_.write(out.view());
    file_.flush();
}

void ReplayWriter::checkpoint(u64 tick, u64 hash) {
    tick_hash("hash", tick, hash);
}
void ReplayWriter::end(u64 tick, u64 hash) {
    tick_hash("end", tick, hash);
}

// --- reader ------------------------------------------------------------------

Str ReplayLog::knob_key(usize i) const {
    return Str(arena_.data() + knobs_[i].key_off, knobs_[i].key_len);
}

Str ReplayLog::knob_value(usize i) const {
    return Str(arena_.data() + knobs_[i].val_off, knobs_[i].val_len);
}

Str ReplayLog::event_text(usize i) const {
    return Str(arena_.data() + events_[i].off, events_[i].len);
}

// A log before v3 was written before the varied world's features came on by
// default, and names only the settings it was given: the rest take the
// values they had then (legacy.h), as a snapshot before v4 does.
bool ReplayLog::replay_config(Config* out, Config::ParseError* err) const {
    if (!out->parse(config_.view(), err))
        return false;
    if (version_ < kReplayLegacyBefore)
        apply_legacy_defaults(*out);
    return true;
}

bool ReplayLog::parse(Str text, TextBuf* err) {
    config_.clear();
    arena_.clear();
    knobs_.clear();
    events_.clear();
    version_ = 0;
    checkpoints_.clear();
    has_end_ = false;
    end_ = {};

    bool header = false;
    u32 line = 0;
    usize i = 0;
    while (i < text.n) {
        usize stop = i;
        while (stop < text.n && text.p[stop] != '\n')
            ++stop;
        ++line;
        Str s = text.sub(i, stop - i).trim();
        i = stop + 1;
        if (s.empty() || s.p[0] == '#')
            continue;

        Str kind, rest;
        split_word(s, &kind, &rest);

        if (!header) {
            u64 version = 0;
            if (!kind.eq("nova-replay") || !parse_u64(rest, &version)) {
                line_error(err, line, "not a Nova replay log (expected 'nova-replay <version>')");
                return false;
            }
            if (version < 1 || version > kReplayVersion) {
                line_error(err, line, "unsupported replay version ");
                err->append_u64(version);
                return false;
            }
            version_ = static_cast<u32>(version);
            header = true;
            continue;
        }
        if (has_end_) {
            line_error(err, line, "record after 'end'");
            return false;
        }

        if (kind.eq("config")) {
            Str k, v;
            if (!split_key_value(rest, &k, &v)) {
                line_error(err, line, "malformed config record");
                return false;
            }
            config_.append(k);
            config_.append(" = ");
            config_.append(v);
            config_.append_char('\n');
        } else if (kind.eq("knob")) {
            Str tick_s, kv, k, v;
            u64 tick = 0;
            split_word(rest, &tick_s, &kv);
            if (!parse_u64(tick_s, &tick) || !split_key_value(kv, &k, &v)) {
                line_error(err, line, "malformed knob record");
                return false;
            }
            if (!knobs_.empty() && tick < knobs_.back().tick) {
                line_error(err, line, "knob records out of tick order");
                return false;
            }
            Knob kn;
            kn.tick = tick;
            kn.key_off = static_cast<u32>(arena_.size());
            kn.key_len = static_cast<u32>(k.n);
            arena_.append(k.p, k.n);
            kn.val_off = static_cast<u32>(arena_.size());
            kn.val_len = static_cast<u32>(v.n);
            arena_.append(v.p, v.n);
            knobs_.push(kn);
        } else if (kind.eq("event") && version_ >= 2) {
            Str tick_s, body;
            u64 tick = 0;
            split_word(rest, &tick_s, &body);
            if (!parse_u64(tick_s, &tick) || body.empty()) {
                line_error(err, line, "malformed event record");
                return false;
            }
            if (!events_.empty() && tick < events_.back().tick) {
                line_error(err, line, "event records out of tick order");
                return false;
            }
            Event ev;
            ev.tick = tick;
            ev.off = static_cast<u32>(arena_.size());
            ev.len = static_cast<u32>(body.n);
            arena_.append(body.p, body.n);
            events_.push(ev);
        } else if (kind.eq("hash") || kind.eq("end")) {
            Str tick_s, hex;
            ReplayCheckpoint cp;
            split_word(rest, &tick_s, &hex);
            if (!parse_u64(tick_s, &cp.tick) || !parse_hex64(hex, &cp.hash)) {
                line_error(err, line, "malformed hash record");
                return false;
            }
            if (kind.eq("end")) {
                if (!checkpoints_.empty() && cp.tick < checkpoints_.back().tick) {
                    line_error(err, line, "end tick before the last checkpoint");
                    return false;
                }
                has_end_ = true;
                end_ = cp;
            } else {
                if (!checkpoints_.empty() && cp.tick <= checkpoints_.back().tick) {
                    line_error(err, line, "checkpoints out of tick order");
                    return false;
                }
                checkpoints_.push(cp);
            }
        } else {
            line_error(err, line, "unknown record kind");
            return false;
        }
    }
    if (!header) {
        err->append("replay: empty log");
        return false;
    }
    return true;
}

bool ReplayLog::load_file(const char* path, TextBuf* err) {
    Array<u8> bytes;
    if (!read_file(path, &bytes)) {
        err->append("replay: cannot read '");
        err->append(path);
        err->append_char('\'');
        return false;
    }
    return parse(Str(reinterpret_cast<const char*>(bytes.data()), bytes.size()), err);
}

} // namespace nova
