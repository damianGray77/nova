// A run's files as streams (docs/analysis-plan.md A0): lines read with shared
// access, CSV headers by name, genebank entries, op symbols, the run's config
// and the lineages that met the gate.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>  // snprintf
#include <cstring> // memchr, memmove

#include "nova/analysis.h"
#include "nova/hash.h"
#include "nova/io.h"

namespace nova {

// --- LineReader -------------------------------------------------------------------

bool LineReader::open(const char* path) {
    close();
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    h_ = h;
    LARGE_INTEGER sz;
    size_ = GetFileSizeEx(h, &sz) ? static_cast<u64>(sz.QuadPart) : 0;
    if (buf_.size() < (1u << 20))
        buf_.resize(1u << 20);
    pos_ = end_ = 0;
    eof_ = false;
    consumed_ = 0;
    return true;
}

void LineReader::close() {
    if (h_) {
        CloseHandle(static_cast<HANDLE>(h_));
        h_ = nullptr;
    }
}

bool LineReader::next(Str* line) {
    if (!h_)
        return false;
    for (;;) {
        if (pos_ < end_) {
            const char* p = buf_.data() + pos_;
            const char* nl = static_cast<const char*>(memchr(p, '\n', end_ - pos_));
            if (nl) {
                usize n = static_cast<usize>(nl - p);
                consumed_ += n + 1;
                pos_ += n + 1;
                if (n && p[n - 1] == '\r')
                    --n;
                *line = Str(p, n);
                return true;
            }
        }
        if (eof_)
            return false; // a partial last line stays unread
        const usize rem = end_ - pos_;
        if (rem && pos_)
            memmove(buf_.data(), buf_.data() + pos_, rem);
        pos_ = 0;
        end_ = rem;
        if (end_ == buf_.size())
            buf_.resize(buf_.size() * 2);
        DWORD got = 0;
        const usize want = buf_.size() - end_;
        const DWORD ask = want > 0x40000000u ? 0x40000000u : static_cast<DWORD>(want);
        if (!ReadFile(static_cast<HANDLE>(h_), buf_.data() + end_, ask, &got, nullptr) || got == 0)
            eof_ = true;
        end_ += got;
    }
}

// --- CSV ----------------------------------------------------------------------------

u32 csv_split(Str line, Str* fields, u32 max) {
    u32 n = 0;
    usize start = 0;
    for (usize i = 0; i <= line.n && n < max; ++i) {
        if (i == line.n || line.p[i] == ',') {
            fields[n++] = Str(line.p + start, i - start);
            start = i + 1;
        }
    }
    return n;
}

bool CsvHeader::parse(Str line) {
    text_.assign(line.p, line.n);
    off_.clear();
    len_.clear();
    usize start = 0;
    for (usize i = 0; i <= line.n; ++i) {
        if (i == line.n || line.p[i] == ',') {
            Str f = Str(text_.data() + start, i - start).trim();
            off_.push(static_cast<u32>(f.p - text_.data()));
            len_.push(static_cast<u32>(f.n));
            start = i + 1;
        }
    }
    return line.n > 0;
}

i32 CsvHeader::find(Str name) const {
    for (usize i = 0; i < off_.size(); ++i)
        if (Str(text_.data() + off_[i], len_[i]).eq(name))
            return static_cast<i32>(i);
    return -1;
}

// --- ids and hex --------------------------------------------------------------------

bool parse_lineage_id(Str s, u64* id) {
    const usize dot = s.find('.');
    u64 hi = 0, lo = 0;
    if (dot >= s.n || !parse_u64(s.sub(0, dot), &hi) || !parse_u64(s.sub(dot + 1, s.n - dot - 1), &lo) ||
        hi > 0xFFFFFFFFull || lo > 0xFFFFFFFFull)
        return false;
    *id = (hi << 32) | lo;
    return true;
}

void append_lineage_id(TextBuf& out, u64 id) {
    out.append_u64(id >> 32);
    out.append_char('.');
    out.append_u64(id & 0xFFFFFFFFull);
}

bool parse_hex(Str s, u64* out) {
    if (s.n == 0 || s.n > 16)
        return false;
    u64 v = 0;
    for (usize i = 0; i < s.n; ++i) {
        const char c = s.p[i];
        u64 d;
        if (c >= '0' && c <= '9')
            d = static_cast<u64>(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = static_cast<u64>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = static_cast<u64>(c - 'A' + 10);
        else
            return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

namespace {

// The value of `key=` in a space-separated record line ("" if absent).
Str field(Str line, const char* key) {
    const Str k(key);
    for (usize i = 0; i + k.n < line.n; ++i) {
        if ((i == 0 || line.p[i - 1] == ' ') && line.p[i + k.n] == '=' && Str(line.p + i, k.n).eq(k)) {
            usize s = i + k.n + 1, e = s;
            while (e < line.n && line.p[e] != ' ')
                ++e;
            return Str(line.p + s, e - s);
        }
    }
    return Str();
}

} // namespace

// --- genebank.txt -------------------------------------------------------------------

bool GenebankReader::open(const char* path) {
    entries_ = 0;
    err_[0] = '\0';
    return r_.open(path);
}

bool GenebankReader::next(BankEntry* e) {
    Str line;
    for (;;) {
        if (!r_.next(&line))
            return false;
        if (line.n == 0 || line.p[0] == '#')
            continue;
        if (line.p[0] == ' ') {
            snprintf(err_, sizeof err_, "genebank: a genome line with no entry before it (entry %llu)",
                     static_cast<unsigned long long>(entries_ + 1));
            return false;
        }
        break;
    }
    head_.assign(line.p, line.n);
    const Str head(head_.data(), head_.size());
    Str genome;
    if (!r_.next(&genome))
        return false; // the entry's genome isn't written yet
    *e = BankEntry{};
    const usize sp = head.find(' ');
    e->name = head.sub(0, sp);
    e->parent = field(head, "parent");
    e->genome = genome.trim();
    u64 node = 0, ph = 0;
    const Str phs = field(head, "parent_hash");
    const bool ok = sp < head.n && parse_u64(field(head, "tick"), &e->tick) && parse_u64(field(head, "node"), &node) &&
                    parse_lineage_id(field(head, "lineage"), &e->lineage) && parse_hex(field(head, "hash"), &e->hash) &&
                    (phs.eq("-") || phs.n == 0 || parse_hex(phs, &ph)) && node <= 0xFFFFFFFFull;
    if (!ok) {
        snprintf(err_, sizeof err_, "genebank: entry %llu is malformed: %.*s",
                 static_cast<unsigned long long>(entries_ + 1), static_cast<int>(head.n > 80 ? 80 : head.n), head.p);
        return false;
    }
    e->node = static_cast<u32>(node);
    e->parent_hash = e->parent.eq("none") ? 0 : ph;
    ++entries_;
    return true;
}

// --- op symbols ----------------------------------------------------------------------

void OpAlphabet::init(const CodonTable& t) {
    by_name_.clear();
    size_ = 0;
    u32 sym_of[kOpCount];
    for (u32 i = 0; i < kOpCount; ++i)
        sym_of[i] = 0xFFFFFFFFu;
    for (u32 c = 0; c < 64; ++c) {
        const u32 op = static_cast<u32>(t.op[c]);
        if (op < kOpCount && sym_of[op] == 0xFFFFFFFFu)
            sym_of[op] = size_++;
    }
    for (u32 op = 0; op < kOpCount; ++op) {
        if (sym_of[op] == 0xFFFFFFFFu)
            continue;
        const char* nm = op_name(static_cast<Op>(op));
        by_name_.get_or_insert(hash_bytes(nm, strlen(nm), 0x6F702D6E616D6573ull)) = sym_of[op];
    }
    // Tierra's nop0 / nop1 spell NOP0 / NOP1 too (isa.h).
    const char* const alias[2] = {"nop0", "nop1"};
    const Op aop[2] = {Op::nop0, Op::nop1};
    for (int i = 0; i < 2; ++i) {
        const u32 op = static_cast<u32>(aop[i]);
        if (sym_of[op] != 0xFFFFFFFFu)
            by_name_.get_or_insert(hash_bytes(alias[i], strlen(alias[i]), 0x6F702D6E616D6573ull)) = sym_of[op];
    }
}

bool OpAlphabet::encode(Str names, Array<u8>* out) const {
    out->clear();
    usize i = 0;
    while (i < names.n) {
        while (i < names.n && names.p[i] == ' ')
            ++i;
        usize j = i;
        while (j < names.n && names.p[j] != ' ')
            ++j;
        if (j > i) {
            const u32* s = by_name_.find(hash_bytes(names.p + i, j - i, 0x6F702D6E616D6573ull));
            if (!s)
                return false;
            out->push(static_cast<u8>(*s));
        }
        i = j;
    }
    return true;
}

// --- the run's config -------------------------------------------------------------------

bool load_run_config(const char* dir, Config* cfg, TextBuf* err) {
    TextBuf path;
    path_join(path, dir, "run.cfg");
    Config::ParseError pe;
    if (path_exists(path.c_str())) {
        if (!cfg->load_file(path.c_str(), &pe)) {
            err->append(path.view());
            err->append(": ");
            err->append(pe.msg);
            return false;
        }
        return true;
    }
    path.clear();
    path_join(path, dir, "replay.log");
    LineReader r;
    if (!r.open(path.c_str())) {
        err->append("no run.cfg or replay.log in '");
        err->append(dir);
        err->append("', so the run's config is unknown");
        return false;
    }
    TextBuf text;
    Str line;
    while (r.next(&line)) {
        if (line.starts_with("nova-replay") || line.starts_with("#"))
            continue;
        if (!line.starts_with("config "))
            break;
        text.append(line.sub(7, line.n - 7));
        text.append_char('\n');
    }
    if (!cfg->parse(text.view(), &pe)) {
        err->append(path.view());
        err->append(": ");
        err->append(pe.msg);
        return false;
    }
    return true;
}

bool run_codon_table(Config& cfg, CodonTable* t, TextBuf* err) {
    const Str name = key_choice(cfg, "isa.table");
    if (name.eq("custom")) {
        if (!parse_table(key_str(cfg, "isa.codons"), t, err))
            return false;
    } else if (!builtin_table(name, t)) {
        err->append("unknown codon table '");
        err->append(name);
        err->append_char('\'');
        return false;
    }
    if (!cfg.ok()) {
        err->append(cfg.value_error());
        return false;
    }
    return true;
}

// --- the gated lineages ------------------------------------------------------------------

bool load_gated(const char* dir, GatedSet* out, TextBuf* err) {
    out->ids.clear();
    out->first_tick.clear();
    out->csv_rows = out->from_csv = out->from_replay = 0;
    out->csv = out->replay = false;
    TextBuf path;
    path_join(path, dir, "lineages.csv");
    LineReader r;
    Str line;
    if (r.open(path.c_str())) {
        out->csv = true;
        CsvHeader h;
        if (!r.next(&line) || !h.parse(line)) {
            err->append("lineages.csv has no header");
            return false;
        }
        const i32 cl = h.find("lineage"), cd = h.find("detected");
        if (cl < 0 || cd < 0) {
            err->append("lineages.csv lacks a lineage or detected column");
            return false;
        }
        const u32 need = static_cast<u32>((cl > cd ? cl : cd) + 1);
        Str f[64];
        while (r.next(&line)) {
            ++out->csv_rows;
            if (csv_split(line, f, need) < need || !f[cd].eq("1"))
                continue;
            u64 id;
            if (!parse_lineage_id(f[cl], &id))
                continue;
            bool added = false;
            out->ids.get_or_insert(id, &added);
            out->from_csv += added;
        }
        r.close();
    }
    path.clear();
    path_join(path, dir, "replay.log");
    if (r.open(path.c_str())) {
        out->replay = true;
        while (r.next(&line)) {
            // event <tick> replicator node=... lineage=...
            if (!line.starts_with("event "))
                continue;
            usize sp = 6;
            while (sp < line.n && line.p[sp] != ' ')
                ++sp;
            if (!line.sub(sp, line.n - sp).starts_with(" replicator "))
                continue;
            u64 id;
            if (!parse_lineage_id(field(line, "lineage"), &id))
                continue;
            bool added = false;
            out->ids.get_or_insert(id, &added);
            out->from_replay += added;
            // When it first met the gate (NoveltyOptions::as_live: the live meter's rule).
            u64 tick = 0;
            if (parse_u64(line.sub(6, sp - 6), &tick)) {
                bool fresh = false;
                u64& first = out->first_tick.get_or_insert(id, &fresh);
                if (fresh || tick + 1 < first)
                    first = tick + 1;
            }
        }
    }
    return true;
}

// --- paths ----------------------------------------------------------------------------------

void path_join(TextBuf& out, Str dir, Str leaf) {
    out.append(dir);
    if (dir.n && dir.p[dir.n - 1] != '/' && dir.p[dir.n - 1] != '\\')
        out.append_char('\\');
    out.append(leaf);
}

bool path_exists(const char* path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool make_dirs(const char* path) {
    char buf[MAX_PATH];
    const usize n = strlen(path);
    if (n == 0 || n >= sizeof buf)
        return false;
    memcpy(buf, path, n + 1);
    for (usize i = 1; i < n; ++i) {
        if (buf[i] != '/' && buf[i] != '\\')
            continue;
        if (buf[i - 1] == ':')
            continue; // "E:\"
        const char c = buf[i];
        buf[i] = '\0';
        CreateDirectoryA(buf, nullptr); // fails harmlessly if it exists
        buf[i] = c;
    }
    CreateDirectoryA(buf, nullptr);
    const DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

namespace {

void full_path(const char* p, char* out, usize cap) {
    const DWORD n = GetFullPathNameA(p, static_cast<DWORD>(cap), out, nullptr);
    if (n == 0 || n >= cap) {
        out[0] = '\0';
        return;
    }
    for (DWORD i = 0; i < n; ++i) {
        char c = out[i];
        if (c == '/')
            c = '\\';
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        out[i] = c;
    }
    usize len = n;
    while (len > 3 && out[len - 1] == '\\')
        out[--len] = '\0';
}

} // namespace

bool path_inside(const char* inner, const char* outer) {
    char a[MAX_PATH * 2], b[MAX_PATH * 2];
    full_path(inner, a, sizeof a);
    full_path(outer, b, sizeof b);
    const usize na = strlen(a), nb = strlen(b);
    if (nb == 0 || na < nb || memcmp(a, b, nb) != 0)
        return false;
    return na == nb || a[nb] == '\\' || b[nb - 1] == '\\';
}

} // namespace nova
