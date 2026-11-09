#include "viewer.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "nova/profile.h" // the roles (A3), for the map of life's colour by role
#include "theme.h"        // the dark theme and the virtual keys

namespace nova {

namespace {

// A5: the map of life's colours by role (kRole... order); unprofiled dim.
u32 role_color(u32 r) {
    static const u32 k[kRoleCount] = {
        0xE06060, // predator
        0xB07CE8, // parasite
        0x5EA1FF, // repairer
        0xE5C050, // signaller
        0x4FD0B0, // wanderer
        0x60C080, // copier
        0x8A91A0, // idle
    };
    return r < kRoleCount ? k[r] : 0x3C414C;
}

// The speed limits the - and + buttons step through (0 = flat out, last).
const double kSpeeds[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 0};
const u32 kSpeedCount = sizeof kSpeeds / sizeof kSpeeds[0];

void commas(u64 v, char* out) {
    char tmp[32];
    const int n = snprintf(tmp, sizeof tmp, "%llu", static_cast<unsigned long long>(v));
    int o = 0;
    for (int i = 0; i < n; ++i) {
        if (i && (n - i) % 3 == 0)
            out[o++] = ',';
        out[o++] = tmp[i];
    }
    out[o] = '\0';
}

// A number for a label: whole numbers with thousands commas up to a billion,
// otherwise three significant digits.
void nice_num(double v, char* out) {
    const double a = v < 0 ? -v : v;
    if (a >= 1000 && a < 1e12 && v >= 0) {
        commas(static_cast<u64>(v + 0.5), out);
        return;
    }
    snprintf(out, 32, "%.3g", v);
}

void lineage_text(u64 id, char* out, usize n) {
    if (!id)
        snprintf(out, n, "-");
    else
        snprintf(out, n, "%u.%u", static_cast<unsigned>(id >> 32), static_cast<unsigned>(id & 0xFFFFFFFFu));
}

template <class T> void swap_arr(Array<T>& a, Array<T>& b) {
    Array<T> t = move(a);
    a = move(b);
    b = move(t);
}

// Stable bottom-up merge sort of indices by a comparator (less-than).
template <class Less> void merge_sort(Array<u32>& a, Less less) {
    const usize n = a.size();
    Array<u32> tmp;
    tmp.resize(n);
    u32* src = a.data();
    u32* dst = tmp.data();
    for (usize width = 1; width < n; width *= 2) {
        for (usize lo = 0; lo < n; lo += 2 * width) {
            const usize mid = lo + width < n ? lo + width : n;
            const usize hi = lo + 2 * width < n ? lo + 2 * width : n;
            usize i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                dst[k++] = less(src[j], src[i]) ? src[j++] : src[i++];
            while (i < mid)
                dst[k++] = src[i++];
            while (j < hi)
                dst[k++] = src[j++];
        }
        u32* t = src;
        src = dst;
        dst = t;
    }
    if (src != a.data())
        memcpy(a.data(), src, n * sizeof(u32));
}

// Word wrap of monospaced text `s` into lines no wider than w pixels; calls
// emit(start, len) for each line.
template <class Emit> void wrap_mono(Canvas& c, const char* s, usize n, i32 w, Emit emit) {
    const i32 cw = c.text_w("0", 1, kFontMono);
    usize per = cw > 0 ? static_cast<usize>(w / cw) : 80;
    per = per < 8 ? 8 : per;
    usize at = 0;
    while (at < n) {
        usize end = at + per < n ? at + per : n;
        if (end < n) {
            usize sp = end;
            while (sp > at && s[sp] != ' ')
                --sp;
            if (sp > at)
                end = sp;
        }
        emit(at, end - at);
        at = end;
        while (at < n && s[at] == ' ')
            ++at;
    }
}

} // namespace

Viewer::Viewer() {
    for (u32 k = 0; k < kCharts; ++k)
        for (u32 s = 0; s < kSeries; ++s)
            series_[k][s] = kNone;
    series_[0][0] = stats_column_index("bodies");
    series_[1][0] = stats_column_index("births");
    series_[1][1] = stats_column_index("deaths");
    series_[2][0] = stats_column_index("lineages");
    series_[2][1] = stats_column_index("colonised");
    series_[2][2] = stats_column_index("live_nodes");
    series_[3][0] = stats_column_index("compress");
}

void Viewer::take(ViewFrame& f) {
    swap_arr(cur_.node, f.node);
    swap_arr(cur_.edge, f.edge);
    swap_arr(cur_.cargo, f.cargo);
    swap_arr(cur_.strip, f.strip);
    swap_arr(cur_.strip_owner, f.strip_owner);
    swap_arr(cur_.bodies, f.bodies);
    swap_arr(cur_.genome, f.genome);
    swap_arr(cur_.cpus, f.cpus);
    swap_arr(cur_.lanes, f.lanes);
    cur_.tick = f.tick;
    cur_.instructions = f.instructions;
    cur_.hash = f.hash;
    cur_.hashed = f.hashed;
    cur_.procedural = f.procedural;
    cur_.archive = f.archive;
    cur_.nodes = f.nodes;
    cur_.detect_tick = f.detect_tick;
    if (f.land_sent) { // a landscape's land comes once
        landscape_ = f.landscape;
        land_w_ = f.land_w;
        land_h_ = f.land_h;
        swap_arr(land_img_, f.land);
        ground_ok_ = false; // (its substrate colours are made again)
        ground_sum_ = 0;
        swap_arr(masses_, f.masses);
        memcpy(box_, f.box, sizeof box_);
    }
    cur_.knobs = f.knobs;
    memcpy(cur_.knob_key, f.knob_key, sizeof cur_.knob_key);
    memcpy(cur_.knob_value, f.knob_value, sizeof cur_.knob_value);
    cur_.sel_node = f.sel_node;
    cur_.strip_w = f.strip_w;
    cur_.strip_h = f.strip_h;
    cur_.fast_end = f.fast_end; // (W3)
    cur_.sel_body = f.sel_body;
    cur_.req_body = f.req_body;
    cur_.req_serial = f.req_serial;
    cur_.body_ok = f.body_ok;
    cur_.body = f.body;

    // New things join the logs.
    for (usize i = 0; i < f.sample_tick.size(); ++i)
        charts_.add(f.sample_tick[i], f.sample.data() + i * kStatsColumns);
    if (flash_tick_.size() != cur_.nodes) {
        flash_tick_.resize(cur_.nodes);
        flash_kind_.resize(cur_.nodes);
    }
    for (usize i = 0; i < f.events.size(); ++i) {
        ViewEvent e = f.events[i];
        const u32 off = static_cast<u32>(event_text_.size());
        event_text_.append(f.event_text.data() + e.off, e.len);
        e.off = off;
        events_.push(e);
        if (e.kind < kEvCount && !first_event_[e.kind])
            first_event_[e.kind] = e.tick + 1;
        if ((e.kind == kEvCollapse || e.kind == kEvRecovered) && e.node < flash_tick_.size()) {
            flash_tick_[e.node] = e.tick + 1;
            flash_kind_[e.node] = static_cast<u8>(e.kind);
        }
        if (event_is_strike(e.kind) && e.node != kNone) { // X7: a strike's map marker
            if (strikes_.size() >= kMaxStrikes) {
                const usize drop = kMaxStrikes / 4;
                memmove(strikes_.data(), strikes_.data() + drop, (strikes_.size() - drop) * sizeof(Strike));
                strikes_.resize(strikes_.size() - drop);
            }
            strikes_.push(Strike{e.tick, e.node, e.kind});
        }
    }
    for (usize i = 0; i < f.saves.size(); ++i) {
        GenomeEntry e = f.saves[i];
        const u32 off = static_cast<u32>(save_text_.size());
        save_text_.append(f.save_text.data() + e.off, e.n);
        e.off = off;
        saves_.push(e);
        order_dirty_ = true;
        if (life_built_ && !life_archive_) // A5: the live map keeps up with the saves
            life_.add(e.tick, e.genotype, e.lineage, e.node, e.len, e.name, Str(save_text_.data() + off, e.n));
    }
    // A5: profiled genotypes' roles (the latest sample's wins), for the colour by role.
    for (usize i = 0; i < f.role_genotype.size() && i < f.role_of.size(); ++i)
        roles_.get_or_insert(f.role_genotype[i]) = static_cast<u8>(f.role_of[i] + 1);
    saves_seen_ += f.saves.size();
    // A2: "new kinds of life": closed windows join the list, the open one's
    // leaders replace the last frame's.
    novelty_ = f.novelty;
    novelty_window_ = f.novelty_window;
    for (usize i = 0; i < f.picks.size(); ++i) {
        NoveltyPick p = f.picks[i];
        const u32 off = static_cast<u32>(pick_text_.size());
        pick_text_.append(f.pick_text.data() + p.off, p.n);
        p.off = off;
        picks_.push(p);
    }
    if (f.picks.size()) { // the list's order: windows newest first, each in rank order
        pick_order_.clear();
        for (usize end = picks_.size(); end > 0;) {
            usize start = end - 1;
            while (start > 0 && picks_[start - 1].window_first == picks_[end - 1].window_first)
                --start;
            for (usize k = start; k < end; ++k)
                pick_order_.push(static_cast<u32>(k));
            end = start;
        }
    }
    if (!cur_.archive) {
        picks_open_.clear();
        pick_open_text_.clear();
        for (usize i = 0; i < f.picks_open.size(); ++i) {
            NoveltyPick p = f.picks_open[i];
            const u32 off = static_cast<u32>(pick_open_text_.size());
            pick_open_text_.append(f.pick_text.data() + p.off, p.n);
            p.off = off;
            picks_open_.push(p);
        }
    }
    // A5: living members per genotype; a finished run's whole map.
    if (f.pop_sent) {
        swap_arr(pop_genotype_, f.pop_genotype);
        swap_arr(pop_count_, f.pop_count);
        pop_known_ = true;
    }
    if (f.life_sent && f.life.points().size()) {
        life_ = move(f.life);
        life_archive_ = life_built_ = true;
    }
    f.clear_new();
    trim_logs();

    // A selection the world no longer has.
    if (sel_node_ != kNone && sel_node_ >= cur_.nodes) {
        sel_node_ = kNone;
        sel_body_ = kNone;
    }
    // (Only a frame built for this very body can say it died: one requested
    // before the click doesn't know about it yet.)
    if (sel_body_ != kNone && f.req_body == sel_body_ && f.req_serial == sel_serial_ && !cur_.body_ok &&
        !cur_.archive)
        sel_body_ = kNone; // it died
    have_ = true;
}

// The logs keep the newest kMaxSaves genebank saves and events: past that the
// oldest quarter goes (a long run saves millions of genotypes).
void Viewer::trim_logs() {
    if (saves_.size() > kMaxSaves) {
        const usize drop = saves_.size() - kMaxSaves + kMaxSaves / 4;
        const u32 base = saves_[drop].off;
        for (usize i = drop; i < saves_.size(); ++i) {
            GenomeEntry e = saves_[i];
            e.off -= base;
            saves_[i - drop] = e;
        }
        saves_.resize(saves_.size() - drop);
        memmove(save_text_.data(), save_text_.data() + base, save_text_.size() - base);
        save_text_.resize(save_text_.size() - base);
        if (sel_save_ != ~static_cast<usize>(0))
            sel_save_ = sel_save_ >= drop ? sel_save_ - drop : ~static_cast<usize>(0);
        order_dirty_ = true;
        phylo_saves_ = ~static_cast<usize>(0); // lay out again
        if (life_built_ && !life_archive_ && !saves_.empty()) // A5: the live map follows the log
            life_.drop_before(saves_[0].tick);
    }
    if (events_.size() > kMaxSaves) {
        const usize drop = events_.size() - kMaxSaves + kMaxSaves / 4;
        const u32 base = events_[drop].off;
        for (usize i = drop; i < events_.size(); ++i) {
            ViewEvent e = events_[i];
            e.off -= base;
            events_[i - drop] = e;
        }
        events_.resize(events_.size() - drop);
        memmove(event_text_.data(), event_text_.data() + base, event_text_.size() - base);
        event_text_.resize(event_text_.size() - base);
    }
}

ViewRequest Viewer::request() const {
    ViewRequest q;
    q.node = sel_node_;
    q.body = sel_body_;
    q.serial = sel_serial_;
    q.strip = strip_by_;
    q.genotype = sel_save_ < saves_.size() ? saves_[sel_save_].genotype : 0;
    q.populations = life_mode_; // A5: the map sizes points by their living members
    q.material = color_by_ == kColorMaterial; // (a scan of every soup: only while the map shows it)
    return q;
}

// --- docs/results/viewer.md: strikes, new kinds of life, the map of life ------------

bool Viewer::strike_marker(u32 n, u32* kind, double* age) const {
    // The latest strike on the node at or before the frame's tick, within kStrikeFade.
    const u64 now = cur_.tick;
    for (usize i = strikes_.size(); i-- > 0;) {
        const Strike& s = strikes_[i];
        if (s.tick > now)
            continue;
        if (now - s.tick >= kStrikeFade)
            return false; // (oldest further back: none newer is left on any node from here)
        if (s.node == n) {
            *kind = s.kind;
            *age = static_cast<double>(now - s.tick) / static_cast<double>(kStrikeFade);
            return true;
        }
    }
    return false;
}

const NoveltyPick& Viewer::pick(usize i) const {
    if (i < picks_open_.size())
        return picks_open_[i];
    return picks_[pick_order_[i - picks_open_.size()]]; // closed windows, newest first, each in rank order
}

void Viewer::select_pick(usize i) {
    if (i >= pick_count())
        return;
    const bool open = i < picks_open_.size();
    const NoveltyPick p = pick(i);
    const char* text = (open ? pick_open_text_.data() : pick_text_.data()) + p.off;
    usize found = ~static_cast<usize>(0);
    for (usize k = 0; k < saves_.size() && found == ~static_cast<usize>(0); ++k) { // (its first save: the one scored)
        const GenomeEntry& e = saves_[k];
        if (p.genotype ? e.genotype == p.genotype
                       : (e.node == p.node && e.lineage == p.lineage && strcmp(e.name, p.name) == 0))
            found = k;
    }
    if (found == ~static_cast<usize>(0)) {
        // Older than the browser keeps (or a finished run's newest saves lack
        // it): it joins the browser, from the list's own copy of its genome.
        GenomeEntry e;
        memset(&e, 0, sizeof e);
        e.tick = p.tick;
        e.genotype = p.genotype;
        e.lineage = p.lineage;
        e.node = p.node;
        e.len = p.len;
        memcpy(e.name, p.name, sizeof e.name);
        snprintf(e.parent_name, sizeof e.parent_name, "?");
        e.off = static_cast<u32>(save_text_.size());
        save_text_.append(text, p.n);
        e.n = p.n;
        e.surprise = static_cast<float>(p.surprise);
        saves_.push(e);
        found = saves_.size() - 1;
        order_dirty_ = true;
    }
    select_save(found);
    sel_lineage_ = p.lineage;
    auto_ = false;
}

void Viewer::set_life_mode(bool on) {
    life_mode_ = on;
    if (on && !life_built_)
        life_build();
}

u32 Viewer::genotype_role(u64 genotype) const {
    const u8* r = roles_.find(genotype);
    return r && *r ? *r - 1u : kRoleCount;
}

void Viewer::set_life_time(u64 tick, u64 span) {
    life_follow_ = false;
    life_t_ = tick;
    life_span_ = span;
}

bool Viewer::life_point_screen(u32 i, i32* x, i32* y) const {
    if (i >= life_sx_.size() || life_sx_[i] == 0x7FFFFFFF)
        return false;
    *x = life_sx_[i];
    *y = life_sy_[i];
    return true;
}

// The live map from the genebank log (its first kLifeRef genotypes the
// reference window), and from then on every save as it comes.
void Viewer::life_build() {
    life_.clear();
    for (usize i = 0; i < saves_.size(); ++i) {
        const GenomeEntry& e = saves_[i];
        life_.add(e.tick, e.genotype, e.lineage, e.node, e.len, e.name, Str(save_text_.data() + e.off, e.n));
    }
    life_built_ = true;
    life_archive_ = false;
}

// Fitted afresh on the log's newest kLifeRef genotypes (a user's choice:
// positions then mean the new axes).
void Viewer::life_refit() {
    Array<char> text;
    Array<u32> off, len;
    HashMap<u8> seen;
    u64 first = ~0ull, last = 0;
    for (usize i = saves_.size(); i-- > 0 && off.size() < kLifeRef;) {
        const GenomeEntry& e = saves_[i];
        bool fresh = false;
        seen.get_or_insert(e.genotype, &fresh);
        if (!fresh)
            continue;
        off.push(static_cast<u32>(text.size()));
        len.push(e.n);
        text.append(save_text_.data() + e.off, e.n);
        first = e.tick < first ? e.tick : first;
        last = e.tick > last ? e.tick : last;
    }
    life_.clear();
    if (off.size() >= 2)
        life_.fit(text, off, len, first, last);
    for (usize i = 0; i < saves_.size(); ++i) {
        const GenomeEntry& e = saves_[i];
        life_.add(e.tick, e.genotype, e.lineage, e.node, e.len, e.name, Str(save_text_.data() + e.off, e.n));
    }
    life_built_ = true;
    life_archive_ = false;
}

void Viewer::select_node(u32 n) {
    if (n != sel_node_) {
        sel_body_ = kNone;
        body_scroll_ = 0;
        genome_scroll_ = 0;
    }
    sel_node_ = n;
    if (n < cur_.node.size() && cur_.node[n].top_lineage)
        sel_lineage_ = cur_.node[n].top_lineage;
}

void Viewer::select_save(usize i) {
    if (i >= saves_.size())
        return;
    sel_save_ = i;
    sel_lineage_ = saves_[i].lineage;
    order_dirty_ = order_dirty_ || lineage_only_;
}

// The lineage with the most saved genotypes (the latest on a tie): the
// richest phylogeny to show first. 0 if nothing is saved.
u64 Viewer::richest_lineage() const {
    Array<u64> ids;
    for (usize i = 0; i < saves_.size(); ++i)
        if (saves_[i].lineage)
            ids.push(saves_[i].lineage);
    sort_u64(ids.data(), ids.size());
    u64 best = 0;
    usize best_n = 0;
    for (usize i = 0; i < ids.size();) {
        usize j = i;
        while (j < ids.size() && ids[j] == ids[i])
            ++j;
        if (j - i >= best_n) {
            best_n = j - i;
            best = ids[i];
        }
        i = j;
    }
    return best_n > 1 ? best : 0;
}

void Viewer::auto_select() {
    if (!have_ || !auto_)
        return;
    if (sel_node_ == kNone) {
        u32 best = kNone, most = 0;
        for (u32 n = 0; n < cur_.node.size(); ++n) {
            if (cur_.node[n].live && cur_.node[n].bodies > most) {
                most = cur_.node[n].bodies;
                best = n;
            }
        }
        if (best == kNone)
            return;
        select_node(best);
        const u64 rich = richest_lineage();
        sel_lineage_ = rich ? rich : cur_.node[best].top_lineage;
        return; // the next frame carries its bodies
    }
    if (cur_.sel_node != sel_node_)
        return;
    if (sel_body_ == kNone && !cur_.bodies.empty()) {
        // A member of the chosen lineage here, else of the node's largest,
        // else the first body.
        usize pick = 0;
        bool found = false;
        const u64 top = sel_node_ < cur_.node.size() ? cur_.node[sel_node_].top_lineage : 0;
        for (u32 pass = 0; pass < 2 && !found; ++pass) {
            const u64 want = pass == 0 ? sel_lineage_ : top;
            for (usize i = 0; i < cur_.bodies.size() && want && !found; ++i) {
                if (cur_.bodies[i].lineage == want) {
                    pick = i;
                    found = true;
                }
            }
        }
        sel_body_ = cur_.bodies[pick].slot;
        sel_serial_ = cur_.bodies[pick].serial;
        if (!sel_lineage_)
            sel_lineage_ = cur_.bodies[pick].lineage;
    }
    if (sel_save_ >= saves_.size() && !saves_.empty()) {
        usize pick = saves_.size() - 1;
        for (usize i = saves_.size(); i-- > 0;) {
            if (!sel_lineage_ || saves_[i].lineage == sel_lineage_) {
                pick = i;
                break;
            }
        }
        select_save(pick);
    }
    if (sel_body_ != kNone && sel_save_ < saves_.size())
        auto_ = false;
}

bool Viewer::node_screen(u32 n, i32* x, i32* y) const {
    if (n >= node_sx_.size())
        return false;
    *x = node_sx_[n];
    *y = node_sy_[n];
    return map_inner_.contains(*x, *y);
}

void Viewer::command(u32 kind, double value) {
    ViewCommand c = {};
    c.kind = kind;
    c.value = value;
    cmds_.push(c);
}

void Viewer::set_tooltip(const char* text) {
    snprintf(tip_, sizeof tip_, "%s", text);
    tip_x_ = mx_;
    tip_y_ = my_;
}

// --- layout and widgets ------------------------------------------------------

void Viewer::layout(Canvas& c, const RunStatus& st) {
    const i32 W = c.width(), H = c.height();
    const i32 gap = c.px(4);
    (void)st;
    const i32 barh = c.px(36.0f); // one row: a run's settings are fixed, so there are no knobs (config-plan C3)
    bar_ = Rect{0, 0, W, barh};
    auto clampf = [](float v) { return v < 0.2f ? 0.2f : (v > 0.85f ? 0.85f : v); };
    fx_ = clampf(fx_);
    fy_ = clampf(fy_);
    fb_ = clampf(fb_);
    fr_ = clampf(fr_);
    const i32 body_h = H - barh;
    const i32 left_w = static_cast<i32>(W * fx_);
    map_ = Rect{gap, barh + gap, left_w - gap, static_cast<i32>(body_h * fy_) - gap};
    const i32 by = map_.bottom() + gap, bh = H - gap - by;
    const i32 cw = static_cast<i32>((left_w - gap) * fb_);
    charts_r_ = Rect{gap, by, cw - gap / 2, bh};
    phylo_r_ = Rect{charts_r_.right() + gap, by, left_w - charts_r_.right() - gap, bh};
    insp_ = Rect{left_w + gap, barh + gap, W - left_w - 2 * gap, static_cast<i32>(body_h * fr_) - gap};
    browse_ = Rect{insp_.x, insp_.bottom() + gap, insp_.w, H - gap - (insp_.bottom() + gap)};
    if (solo_) { // the map alone (frames for an animation)
        map_ = Rect{gap, barh + gap, W - 2 * gap, body_h - 2 * gap};
        charts_r_ = phylo_r_ = insp_ = browse_ = Rect{};
    }
}

bool Viewer::button(const UiInput& in, Canvas& c, const Rect& r, const char* label, bool on) {
    const bool hot = r.contains(in.mx, in.my);
    c.fill(r, on ? kButtonOn : (hot ? kButtonHot : kButton));
    c.text(r.x + r.w / 2, r.y + (r.h - c.line_h(kFontUi)) / 2, label, kFontUi, kText, kAlignCenter, r.w - 4);
    if (hot)
        cursor_ = 3;
    return hot && in.pressed;
}

void Viewer::panel_box(Canvas& c, const Rect& r, const char* title, Rect* inner) {
    c.fill(r, kPanel);
    const i32 hh = c.line_h(kFontBold) + c.px(6);
    c.fill(Rect{r.x, r.y, r.w, hh}, kHead);
    c.text(r.x + c.px(8), r.y + c.px(3), title, kFontBold, kText);
    const i32 pad = c.px(6);
    *inner = Rect{r.x + pad, r.y + hh + pad, r.w - 2 * pad, r.h - hh - 2 * pad};
}

void Viewer::open_popup(u32 kind, u32 owner, const Rect& anchor) {
    popup_ = kind;
    popup_owner_ = owner;
    popup_scroll_ = 0;
    popup_bar_.release();
    popup_r_ = Rect{anchor.x, anchor.bottom(), 0, 0}; // sized when drawn
}

// --- the frame -----------------------------------------------------------------

void Viewer::frame(const UiInput& input, Canvas& c, const RunStatus& st) {
    UiInput in = input;
    mx_ = in.mx;
    my_ = in.my;
    tip_[0] = '\0';
    cursor_ = 0;
    layout(c, st);
    c.unclip();
    c.fill(Rect{0, 0, c.width(), c.height()}, kBg);

    // Typing into the genome browser's filter.
    if (edit_ != kNone) {
        for (u32 i = 0; i < in.nchars; ++i) {
            const char ch = in.chars[i];
            const bool ok = (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
            if (ok && edit_len_ + 1 < sizeof edit_buf_ && edit_len_ + 1 < sizeof filt_)
                edit_buf_[edit_len_++] = ch;
        }
        for (u32 i = 0; i < in.nkeys; ++i) {
            if (in.keys[i] == kVkBack && edit_len_ > 0)
                --edit_len_;
            else if (in.keys[i] == kVkReturn) {
                edit_buf_[edit_len_] = '\0';
                set_filter(edit_buf_);
                edit_ = kNone;
            } else if (in.keys[i] == kVkEscape) {
                edit_ = kNone;
            }
        }
    } else {
        // W8: T cycles the substrate type the map shows; B switches the
        // ground between substrate colours and the plain land.
        for (u32 i = 0; i < in.nkeys; ++i) {
            if (in.keys[i] == 0x54 && color_by_ == kColorSubstrate) // 'T'
                sub_type_ = (sub_type_ + 1) % kSubstrateTypes;
            if (in.keys[i] == 0x42) // 'B'
                ground_on_ = !ground_on_;
            if (in.keys[i] == 0x24) // Home: the charts show the whole run again
                charts_whole_run();
        }
    }

    bool consumed = false;
    popup(in, c, &consumed); // input only, when not drawing (see below)
    if (consumed) {
        in.pressed = in.released = in.dbl = false;
        in.wheel = 0;
    }
    if (in.pressed && edit_ != kNone)
        edit_ = kNone; // a click elsewhere cancels the edit (the field re-opens it)

    if (!solo_)
        splitters(in, c);
    if (drag_split_) {
        in.pressed = in.released = in.dbl = false;
        in.wheel = 0;
    }
    toolbar(in, c, st);
    map_panel(in, c);
    if (!solo_) {
        charts_panel(in, c);
        phylogeny(in, c);
        inspector(in, c);
        browser(in, c);
    }
    c.unclip();
    if (popup_)
        popup(in, c, nullptr); // drawn last, over everything
    tooltip(c);
}

void Viewer::splitters(const UiInput& in, Canvas& c) {
    const i32 z = c.px(3);
    const i32 W = c.width(), H = c.height();
    const Rect sx{map_.right(), bar_.bottom(), insp_.x - map_.right(), H - bar_.bottom()};
    const Rect sy{map_.x, map_.bottom(), map_.w, charts_r_.y - map_.bottom()};
    const Rect sb{charts_r_.right(), charts_r_.y, phylo_r_.x - charts_r_.right(), charts_r_.h};
    const Rect sr{insp_.x, insp_.bottom(), insp_.w, browse_.y - insp_.bottom()};
    auto grow = [z](Rect r, bool vertical) {
        if (vertical) {
            r.x -= z;
            r.w += 2 * z;
        } else {
            r.y -= z;
            r.h += 2 * z;
        }
        return r;
    };
    const Rect zones[4] = {grow(sx, true), grow(sy, false), grow(sb, true), grow(sr, false)};
    if (!drag_split_) {
        for (u32 i = 0; i < 4; ++i) {
            if (zones[i].contains(in.mx, in.my)) {
                cursor_ = (i == 0 || i == 2) ? 1 : 2;
                if (in.pressed)
                    drag_split_ = i + 1;
                break;
            }
        }
        return;
    }
    cursor_ = (drag_split_ == 1 || drag_split_ == 3) ? 1 : 2;
    if (!in.down) {
        drag_split_ = 0;
        return;
    }
    const i32 body_h = H - bar_.h;
    switch (drag_split_) {
    case 1:
        fx_ = static_cast<float>(in.mx) / (W > 0 ? W : 1);
        break;
    case 2:
        fy_ = static_cast<float>(in.my - bar_.h) / (body_h > 0 ? body_h : 1);
        break;
    case 3: {
        const i32 left_w = insp_.x;
        fb_ = static_cast<float>(in.mx) / (left_w > 0 ? left_w : 1);
        break;
    }
    case 4:
        fr_ = static_cast<float>(in.my - bar_.h) / (body_h > 0 ? body_h : 1);
        break;
    default:
        break;
    }
}

// --- the toolbar ---------------------------------------------------------------

void Viewer::toolbar(const UiInput& in, Canvas& c, const RunStatus& st) {
    c.fill(bar_, kPanel);
    const i32 h = c.px(26), y = c.px(4), gap = c.px(6);
    i32 x = c.px(8);
    char buf[256], num[32];
    if (!st.live) {
        // A finished run: a slider over its nodes.csv windows.
        const Rect prev{x, y, c.px(28), h};
        const Rect next{x + c.px(32), y, c.px(28), h};
        const u32 n = st.windows;
        if (button(in, c, prev, "<") && n && st.window > 0)
            command(kCmdWindow, st.window - 1);
        if (button(in, c, next, ">") && n && st.window + 1 < n)
            command(kCmdWindow, st.window + 1);
        x = next.right() + gap;
        const Rect set{x, y, c.px(76), h}; // the settings it ran with (read-only)
        if (button(in, c, set, "Settings"))
            command(kCmdSettings, 0);
        x = set.right() + gap;
        slider_ = Rect{x, y + h / 2 - c.px(4), c.width() / 2, c.px(8)};
        c.fill(slider_, kButton);
        if (n > 1) {
            const i32 hx = slider_.x + static_cast<i32>(static_cast<double>(st.window) / (n - 1) * slider_.w);
            c.fill(Rect{hx - c.px(4), y + c.px(3), c.px(8), h - c.px(6)}, kAccent);
            const Rect hit{slider_.x, y, slider_.w, h};
            if (in.pressed && hit.contains(in.mx, in.my))
                drag_slider_ = true;
            if (!in.down)
                drag_slider_ = false;
            if (drag_slider_) {
                double f = static_cast<double>(in.mx - slider_.x) / (slider_.w > 0 ? slider_.w : 1);
                f = f < 0 ? 0 : (f > 1 ? 1 : f);
                const u32 wnd = static_cast<u32>(f * (n - 1) + 0.5);
                if (wnd != st.window)
                    command(kCmdWindow, wnd);
            }
        }
        commas(cur_.tick, num);
        snprintf(buf, sizeof buf, "%s   window %u of %u, to tick %s", st.title, n ? st.window + 1 : 0, n, num);
        c.text(slider_.right() + c.px(12), y + c.px(4), buf, kFontUi, kText);
        return;
    }

    const bool paused = st.paused || st.finished;
    if (st.finished) {
        c.fill(Rect{x, y, c.px(76), h}, kButton);
        c.text(x + c.px(38), y + c.px(4), "Stopped", kFontUi, kDim, kAlignCenter);
    } else if (button(in, c, Rect{x, y, c.px(76), h}, paused ? "Resume" : "Pause", paused)) {
        command(paused ? kCmdResume : kCmdPause, 0);
    }
    x += c.px(80);
    if (button(in, c, Rect{x, y, c.px(52), h}, "Step") && !st.finished)
        command(kCmdStep, 0);
    x += c.px(52) + gap;
    // Speed: - limit +
    u32 si = kSpeedCount - 1;
    for (u32 i = 0; i < kSpeedCount; ++i)
        if (kSpeeds[i] == st.speed)
            si = i;
    if (button(in, c, Rect{x, y, c.px(24), h}, "-") && si > 0)
        command(kCmdSpeed, kSpeeds[si - 1]);
    x += c.px(26);
    if (st.speed > 0)
        snprintf(buf, sizeof buf, "%g ticks/s", st.speed);
    else
        snprintf(buf, sizeof buf, "flat out");
    c.fill(Rect{x, y, c.px(86), h}, kBg);
    c.text(x + c.px(43), y + c.px(4), buf, kFontUi, kText, kAlignCenter);
    x += c.px(88);
    if (button(in, c, Rect{x, y, c.px(24), h}, "+") && si + 1 < kSpeedCount)
        command(kCmdSpeed, kSpeeds[si + 1]);
    x += c.px(24) + gap;
    if (button(in, c, Rect{x, y, c.px(84), h}, "Snapshot"))
        command(kCmdSnapshot, 0);
    x += c.px(84) + c.px(4);
    if (button(in, c, Rect{x, y, c.px(56), h}, "Stop") && !st.finished)
        command(kCmdStop, 0);
    x += c.px(56) + c.px(4);
    // The settings it started with: fixed for the whole run (docs/config-plan.md C3).
    if (button(in, c, Rect{x, y, c.px(76), h}, "Settings"))
        command(kCmdSettings, 0);
    x += c.px(76) + c.px(12);

    commas(cur_.tick, num);
    i32 n = snprintf(buf, sizeof buf, "tick %s   %.0f ticks/s   %.0f M instr/s", num, st.tps, st.ips / 1e6);
    if (cur_.detect_tick) {
        commas(cur_.detect_tick - 1, num);
        n += snprintf(buf + n, sizeof buf - n, "   first replicator at tick %s", num);
    }
    if (cur_.hashed)
        snprintf(buf + n, sizeof buf - n, "   hash %016llx", static_cast<unsigned long long>(cur_.hash));
    c.text(x, y + c.px(4), buf, kFontUi, kText);
    if (st.note[0])
        c.text(c.width() - c.px(10), y + c.px(4), st.note, kFontUi, kWarn, kAlignRight,
               c.width() / 3);
}

// --- the map ---------------------------------------------------------------------

// Pixels per map unit at zoom 1: the unit square fits the map, or a
// landscape's own box does (so a wide world fills a wide panel).
double Viewer::map_scale() const {
    const double w = map_inner_.w, h = map_inner_.h;
    if (landscape_) {
        const double bw = box_[2] - box_[0] > 0 ? box_[2] - box_[0] : 1, bh = box_[3] - box_[1] > 0 ? box_[3] - box_[1] : 1;
        return 0.96 * (w / bw < h / bh ? w / bw : h / bh);
    }
    return (w < h ? w : h) * 0.92;
}

void Viewer::to_screen(float x, float y, i32* sx, i32* sy) const {
    const double s = map_scale() * zoom_;
    const double cx = map_inner_.x + map_inner_.w / 2.0, cy = map_inner_.y + map_inner_.h / 2.0;
    *sx = static_cast<i32>(floor(cx + (x - 0.5 - pan_x_) * s + 0.5));
    *sy = static_cast<i32>(floor(cy + (y - 0.5 - pan_y_) * s + 0.5));
}

void Viewer::to_world(i32 sx, i32 sy, double* x, double* y) const {
    const double s = map_scale() * zoom_;
    const double cx = map_inner_.x + map_inner_.w / 2.0, cy = map_inner_.y + map_inner_.h / 2.0;
    *x = (sx - cx) / s + 0.5 + pan_x_;
    *y = (sy - cy) / s + 0.5 + pan_y_;
}

// The header's world map / map of life switch (A5), at its right end.
bool Viewer::map_header_toggles(const UiInput& in, Canvas& c, i32 x) {
    (void)x;
    const i32 hh = c.line_h(kFontBold) + c.px(6);
    const char* label = "map of life";
    const i32 w = c.text_w(label, kFontUi) + c.px(16);
    const Rect r{map_.right() - w - c.px(6), map_.y + c.px(2), w, hh - c.px(4)};
    if (button(in, c, r, label, life_mode_)) {
        set_life_mode(!life_mode_);
        return true;
    }
    return false;
}

void Viewer::map_panel(const UiInput& in_raw, Canvas& c) {
    if (life_mode_) { // A5: the map of life in the map's place
        life_panel(in_raw, c);
        return;
    }
    const UiInput& in_hdr = in_raw;
    Rect inner;
    panel_box(c, map_, "Map", &inner);
    map_inner_ = inner;
    char buf[1024], num[32];
    // Header controls.
    const i32 hh = c.line_h(kFontBold) + c.px(6);
    i32 x = map_.x + c.px(56);
    snprintf(buf, sizeof buf, "colour: %s  v", color_by_name(color_by_));
    const Rect cb{x, map_.y + c.px(2), c.text_w(buf, kFontUi) + c.px(16), hh - c.px(4)};
    if (button(in_hdr, c, cb, buf, popup_ == 1))
        open_popup(1, 0, cb);
    x = cb.right() + c.px(6);
    if (color_by_ == kColorSubstrate) { // W8: which type (the T key cycles them too)
        snprintf(buf, sizeof buf, "type: %s  v", substrate_label(sub_type_));
        const Rect tb{x, map_.y + c.px(2), c.text_w(buf, kFontUi) + c.px(16), hh - c.px(4)};
        if (button(in_hdr, c, tb, buf, popup_ == 4))
            open_popup(4, 0, tb);
        x = tb.right() + c.px(6);
    }
    const char* toggles[3] = {"size by bodies", "cargo", "links"};
    bool* flags[3] = {&size_by_bodies_, &show_cargo_, &show_edges_};
    for (u32 i = 0; i < 3; ++i) {
        const Rect r{x, map_.y + c.px(2), c.text_w(toggles[i], kFontUi) + c.px(16), hh - c.px(4)};
        if (button(in_hdr, c, r, toggles[i], *flags[i]))
            *flags[i] = !*flags[i];
        x = r.right() + c.px(4);
    }
    if (map_header_toggles(in_hdr, c, x))
        return; // (switched: the map of life draws from the next frame)

    const u32 n = static_cast<u32>(cur_.node.size());
    node_sx_.resize(n);
    node_sy_.resize(n);
    if (!have_ || n == 0) {
        c.text(inner.x, inner.y, "waiting for the first frame...", kFontUi, kDim);
        return;
    }
    // The overview map (a landscape): the whole land, with the view's frame;
    // a click or drag there moves the view. It takes the mouse first.
    UiInput in = in_raw;
    if (landscape_ && land_w_ && land_h_) {
        const double bw = box_[2] - box_[0], bh = box_[3] - box_[1];
        const i32 mw = c.px(170), mh = static_cast<i32>(mw * (bh > 0 ? bh / bw : 1));
        minimap_ = Rect{inner.right() - mw - c.px(6), inner.bottom() - mh - c.px(24), mw, mh};
        if (in.pressed && minimap_.contains(in.mx, in.my))
            mm_drag_ = true;
        if (!in.down)
            mm_drag_ = false;
        if (mm_drag_ || minimap_.contains(in.mx, in.my)) {
            if (mm_drag_) {
                double fx = static_cast<double>(in.mx - minimap_.x) / minimap_.w;
                double fy = static_cast<double>(in.my - minimap_.y) / minimap_.h;
                fx = fx < 0 ? 0 : (fx > 1 ? 1 : fx);
                fy = fy < 0 ? 0 : (fy > 1 ? 1 : fy);
                pan_x_ = box_[0] + fx * bw - 0.5;
                pan_y_ = box_[1] + fy * bh - 0.5;
                cursor_ = 3;
            }
            in.pressed = in.released = in.dbl = false;
            in.wheel = 0;
            panning_ = false;
        }
    } else {
        minimap_ = Rect{};
    }
    // Interaction: wheel zoom about the cursor, drag to pan, click to select,
    // double-click to fit a landmass (or reset).
    if (in.wheel && inner.contains(in.mx, in.my)) {
        double wx, wy;
        to_world(in.mx, in.my, &wx, &wy);
        zoom_ *= pow(1.25, in.wheel);
        zoom_ = zoom_ < 0.25 ? 0.25 : (zoom_ > 64 ? 64 : zoom_);
        const double s = map_scale() * zoom_;
        pan_x_ = wx - 0.5 - (in.mx - (inner.x + inner.w / 2.0)) / s;
        pan_y_ = wy - 0.5 - (in.my - (inner.y + inner.h / 2.0)) / s;
    }
    const bool dbl = in.dbl && inner.contains(in.mx, in.my); // handled once the nodes are placed
    if (in.pressed && inner.contains(in.mx, in.my)) {
        panning_ = true;
        pan_moved_ = false;
        pan_mx_ = press_x_ = in.mx;
        pan_my_ = press_y_ = in.my;
    }
    if (panning_ && in.down) {
        const double s = map_scale() * zoom_;
        const i32 moved = (in.mx - press_x_) * (in.mx - press_x_) + (in.my - press_y_) * (in.my - press_y_);
        if (moved > c.px(3) * c.px(3))
            pan_moved_ = true;
        if (pan_moved_) {
            pan_x_ -= (in.mx - pan_mx_) / s;
            pan_y_ -= (in.my - pan_my_) / s;
            cursor_ = 3;
        }
        pan_mx_ = in.mx;
        pan_my_ = in.my;
    }

    // The metric's range: fixed, or from the lowest live node to the 95th
    // percentile, so one outlier doesn't flatten every other node's colour.
    // (Metrics are never negative, so their bit patterns sort as values.)
    double lo = 0, hi = 1;
    const bool fixed = metric_range(color_by_, &lo, &hi);
    bool clipped = false;
    Array<u64> keys;
    if (!fixed) {
        for (u32 i = 0; i < n; ++i) {
            const NodeView& v = cur_.node[i];
            if (!v.live || !(v.known >> color_by_ & 1))
                continue;
            double m = node_metric(v, color_by_);
            m = m > 0 ? m : 0;
            u64 bits;
            memcpy(&bits, &m, 8);
            keys.push(bits);
        }
        if (!keys.empty()) {
            sort_u64(keys.data(), keys.size());
            memcpy(&lo, &keys[0], 8);
            memcpy(&hi, &keys[(keys.size() - 1) * 95 / 100], 8);
            double top;
            memcpy(&top, &keys.back(), 8);
            clipped = top > hi;
        }
        if (hi <= lo)
            hi = lo + 1;
    }
    // Node sizes against the 95th percentile of bodies too.
    u32 max_bodies = 1;
    keys.clear();
    for (u32 i = 0; i < n; ++i)
        if (cur_.node[i].live)
            keys.push(cur_.node[i].bodies);
    if (!keys.empty()) {
        sort_u64(keys.data(), keys.size());
        max_bodies = static_cast<u32>(keys[(keys.size() - 1) * 95 / 100]);
        max_bodies = max_bodies ? max_bodies : 1;
    }
    for (u32 i = 0; i < n; ++i)
        to_screen(cur_.node[i].x, cur_.node[i].y, &node_sx_[i], &node_sy_[i]);
    // Zoomed out over many nodes, draw simply: nodes as dots, short paths
    // hidden (Phase 7). The typical spacing between nodes, in pixels:
    const double base = map_scale();
    const double box_area = landscape_ ? (box_[2] - box_[0]) * (box_[3] - box_[1]) : 1.0;
    const double spacing_px = sqrt(box_area / (n ? n : 1)) * base * zoom_;
    const bool simple = spacing_px < c.px(10);

    c.clip(inner);
    if (landscape_ && land_w_ && land_h_) { // land and water behind everything
        update_ground();
        i32 x0, y0, x1, y1;
        to_screen(box_[0], box_[1], &x0, &y0);
        to_screen(box_[2], box_[3], &x1, &y1);
        c.fill(inner, 0x0C1520);
        c.image(Rect{x0, y0, x1 - x0, y1 - y0}, shown_land().data(), static_cast<i32>(land_w_),
                static_cast<i32>(land_h_));
    }
    const u32 kBridgeOpen = 0xE8A840, kBridgeShut = 0x6E5A34;
    u32 bridges = 0, open_bridges = 0;
    if (show_edges_) {
        const i32 min_len2 = simple ? c.px(4) * c.px(4) : 0;
        for (usize i = 0; i < cur_.edge.size(); ++i) {
            const EdgeView& e = cur_.edge[i];
            if (e.a >= n || e.b >= n)
                continue;
            const i32 ax = node_sx_[e.a], ay = node_sy_[e.a], bx = node_sx_[e.b], by = node_sy_[e.b];
            if (e.bridge) {
                ++bridges;
                open_bridges += !e.shut;
                if (e.shut)
                    c.dashed(ax, ay, bx, by, kBridgeShut, c.px(5));
                else
                    c.line(ax, ay, bx, by, kBridgeOpen, c.px(2));
                continue;
            }
            if ((bx - ax) * (bx - ax) + (by - ay) * (by - ay) < min_len2 && !e.long_link)
                continue;
            if (e.down)
                c.dashed(ax, ay, bx, by, kEdgeDown, c.px(4));
            else // (over a landscape's land, a lighter line)
                c.line(ax, ay, bx, by, e.long_link ? kEdgeLong : (landscape_ ? 0x76836F : kEdge));
        }
    }
    if (show_cargo_) {
        const i32 d = c.px(2) > 1 ? c.px(2) : 2;
        for (usize i = 0; i < cur_.cargo.size(); ++i) {
            i32 sx, sy;
            to_screen(cur_.cargo[i].x, cur_.cargo[i].y, &sx, &sy);
            c.fill(Rect{sx - d / 2, sy - d / 2, d, d}, kCargo);
        }
    }
    // Nodes, and which one the mouse is over.
    hover_node_ = kNone;
    i32 best_d = 0x7FFFFFFF;
    for (u32 i = 0; i < n; ++i) {
        const NodeView& v = cur_.node[i];
        // Radius: up to 11 px, but no more than about half the spacing
        // between nodes, so a crowded map doesn't become one blob.
        double rmax = 0.45 * spacing_px;
        rmax = rmax > c.px(11) ? c.px(11) : (rmax < c.px(2.5f) ? c.px(2.5f) : rmax);
        i32 r = static_cast<i32>(rmax * 0.55 + 0.5);
        if (size_by_bodies_) {
            const double f = static_cast<double>(v.bodies) / max_bodies;
            r = static_cast<i32>(rmax * (0.3 + 0.7 * sqrt(f < 1.0 ? f : 1.0)) + 0.5);
        }
        r = r < 1 ? 1 : r;
        const i32 sx = node_sx_[i], sy = node_sy_[i];
        const i32 dx = in.mx - sx, dy = in.my - sy, d2 = dx * dx + dy * dy;
        const i32 reach = r + c.px(4);
        if (inner.contains(in.mx, in.my) && d2 <= reach * reach && d2 < best_d) {
            best_d = d2;
            hover_node_ = i;
        }
        if (sx < inner.x - 20 || sy < inner.y - 20 || sx > inner.right() + 20 || sy > inner.bottom() + 20)
            continue;
        // Recent collapses (red) and recoveries (green) fade over 400 ticks.
        if (i < flash_tick_.size() && flash_tick_[i] && cur_.tick + 1 >= flash_tick_[i]) {
            const double age = static_cast<double>(cur_.tick + 1 - flash_tick_[i]) / 400.0;
            if (age < 1) {
                const u32 col = flash_kind_[i] == kEvCollapse ? kBad : kGood;
                const i32 rr = r + c.px(static_cast<float>(4 + 14 * age));
                c.circle(sx, sy, rr, 0, mix_color(col, kPanel, age), false);
            }
        }
        if (!v.live) {
            if (simple)
                c.fill(Rect{sx - 1, sy - 1, 2, 2}, kFaint);
            else
                c.circle(sx, sy, r, 0, kFaint, false);
            continue;
        }
        u32 col;
        if (!(v.known >> color_by_ & 1))
            col = 0x606060;
        else if (color_by_ == kColorLineage)
            col = lineage_color(v.top_lineage);
        else if (color_by_ == kColorSubstrate) // W8: the type's level, five fixed colours
            col = substrate_level_color(v.sub_level[sub_type_]);
        else
            col = ramp_color((node_metric(v, color_by_) - lo) / (hi - lo));
        if (simple) { // a dot, sized a little by bodies
            const i32 d = size_by_bodies_ && v.bodies * 2 > max_bodies ? 3 : 2;
            c.fill(Rect{sx - d / 2, sy - d / 2, d, d}, col);
            if (i == sel_node_)
                c.circle(sx, sy, c.px(5), 0, 0xFFFFFF, false);
            else if (v.holding)
                c.circle(sx, sy, c.px(4), 0, kAccent, false);
            continue;
        }
        c.circle(sx, sy, r, col, mix_color(col, 0x000000, 0.4), true);
        if (v.holding)
            c.circle(sx, sy, r + c.px(3), 0, kAccent, false);
        if (i == sel_node_) {
            c.circle(sx, sy, r + c.px(2), 0, 0xFFFFFF, false);
            c.circle(sx, sy, r + c.px(3), 0, 0xFFFFFF, false);
        }
    }
    // X7: nodes a disaster struck in the last kStrikeFade ticks: a diamond
    // around the node, coloured by kind, fading with age (the latest strike
    // on a node shows).
    u32 kinds_seen = 0, struck_shown = 0;
    {
        Array<u32> sk;
        Array<float> sa;
        sk.resize(n);
        sk.fill(kNone);
        sa.resize(n);
        const u64 now = cur_.tick;
        for (usize i = strikes_.size(); i-- > 0;) {
            const Strike& s = strikes_[i];
            if (s.tick > now)
                continue;
            if (now - s.tick >= kStrikeFade)
                break;
            if (s.node < n && sk[s.node] == kNone) {
                sk[s.node] = s.kind;
                sa[s.node] = static_cast<float>(static_cast<double>(now - s.tick) / static_cast<double>(kStrikeFade));
            }
        }
        double rmax = 0.45 * spacing_px;
        rmax = rmax > c.px(11) ? c.px(11) : (rmax < c.px(2.5f) ? c.px(2.5f) : rmax);
        const i32 d = static_cast<i32>(rmax + 0.5) + c.px(5);
        for (u32 i = 0; i < n; ++i) {
            if (sk[i] == kNone)
                continue;
            const i32 sx = node_sx_[i], sy = node_sy_[i];
            if (sx < inner.x - 20 || sy < inner.y - 20 || sx > inner.right() + 20 || sy > inner.bottom() + 20)
                continue;
            const u32 col = mix_color(strike_color(sk[i]), kPanel, sa[i] * 0.85);
            const i32 pts[10] = {sx, sy - d, sx + d, sy, sx, sy + d, sx - d, sy, sx, sy - d};
            c.polyline(pts, 5, col, c.px(2));
            kinds_seen |= 1u << (sk[i] - kEvSurge);
            ++struck_shown;
        }
    }
    if (panning_ && in.released) {
        if (!pan_moved_ && hover_node_ != kNone) {
            select_node(hover_node_);
            auto_ = false;
        }
        panning_ = false;
    }
    if (!in.down)
        panning_ = false;
    // Double-click: in a landscape, fit the landmass under the mouse (its
    // nearest node's); elsewhere, or on open water, reset the view.
    if (dbl) {
        u32 near = hover_node_;
        if (near == kNone && landscape_) {
            i32 best = c.px(40) * c.px(40);
            for (u32 i = 0; i < n; ++i) {
                const i32 dx = in.mx - node_sx_[i], dy = in.my - node_sy_[i];
                if (dx * dx + dy * dy < best) {
                    best = dx * dx + dy * dy;
                    near = i;
                }
            }
        }
        if (landscape_ && near != kNone) {
            fit_mass(cur_.node[near].mass);
        } else {
            zoom_ = 1;
            pan_x_ = pan_y_ = 0;
        }
    }

    // Landmass labels, where there's room for them.
    if (landscape_) {
        for (usize k = 0; k < masses_.size(); ++k) {
            const MassView& m = masses_[k];
            const double extent = sqrt(static_cast<double>(m.nodes)) * spacing_px;
            if (extent < c.px(90))
                continue;
            i32 sx, sy;
            to_screen(m.x, m.y, &sx, &sy);
            snprintf(buf, sizeof buf, "landmass %u  (%u nodes)", m.id, m.nodes);
            const i32 tw = c.text_w(buf, kFontSmall), th = c.line_h(kFontSmall);
            c.fill(Rect{sx - tw / 2 - c.px(4), sy - th / 2 - c.px(1), tw + c.px(8), th + c.px(2)}, 0x101418);
            c.text(sx, sy - th / 2, buf, kFontSmall, 0xC8D4C0, kAlignCenter);
        }
    }

    // The overview map: the whole land and the view's frame.
    if (minimap_.w > 0) {
        c.fill(Rect{minimap_.x - 1, minimap_.y - 1, minimap_.w + 2, minimap_.h + 2}, 0x404858);
        c.image(minimap_, shown_land().data(), static_cast<i32>(land_w_), static_cast<i32>(land_h_));
        double wx0, wy0, wx1, wy1;
        to_world(inner.x, inner.y, &wx0, &wy0);
        to_world(inner.right(), inner.bottom(), &wx1, &wy1);
        const double bw = box_[2] - box_[0], bh = box_[3] - box_[1];
        auto mx = [&](double wx) { return minimap_.x + static_cast<i32>((wx - box_[0]) / bw * minimap_.w); };
        auto my = [&](double wy) { return minimap_.y + static_cast<i32>((wy - box_[1]) / bh * minimap_.h); };
        i32 rx0 = mx(wx0), ry0 = my(wy0), rx1 = mx(wx1), ry1 = my(wy1);
        rx0 = rx0 < minimap_.x ? minimap_.x : rx0;
        ry0 = ry0 < minimap_.y ? minimap_.y : ry0;
        rx1 = rx1 > minimap_.right() ? minimap_.right() : rx1;
        ry1 = ry1 > minimap_.bottom() ? minimap_.bottom() : ry1;
        if (rx1 > rx0 && ry1 > ry0)
            c.frame(Rect{rx0, ry0, rx1 - rx0, ry1 - ry0}, 0xFFFFFF);
        if (sel_node_ < n) {
            const i32 sx = mx(cur_.node[sel_node_].x), sy = my(cur_.node[sel_node_].y);
            c.fill(Rect{sx - 2, sy - 2, 4, 4}, 0xFFFFFF);
        }
    }

    // The ground's key (top left): each substrate family's colour, and barren.
    if (ground_shown()) {
        const i32 ky = inner.y + c.px(6);
        i32 w = c.text_w("ground by substrate:", kFontSmall) + c.px(6);
        for (u32 f = 0; f < kGroundFamilies; ++f)
            w += c.px(13) + c.text_w(ground_family_name(f), kFontSmall) + c.px(8);
        w += c.px(13) + c.text_w("poor", kFontSmall) + c.px(8) + c.text_w("(B: plain land)", kFontSmall);
        c.fill(Rect{inner.x + c.px(3), ky - c.px(3), w + c.px(8), c.line_h(kFontSmall) + c.px(6)}, 0x0C1520);
        i32 kx = inner.x + c.px(7);
        c.text(kx, ky, "ground by substrate:", kFontSmall, kDim);
        kx += c.text_w("ground by substrate:", kFontSmall) + c.px(6);
        for (u32 f = 0; f <= kGroundFamilies; ++f) {
            const bool poor = f == kGroundFamilies;
            const char* name = poor ? "poor" : ground_family_name(f);
            c.fill(Rect{kx, ky + c.px(2), c.px(10), c.px(9)}, poor ? ground_poor_color() : ground_family_color(f));
            c.text(kx + c.px(13), ky, name, kFontSmall, kDim);
            kx += c.px(13) + c.text_w(name, kFontSmall) + c.px(8);
        }
        c.text(kx, ky, "(B: plain land)", kFontSmall, kFaint);
    }

    // The legend and a summary line.
    const i32 ly = inner.bottom() - c.px(18);
    if (struck_shown) { // X7's key: the kinds marked now
        i32 kx = inner.x + c.px(4);
        const i32 ky = ly - c.line_h(kFontSmall) - c.px(2);
        snprintf(buf, sizeof buf, "struck in the last %llu ticks:", static_cast<unsigned long long>(kStrikeFade));
        c.text(kx, ky, buf, kFontSmall, kDim);
        kx += c.text_w(buf, kFontSmall) + c.px(6);
        static const char* const kStrikeName[5] = {"surge", "crash", "burnout", "disk failure", "lightning"};
        for (u32 k = 0; k < 5; ++k) {
            if (!(kinds_seen >> k & 1))
                continue;
            c.text(kx, ky, kStrikeName[k], kFontSmall, strike_color(kEvSurge + k));
            kx += c.text_w(kStrikeName[k], kFontSmall) + c.px(8);
        }
    }
    if (color_by_ == kColorLineage) {
        c.text(inner.x + c.px(4), ly, "each node coloured by its largest lineage", kFontSmall, kDim);
    } else if (color_by_ == kColorSubstrate) { // W8's key: the five levels
        i32 kx = inner.x + c.px(4);
        double floor_seen = 0.1;
        for (u32 i = 0; i < n; ++i)
            if (cur_.node[i].sub_level[sub_type_] == 0 && cur_.node[i].sub[sub_type_] > 0) {
                floor_seen = cur_.node[i].sub[sub_type_];
                break;
            }
        snprintf(buf, sizeof buf, "%s:", substrate_label(sub_type_));
        c.text(kx, ly - c.px(1), buf, kFontSmall, kDim);
        kx += c.text_w(buf, kFontSmall) + c.px(6);
        for (u32 k = 0; k < kSubstrateLevels; ++k) {
            const double m[kSubstrateLevels] = {floor_seen, 0.5, 1.0, 1.5, 2.0};
            c.fill(Rect{kx, ly + c.px(1), c.px(12), c.px(9)}, substrate_level_color(k));
            snprintf(buf, sizeof buf, "%gx", m[k]);
            c.text(kx + c.px(15), ly - c.px(1), buf, kFontSmall, kDim);
            kx += c.px(15) + c.text_w(buf, kFontSmall) + c.px(8);
        }
        c.text(kx, ly - c.px(1), "(T: next type)", kFontSmall, kFaint);
    } else {
        const i32 lw = c.px(120);
        for (i32 k = 0; k < lw; ++k)
            c.fill(Rect{inner.x + c.px(4) + k, ly + c.px(2), 1, c.px(8)}, ramp_color(static_cast<double>(k) / lw));
        char a[32], b[32];
        nice_num(lo, a);
        nice_num(hi, b);
        snprintf(buf, sizeof buf, "%s  %s - %s%s", color_by_name(color_by_), a, b, clipped ? " and up" : "");
        c.text(inner.x + c.px(8) + lw, ly - c.px(1), buf, kFontSmall, kDim);
    }
    u32 live = 0;
    for (u32 i = 0; i < n; ++i)
        live += cur_.node[i].live;
    commas(cur_.cargo.size(), num);
    // The summary, then the mouse hints if there's room beside the legend.
    i32 len;
    if (landscape_)
        len = snprintf(buf, sizeof buf, "%u nodes (%u live) on %u landmasses   %u paths   %u bridges (%u open)   %s in flight",
                      n, live, static_cast<unsigned>(masses_.size()),
                      static_cast<unsigned>(cur_.edge.size() - bridges), bridges, open_bridges, num);
    else
        len = snprintf(buf, sizeof buf, "%u nodes (%u live)   %u links   %s cargoes in flight", n, live,
                      static_cast<unsigned>(cur_.edge.size()), num);
    const i32 room = inner.w - c.px(330); // the legend's side
    const char* hints = landscape_ ? "   wheel: zoom, drag: pan, double-click: fit a landmass" : "   wheel: zoom, drag: pan";
    if (c.text_w(buf, kFontSmall) + c.text_w(hints, kFontSmall) < room)
        snprintf(buf + len, sizeof buf - len, "%s", hints);
    if (c.text_w(buf, kFontSmall) < room + c.px(330) - c.px(140))
        c.text(inner.right() - c.px(4), ly - c.px(1), buf, kFontSmall, kDim, kAlignRight,
               room > c.px(100) ? room : c.px(100));
    c.unclip();

    if (hover_node_ != kNone && !panning_) {
        const NodeView& v = cur_.node[hover_node_];
        char lin[32];
        lineage_text(v.top_lineage, lin, sizeof lin);
        if (!v.live) {
            snprintf(buf, sizeof buf, "node %u: a fallow site (lives so far: %u)", hover_node_, v.life);
        } else {
            snprintf(buf, sizeof buf,
                     "node %u   (life %u, health %u)\n%u bodies, %u CPUs\n%u lineages, %u colonist members\n"
                     "largest lineage %s: %u members\nbirths %.3g / tick, sent %.3g, arrived %.3g\n"
                     "free CPU %.0f%%, waste %.0f%%\nmemory %u, millicores %u, GPU %u, NPU %u, disk %u\n"
                     "climate %.3g, sparks %.3g / tick, radiation %.3gx\ndegree %u%s",
                     hover_node_, v.life, v.health, v.bodies, v.cpus, v.lineages, v.colonists, lin, v.top_alive,
                     v.births, v.sent, v.arrived, 100.0 * node_metric(v, kColorFreeCpu),
                     100.0 * node_metric(v, kColorWaste), v.soup_size, v.millicores, v.gpus, v.npus, v.disk, v.cool,
                     v.spark_rate, v.radiation, v.degree, v.holding ? "\nholds the selected genotype" : "");
        }
        if (landscape_) {
            const usize at = strlen(buf);
            snprintf(buf + at, sizeof buf - at, "\nlandmass %u", v.mass);
        }
        if (v.live) { // W8 and X7: its substrate, occupancy and disasters, where known
            char more[400];
            usize at = 0;
            more[0] = '\0';
            auto left = [&]() { return at < sizeof more ? sizeof more - at : 0; };
            auto clamp = [&]() { at = at < sizeof more ? at : sizeof more - 1; };
            if (v.known >> kColorSubstrate & 1) {
                at += snprintf(more + at, left(), "\nsubstrate: P %gx E %gx GPU %gx NPU %gx", v.sub[0], v.sub[1],
                               v.sub[2], v.sub[3]);
                clamp();
                at += snprintf(more + at, left(), "\n  memory %gx/%gx, disk %gx/%gx (fast/slow)", v.sub[4], v.sub[5],
                               v.sub[6], v.sub[7]);
                clamp();
            }
            if (v.known >> kColorOccupancy & 1) {
                at += snprintf(more + at, left(), "\noccupancy %.0f%%", 100.0 * v.occupancy);
                clamp();
            }
            if (v.warp_lanes) { // docs/inject-plan.md W1: its GPU's warp width (docs/lanes-plan.md L1: the most a warp has)
                at += snprintf(more + at, left(), "\nwarps of up to %u lanes", v.warp_lanes);
                clamp();
            }
            if ((v.known >> kColorSymptoms & 1) && (v.symptoms > 0 || v.struck || v.cores_offline)) {
                at += snprintf(more + at, left(), "\ndisasters: symptoms %.0f%% of ticks, struck %u, %u cores offline",
                               100.0 * v.symptoms, v.struck, v.cores_offline);
                clamp();
            }
            u32 sk = 0;
            double sa = 0;
            if (strike_marker(hover_node_, &sk, &sa)) {
                static const char* const kStrikeWord[5] = {"a power surge", "an OS crash", "a burnout", "a disk failure",
                                                           "lightning"};
                snprintf(more + at, left(), "\nstruck by %s %.0f ticks ago", kStrikeWord[sk - kEvSurge],
                         sa * static_cast<double>(kStrikeFade));
            }
            const usize bl = strlen(buf);
            snprintf(buf + bl, sizeof buf - bl, "%s", more);
        }
        set_tooltip(buf);
    }
}

// Zooms and pans so a landmass's nodes fill most of the map.
// The ground by substrate: rebuilt when the nodes' substrates change (a
// checksum of them), at most every kGroundEvery frames (they drift with
// world.drift, a little every tick).
void Viewer::update_ground() {
    ++ground_age_;
    if (!ground_on_ || !landscape_ || !land_w_ || !land_h_ || land_img_.size() != static_cast<usize>(land_w_) * land_h_)
        return;
    u64 sum = 1469598103934665603ull;
    for (usize i = 0; i < cur_.node.size(); ++i) {
        const NodeView& v = cur_.node[i];
        sum = (sum ^ (v.known >> kColorSubstrate & 1)) * 1099511628211ull;
        for (u32 t = 0; t < kSubstrateTypes; ++t) {
            u32 bits = 0;
            memcpy(&bits, &v.sub[t], 4);
            sum = (sum ^ bits) * 1099511628211ull;
        }
        u32 xb = 0;
        memcpy(&xb, &v.x, 4);
        sum = (sum ^ xb) * 1099511628211ull;
    }
    if (ground_ok_ && (sum == ground_sum_ || ground_age_ < kGroundEvery))
        return;
    ground_ok_ = substrate_ground(land_img_.data(), land_w_, land_h_, box_, cur_.node.data(),
                                 static_cast<u32>(cur_.node.size()), &ground_img_);
    ground_sum_ = sum;
    ground_age_ = 0;
}

void Viewer::fit_mass(u32 mass) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (usize i = 0; i < cur_.node.size(); ++i) {
        const NodeView& v = cur_.node[i];
        if (v.mass != mass)
            continue;
        x0 = v.x < x0 ? v.x : x0;
        y0 = v.y < y0 ? v.y : y0;
        x1 = v.x > x1 ? v.x : x1;
        y1 = v.y > y1 ? v.y : y1;
    }
    if (x1 < x0)
        return;
    const double inner_w = map_inner_.w, inner_h = map_inner_.h;
    const double base = map_scale();
    const double bw = x1 - x0 > 0.01 ? x1 - x0 : 0.01, bh = y1 - y0 > 0.01 ? y1 - y0 : 0.01;
    double z = 0.85 * (inner_w / (bw * base) < inner_h / (bh * base) ? inner_w / (bw * base) : inner_h / (bh * base));
    zoom_ = z < 0.25 ? 0.25 : (z > 64 ? 64 : z);
    pan_x_ = 0.5 * (x0 + x1) - 0.5;
    pan_y_ = 0.5 * (y0 + y1) - 0.5;
}

// --- charts ----------------------------------------------------------------------

// The whole log's span of ticks (one tick wide until there are two samples).
TickSpan Viewer::chart_full() const {
    const usize n = charts_.size();
    const double t0 = n ? static_cast<double>(charts_.tick(0)) : 0.0;
    const double t1 = n ? static_cast<double>(charts_.tick(n - 1)) : 1.0;
    return TickSpan{t0, t1 > t0 ? t1 : t0 + 1.0};
}

void Viewer::charts_panel(const UiInput& in, Canvas& c) {
    Rect inner;
    panel_box(c, charts_r_, "Charts", &inner);
    char buf[256];
    const usize n = charts_.size();
    const i32 each = inner.h / static_cast<i32>(kCharts);
    const i32 lab = c.px(52);
    const i32 head = c.line_h(kFontSmall) + c.px(2);
    const i32 plot_x = inner.x + lab, plot_w = inner.w - lab;
    const double across = plot_w > 1 ? plot_w - 1 : 1; // pixels from the span's first tick to its last
    // Each chart: a header (its series) over its plot; the last has the ticks under it.
    for (u32 k = 0; k < kCharts; ++k) {
        const i32 cy = inner.y + static_cast<i32>(k) * each, ch = each - c.px(4);
        cplot_[k] = Rect{plot_x, cy + head, plot_w, ch - head - (k + 1 == kCharts ? c.line_h(kFontSmall) : 0)};
    }

    // The span of ticks every chart shows (2026-11-09): the whole run, or the
    // part the wheel zoomed into about the tick under the mouse. Dragging a
    // plot pans it; a double-click on one, Home or "whole run" shows the whole
    // run again. Zoomed into the run's end, it keeps up with the newest sample.
    const TickSpan full = chart_full();
    if (czoom_) {
        if (cfollow_)
            cspan_ = TickSpan{full.hi - cspan_.width(), full.hi};
        cspan_ = pan_span(cspan_, 0, full);
        czoom_ = cspan_.width() < full.width();
    }
    TickSpan view = czoom_ ? cspan_ : full;
    i32 over = -1; // the plot under the mouse
    for (u32 k = 0; k < kCharts; ++k)
        over = cplot_[k].contains(in.mx, in.my) ? static_cast<i32>(k) : over;
    if (n > 1 && over >= 0) {
        if (in.wheel) {
            const double at = view.lo + (in.mx - plot_x) / across * view.width();
            const double least = 3.0 * full.width() / static_cast<double>(n - 1); // three samples apart at least
            cspan_ = zoom_span(view, at, in.wheel, full, least);
            czoom_ = cspan_.width() < full.width();
            cfollow_ = czoom_ && cspan_.hi >= full.hi;
            view = czoom_ ? cspan_ : full;
        }
        if (in.dbl) {
            charts_whole_run();
            view = full;
        } else if (in.pressed && czoom_) {
            cpan_ = true;
            cpan_x_ = in.mx;
            cpan_from_ = cspan_;
        }
    }
    if (cpan_ && (!in.down || !czoom_))
        cpan_ = false;
    if (cpan_) {
        cspan_ = pan_span(cpan_from_, -(in.mx - cpan_x_) / across * cpan_from_.width(), full);
        cfollow_ = cspan_.hi >= full.hi;
        view = cspan_;
        cursor_ = 1;
    }
    // The header's right end: "whole run" while zoomed, the log's thinning,
    // and the mouse's hints where there is room.
    {
        i32 right = charts_r_.right() - c.px(8);
        if (czoom_) {
            const char* label = "whole run";
            const i32 hh = c.line_h(kFontBold) + c.px(6), w = c.text_w(label, kFontUi) + c.px(16);
            const Rect r{charts_r_.right() - w - c.px(6), charts_r_.y + c.px(2), w, hh - c.px(4)};
            if (button(in, c, r, label)) {
                charts_whole_run();
                view = full;
            }
            right = r.x - c.px(8);
        }
        const i32 left = charts_r_.x + c.text_w("Charts", kFontBold) + c.px(20);
        if (charts_.step() > 1) {
            snprintf(buf, sizeof buf, "every %u samples shown", charts_.step());
            c.text(right, charts_r_.y + c.px(4), buf, kFontSmall, kDim, kAlignRight);
            right -= c.text_w(buf, kFontSmall) + c.px(12);
        }
        const char* hint = czoom_ ? "drag: pan, double-click: whole run" : "wheel: zoom the ticks";
        if (n > 1 && right - c.text_w(hint, kFontSmall) >= left)
            c.text(right, charts_r_.y + c.px(4), hint, kFontSmall, kFaint, kAlignRight);
    }
    const double vw = view.width() > 0 ? view.width() : 1.0;
    // Samples [a, b) are in the span; one more each side reaches its edges.
    auto first_after = [&](double t, bool or_at) { // the first sample at (or_at) or after t
        usize lo = 0, hi = n;
        while (lo < hi) {
            const usize mid = (lo + hi) / 2;
            const double m = static_cast<double>(charts_.tick(mid));
            if (or_at ? m < t : m <= t)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    };
    const usize a = first_after(view.lo, true), b = first_after(view.hi, false);
    const usize d0 = a ? a - 1 : 0, d1 = b < n ? b + 1 : n;
    auto x_of = [&](double t) {
        double x = plot_x + (t - view.lo) / vw * across;
        x = x < -1e6 ? -1e6 : (x > 1e6 ? 1e6 : x);
        return static_cast<i32>(floor(x + 0.5));
    };
    // The sample under the mouse (shared by every chart): the nearest.
    usize hover = ~static_cast<usize>(0);
    if (n > 1 && in.mx >= plot_x && in.mx < plot_x + plot_w && inner.contains(in.mx, in.my)) {
        const double t = view.lo + (in.mx - plot_x) / across * vw;
        usize j = first_after(t, true);
        j = j < n ? j : n - 1;
        if (j > 0 && t - static_cast<double>(charts_.tick(j - 1)) < static_cast<double>(charts_.tick(j)) - t)
            --j;
        hover = j;
    }
    Array<i32> pts;
    for (u32 k = 0; k < kCharts; ++k) {
        const Rect ck{inner.x, inner.y + static_cast<i32>(k) * each, inner.w, each - c.px(4)};
        const Rect hr{ck.x, ck.y, ck.w, head};
        // The header: each series in its colour (with the value under the mouse).
        i32 x = ck.x + c.px(2);
        bool any = false;
        for (u32 s = 0; s < kSeries; ++s) {
            const u32 col = series_[k][s];
            if (col == kNone)
                continue;
            any = true;
            if (hover < n)
                snprintf(buf, sizeof buf, "%s %.4g", stats_column(col), charts_.value(hover, col));
            else
                snprintf(buf, sizeof buf, "%s", stats_column(col));
            c.text(x, ck.y, buf, kFontSmall, kSeriesColor[s]);
            x += c.text_w(buf, kFontSmall) + c.px(12);
        }
        if (!any)
            c.text(x, ck.y, "(click to pick series)", kFontSmall, kDim);
        if (hr.contains(in.mx, in.my)) {
            cursor_ = 3;
            if (in.pressed)
                open_popup(3, k, Rect{hr.x + c.px(40), hr.y, 0, hr.h});
        }
        const Rect& pr = cplot_[k];
        c.fill(pr, 0x1A1D23);
        if (n < 2 || !any) {
            if (k == 0)
                c.text(pr.x + c.px(6), pr.y + c.px(4), n < 2 ? "waiting for samples..." : "", kFontSmall, kDim);
            continue;
        }
        // The values' range over the samples in the span (zoomed in, the
        // part's own; its neighbours' if it falls between two samples).
        const usize v0 = a < b ? a : d0, v1 = a < b ? b : d1;
        double vmax = 0, vmin = 0;
        for (u32 s = 0; s < kSeries; ++s) {
            const u32 col = series_[k][s];
            if (col == kNone)
                continue;
            for (usize i = v0; i < v1; ++i) {
                const double v = charts_.value(i, col);
                vmax = v > vmax ? v : vmax;
                vmin = v < vmin ? v : vmin;
            }
        }
        if (vmax <= vmin)
            vmax = vmin + 1;
        const double range = (vmax - vmin) * 1.05;
        nice_num(vmax, buf);
        c.text(pr.x - c.px(4), pr.y, buf, kFontSmall, kDim, kAlignRight);
        nice_num(vmin, buf);
        c.text(pr.x - c.px(4), pr.bottom() - c.line_h(kFontSmall), buf, kFontSmall, kDim, kAlignRight);
        c.clip(pr);
        // Milestones: the first event of each kind, as a labelled line.
        static const char kLetter[] = "RGNCDS";
        for (u32 e = 0; e < 6; ++e) {
            const double et = static_cast<double>(first_event_[e]) - 1;
            if (!first_event_[e] || et < view.lo || et > view.hi)
                continue;
            const i32 ex = x_of(et);
            c.dashed(ex, pr.y, ex, pr.bottom(), kWarn, c.px(3));
            const char l[2] = {kLetter[e], 0};
            c.text(ex + c.px(2), pr.y, l, kFontSmall, kWarn);
        }
        // Collapses (red) and recoveries (green) as ticks along the bottom.
        for (usize i = events_.size(), shown = 0; i-- > 0 && shown < 4000;) {
            const ViewEvent& e = events_[i];
            const double et = static_cast<double>(e.tick);
            if ((e.kind != kEvCollapse && e.kind != kEvRecovered) || et < view.lo || et > view.hi)
                continue;
            ++shown;
            c.fill(Rect{x_of(et), pr.bottom() - c.px(4), 1, c.px(4)}, e.kind == kEvCollapse ? kBad : kGood);
        }
        for (u32 s = 0; s < kSeries; ++s) {
            const u32 col = series_[k][s];
            if (col == kNone)
                continue;
            pts.resize(2 * (d1 - d0));
            for (usize i = d0; i < d1; ++i) {
                pts[2 * (i - d0)] = x_of(static_cast<double>(charts_.tick(i)));
                pts[2 * (i - d0) + 1] =
                    pr.bottom() - 1 - static_cast<i32>((charts_.value(i, col) - vmin) / range * (pr.h - 1));
            }
            c.polyline(pts.data(), static_cast<u32>(d1 - d0), kSeriesColor[s]);
        }
        if (hover < n) {
            const i32 hx = x_of(static_cast<double>(charts_.tick(hover)));
            c.line(hx, pr.y, hx, pr.bottom(), kFaint);
        }
        c.unclip();
        if (k + 1 == kCharts) { // the ticks: the span's ends (zoomed: the part's)
            char ta[32], tb[32];
            commas(static_cast<u64>(view.lo + 0.5), ta);
            commas(static_cast<u64>(view.hi + 0.5), tb);
            c.text(pr.x, pr.bottom(), ta, kFontSmall, kDim);
            c.text(pr.right(), pr.bottom(), tb, kFontSmall, kDim, kAlignRight);
            if (hover < n) {
                commas(charts_.tick(hover), ta);
                snprintf(buf, sizeof buf, "tick %s", ta);
                c.text(pr.x + pr.w / 2, pr.bottom(), buf, kFontSmall, kText, kAlignCenter);
            }
        }
    }
}

// --- the phylogeny ---------------------------------------------------------------

void Viewer::phylogeny(const UiInput& in, Canvas& c) {
    char title[128], lin[32];
    lineage_text(sel_lineage_, lin, sizeof lin);
    if (phylo_lineage_ != sel_lineage_ || phylo_saves_ != saves_.size()) {
        phylo_layout(saves_.data(), saves_.size(), sel_lineage_, &phylo_);
        phylo_lineage_ = sel_lineage_;
        phylo_saves_ = saves_.size();
    }
    if (kinds_) {
        char a[32];
        commas(novelty_window_ ? novelty_window_ : kNoveltyListWindow, a);
        snprintf(title, sizeof title, "New kinds of life: windows of %s ticks", a);
    } else if (sel_lineage_) {
        snprintf(title, sizeof title, "Phylogeny: lineage %s, %u genotypes", lin,
                 static_cast<unsigned>(phylo_.size()));
    } else {
        snprintf(title, sizeof title, "Phylogeny");
    }
    Rect inner;
    panel_box(c, phylo_r_, title, &inner);
    { // A2: the panel shows the phylogeny or the new kinds of life
        const i32 hh = c.line_h(kFontBold) + c.px(6);
        const char* label = "new kinds";
        const i32 w = c.text_w(label, kFontUi) + c.px(16);
        const Rect r{phylo_r_.right() - w - c.px(6), phylo_r_.y + c.px(2), w, hh - c.px(4)};
        if (button(in, c, r, label, kinds_)) {
            kinds_ = !kinds_;
            kinds_scroll_ = 0;
            return;
        }
    }
    if (kinds_) {
        kinds_panel(in, c, inner);
        return;
    }
    if (phylo_.empty()) {
        c.text(inner.x, inner.y, sel_lineage_ ? "no saved genotypes in this lineage yet"
                                              : "select a lineage: a node, a body or a genome",
               kFontSmall, kDim, kAlignLeft, inner.w);
        return;
    }
    const i32 rh = c.line_h(kFontSmall) + c.px(3);
    const i32 rows = static_cast<i32>(phylo_.size());
    { // The wheel or the scrollbar, in pixels; the tree keeps clear of the bar.
        Rect track;
        const Rect area = inner;
        scroll_split(c, area, &inner, &track);
        phylo_bar_.update(in, c, area, track, rows * rh, inner.h, 3 * rh, &phylo_scroll_);
    }
    u64 tmin = phylo_[0].tick, tmax = phylo_[0].tick;
    for (usize i = 0; i < phylo_.size(); ++i) {
        tmin = phylo_[i].tick < tmin ? phylo_[i].tick : tmin;
        tmax = phylo_[i].tick > tmax ? phylo_[i].tick : tmax;
    }
    const i32 label_w = c.px(96);
    const i32 x0 = inner.x + c.px(6), x1 = inner.right() - label_w;
    const double tspan = tmax > tmin ? static_cast<double>(tmax - tmin) : 1.0;
    auto px_of = [&](const PhyloNode& p) {
        return x0 + static_cast<i32>((static_cast<double>(p.tick) - tmin) / tspan * (x1 - x0));
    };
    auto py_of = [&](const PhyloNode& p) {
        return inner.y + static_cast<i32>(p.row) * rh + rh / 2 - phylo_scroll_;
    };
    c.clip(inner);
    for (usize i = 0; i < phylo_.size(); ++i) {
        const PhyloNode& p = phylo_[i];
        if (p.parent == kNone)
            continue;
        const PhyloNode& q = phylo_[p.parent];
        const i32 px = px_of(q), py = py_of(q), cx = px_of(p), cy = py_of(p);
        if ((py < inner.y && cy < inner.y) || (py > inner.bottom() && cy > inner.bottom()))
            continue;
        c.line(px, py, px, cy, kFaint);
        c.line(px, cy, cx, cy, kFaint);
    }
    const u32 col = lineage_color(sel_lineage_);
    for (usize i = 0; i < phylo_.size(); ++i) {
        const PhyloNode& p = phylo_[i];
        const i32 x = px_of(p), y = py_of(p);
        if (y < inner.y - rh || y > inner.bottom() + rh)
            continue;
        const GenomeEntry& e = saves_[p.entry];
        const bool sel = sel_save_ < saves_.size() && saves_[sel_save_].genotype == e.genotype;
        const Rect row{inner.x, y - rh / 2, inner.w, rh};
        if (row.contains(in.mx, in.my)) {
            c.fill(row, kRowHot);
            if (in.pressed) {
                select_save(p.entry);
                auto_ = false;
            }
        }
        c.circle(x, y, c.px(3), col, sel ? 0xFFFFFF : mix_color(col, 0, 0.4), true);
        if (sel)
            c.circle(x, y, c.px(5), 0, 0xFFFFFF, false);
        c.text(x + c.px(7), y - c.line_h(kFontSmall) / 2, e.name, kFontSmall, sel ? 0xFFFFFF : kText);
    }
    c.unclip();
}

// --- new kinds of life (A2) ----------------------------------------------------------

// Each window's most surprising new genotypes in lineages that met the gate
// by its end, newest window first (live, the open window's leaders so far on
// top). A click selects one in the genome browser (its lineage's phylogeny
// with it); a click on its lineage also filters the browser to that lineage.
void Viewer::kinds_panel(const UiInput& in, Canvas& c, const Rect& inner) {
    char buf[256], a[32], b[32];
    const usize n = pick_count();
    if (!novelty_ || n == 0) {
        const char* why =
            !novelty_ ? (cur_.archive ? "no list for this run: nova analyse novelty <run> writes novelty_top.csv "
                                        "(its default folder, analysis\\<run>, is looked in too)"
                                      : "the live novelty meter is off (novelty.enabled, which needs detection)")
                      : "no successful new genotype yet: each window lists its most surprising new genotypes in "
                        "lineages that have met the gate";
        c.text(inner.x, inner.y, why, kFontSmall, kDim, kAlignLeft, inner.w);
        return;
    }
    const i32 sh = c.line_h(kFontSmall) + c.px(3);
    const i32 cols[7] = {0, c.px(22), c.px(82), c.px(146), c.px(214), c.px(290), c.px(332)};
    const char* heads[7] = {"#", "bits/codon", "name", "lineage", "tick", "node", "len"};
    for (u32 k = 0; k < 7; ++k)
        c.text(inner.x + cols[k], inner.y, heads[k], kFontSmall, kDim);
    const Rect area{inner.x, inner.y + sh, inner.w, inner.h - sh};
    Rect rows, track; // the list and its scrollbar
    scroll_split(c, area, &rows, &track);
    // Rows: a header for each window, then its picks.
    u32 lines = 0;
    for (usize i = 0; i < n; ++i)
        lines += 1 + (i == 0 || pick(i).window_first != pick(i - 1).window_first || pick(i).open != pick(i - 1).open);
    const i32 visible = rows.h / sh;
    kinds_bar_.update(in, c, area, track, static_cast<i32>(lines), visible, 3, &kinds_scroll_);
    const GenomeEntry* sel = sel_save_ < saves_.size() ? &saves_[sel_save_] : nullptr;
    c.clip(rows);
    i32 line = 0;
    usize clicked = ~static_cast<usize>(0);
    bool lineage_cell = false;
    pick_sy_.resize(n);
    pick_sy_.fill(-1);
    pick_sx_ = rows.x + cols[2] + c.px(4);
    for (usize i = 0; i < n; ++i) {
        const NoveltyPick& p = pick(i);
        if (i == 0 || p.window_first != pick(i - 1).window_first || p.open != pick(i - 1).open) {
            const i32 y = rows.y + (line - kinds_scroll_) * sh;
            if (y >= rows.y - sh && y < rows.bottom()) {
                commas(p.window_first, a);
                commas(p.window_last, b);
                snprintf(buf, sizeof buf, "ticks %s - %s%s", a, b, p.open ? "   (open: the leaders so far)" : "");
                c.fill(Rect{rows.x, y + c.px(1), rows.w, sh - c.px(2)}, kHead);
                c.text(rows.x + c.px(4), y + c.px(1), buf, kFontSmall, p.open ? kWarn : kText);
            }
            ++line;
        }
        const i32 y = rows.y + (line - kinds_scroll_) * sh;
        ++line;
        if (y < rows.y - sh || y >= rows.bottom())
            continue;
        if (y >= rows.y && y + sh <= rows.bottom())
            pick_sy_[i] = y + sh / 2;
        const Rect rr{rows.x, y, rows.w, sh};
        const bool is_sel = sel && (p.genotype ? sel->genotype == p.genotype
                                               : sel->node == p.node && strcmp(sel->name, p.name) == 0);
        if (is_sel)
            c.fill(rr, kRowSel);
        else if (rr.contains(in.mx, in.my))
            c.fill(rr, kRowHot);
        if (rr.contains(in.mx, in.my)) {
            cursor_ = 3;
            if (in.pressed) {
                clicked = i;
                lineage_cell = in.mx >= rows.x + cols[3] && in.mx < rows.x + cols[4];
            }
        }
        char cells[7][32];
        snprintf(cells[0], 32, "%u", p.rank);
        snprintf(cells[1], 32, "%.3f", p.surprise);
        snprintf(cells[2], 32, "%s", p.name);
        lineage_text(p.lineage, cells[3], 32);
        commas(p.tick, cells[4]);
        snprintf(cells[5], 32, "%u", p.node);
        snprintf(cells[6], 32, "%u", p.len);
        for (u32 k = 0; k < 7; ++k)
            c.text(rr.x + cols[k], rr.y + c.px(1), cells[k], kFontSmall,
                   k == 3 ? lineage_color(p.lineage) : (k == 1 && p.surprise >= 3.0 ? kWarn : kText), kAlignLeft,
                   k + 1 < 7 ? cols[k + 1] - cols[k] - c.px(2) : 0);
    }
    c.unclip();
    if (clicked != ~static_cast<usize>(0)) {
        const u64 lin = pick(clicked).lineage;
        select_pick(clicked);
        if (lineage_cell && lin) {
            char f[32];
            snprintf(f, sizeof f, "%u.%u", static_cast<unsigned>(lin >> 32), static_cast<unsigned>(lin & 0xFFFFFFFFu));
            set_filter(f);
        }
    }
}

// --- the map of life (A5) -------------------------------------------------------------

void Viewer::life_panel(const UiInput& in_raw, Canvas& c) {
    UiInput in = in_raw;
    char title[256], buf[512], a[32], b[32];
    if (!life_built_ && (have_ || !saves_.empty()))
        life_build();
    const usize np = life_.points().size();
    commas(np, a);
    if (life_.fitted())
        snprintf(title, sizeof title, "Map of life: %s genotypes", a);
    else
        snprintf(title, sizeof title, "Map of life");
    Rect inner;
    panel_box(c, map_, title, &inner);
    map_inner_ = inner;
    const i32 hh = c.line_h(kFontBold) + c.px(6);
    if (map_header_toggles(in, c, 0))
        return;
    // Header: refit (live), and the window's width.
    i32 x = map_.x + c.text_w(title, kFontBold) + c.px(24);
    if (!life_archive_) {
        const Rect rf{x, map_.y + c.px(2), c.text_w("refit on the newest", kFontUi) + c.px(16), hh - c.px(4)};
        if (button(in, c, rf, "refit on the newest"))
            life_refit();
        x = rf.right() + c.px(6);
    }
    { // the colour: lineage or role (A3's, where a profile sampled the genotype)
        const char* cl = life_by_role_ ? "colour: role" : "colour: lineage";
        const Rect rc{x, map_.y + c.px(2), c.text_w("colour: lineage", kFontUi) + c.px(16), hh - c.px(4)};
        if (button(in, c, rc, cl, life_by_role_))
            life_by_role_ = !life_by_role_;
        x = rc.right() + c.px(6);
    }
    const Rect wm{x, map_.y + c.px(2), c.px(24), hh - c.px(4)};
    const Rect wp{x + c.px(26) + c.px(110), map_.y + c.px(2), c.px(24), hh - c.px(4)};
    const u64 t_lo = life_.first_tick();
    const u64 t_hi = life_.last_tick() > cur_.tick ? life_.last_tick() : cur_.tick;
    const u64 range = t_hi > t_lo ? t_hi - t_lo : 1;
    u64 span = life_span_ ? life_span_ : (range / 12 > 500 ? range / 12 : 500);
    if (button(in, c, wm, "-"))
        life_span_ = span = span / 2 > 10 ? span / 2 : 10;
    if (button(in, c, wp, "+"))
        life_span_ = span = span * 2;
    commas(span, a);
    snprintf(buf, sizeof buf, "window %s ticks", a);
    c.text(x + c.px(26) + c.px(55), map_.y + c.px(4), buf, kFontSmall, kText, kAlignCenter);
    life_span_now_ = span;

    life_sx_.resize(np);
    life_sy_.resize(np);
    for (usize i = 0; i < np; ++i)
        life_sx_[i] = life_sy_[i] = 0x7FFFFFFF;
    if (np == 0 || !life_.fitted()) {
        c.text(inner.x, inner.y,
               np == 0 ? "no genotype saved yet (each genebank save becomes a point)"
                       : "waiting for 8 genotypes to fit the first axes on",
               kFontUi, kDim);
        return;
    }
    // The time: the frame's tick when following, else the slider's.
    const i32 bar_h = c.px(26);
    life_slider_ = Rect{inner.x + c.px(64), inner.bottom() - bar_h + c.px(9), inner.w - c.px(64) - c.px(200), c.px(8)};
    const Rect hit{life_slider_.x, inner.bottom() - bar_h, life_slider_.w, bar_h};
    if (in.pressed && hit.contains(in.mx, in.my))
        drag_life_ = true;
    if (!in.down)
        drag_life_ = false;
    if (drag_life_) {
        double f = static_cast<double>(in.mx - life_slider_.x) / (life_slider_.w > 0 ? life_slider_.w : 1);
        f = f < 0 ? 0 : (f > 1 ? 1 : f);
        life_t_ = t_lo + static_cast<u64>(f * static_cast<double>(range) + 0.5);
        life_follow_ = f >= 0.999 && !cur_.archive;
        in.pressed = false;
    }
    if (life_follow_)
        life_t_ = cur_.archive ? cur_.tick : t_hi;
    const u64 T = life_t_ < t_lo ? t_lo : life_t_;
    const Rect follow{inner.x, inner.bottom() - bar_h + c.px(2), c.px(58), bar_h - c.px(6)};
    if (button(in, c, follow, cur_.archive ? "window" : "now", life_follow_))
        life_follow_ = !life_follow_;
    c.fill(life_slider_, kButton);
    {
        const i32 hx = life_slider_.x + static_cast<i32>(static_cast<double>(T - t_lo) / range * life_slider_.w);
        const i32 wx = life_slider_.x + static_cast<i32>(static_cast<double>(span) / range * life_slider_.w);
        c.fill(Rect{hx - (wx - life_slider_.x), life_slider_.y + c.px(2), wx - life_slider_.x, c.px(4)}, kFaint);
        c.fill(Rect{hx - c.px(3), life_slider_.y - c.px(5), c.px(6), c.px(18)}, kAccent);
    }
    commas(T > span ? T - span : 0, a);
    commas(T, b);
    snprintf(buf, sizeof buf, "ticks %s - %s", a, b);
    c.text(life_slider_.right() + c.px(10), life_slider_.y - c.px(4), buf, kFontSmall, kText);

    // The plot: the fitted box, equal scales on both axes.
    const i32 info_h = c.line_h(kFontSmall) + c.px(4);
    life_plot_ = Rect{inner.x, inner.y + info_h, inner.w, inner.h - info_h - bar_h - c.px(4)};
    const float* bx = life_.box();
    const double bw = bx[2] - bx[0] > 1e-9 ? bx[2] - bx[0] : 1, bh = bx[3] - bx[1] > 1e-9 ? bx[3] - bx[1] : 1;
    const double s = 0.96 * (life_plot_.w / bw < life_plot_.h / bh ? life_plot_.w / bw : life_plot_.h / bh);
    life_scale_ = static_cast<float>(s);
    life_cx_ = static_cast<float>((bx[0] + bx[2]) * 0.5);
    life_cy_ = static_cast<float>((bx[1] + bx[3]) * 0.5);
    const double pcx = life_plot_.x + life_plot_.w * 0.5, pcy = life_plot_.y + life_plot_.h * 0.5;
    auto sx_of = [&](double v) { return static_cast<i32>(floor(pcx + (v - life_cx_) * s + 0.5)); };
    auto sy_of = [&](double v) { return static_cast<i32>(floor(pcy - (v - life_cy_) * s + 0.5)); }; // up is +PC2
    c.fill(life_plot_, 0x15181E);
    c.clip(life_plot_);
    // The axes through the reference's centre (0, 0).
    c.dashed(sx_of(0), life_plot_.y, sx_of(0), life_plot_.bottom(), kFaint, c.px(4));
    c.dashed(life_plot_.x, sy_of(0), life_plot_.right(), sy_of(0), kFaint, c.px(4));
    // The points shown: saved within the window. Older fade; living members
    // size them when known (live, at now), else saves.
    const bool pops = pop_known_ && !cur_.archive && life_follow_;
    const u64 sel_geno = sel_save_ < saves_.size() ? saves_[sel_save_].genotype : 0;
    u32 shown = 0, hover = kNone;
    i32 best = c.px(7) * c.px(7);
    const LifePoint* P = life_.points().data();
    for (usize i = 0; i < np; ++i) {
        const LifePoint& p = P[i];
        if (p.first > T || p.last + span < T)
            continue;
        double age = static_cast<double>(T - (p.last < T ? p.last : T)) / static_cast<double>(span);
        age = age > 1 ? 1 : age;
        double size = p.saves;
        if (pops) {
            usize lo = 0, hi = pop_genotype_.size();
            while (lo < hi) {
                const usize mid = (lo + hi) / 2;
                if (pop_genotype_[mid] < p.genotype)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            size = lo < pop_genotype_.size() && pop_genotype_[lo] == p.genotype ? pop_count_[lo] : 0;
        }
        double r = c.px(1.5f) + c.px(1.2f) * sqrt(size);
        r = r > c.px(9) ? c.px(9) : r;
        i32 sx = sx_of(p.x), sy = sy_of(p.y);
        const bool outside = sx < life_plot_.x || sy < life_plot_.y || sx >= life_plot_.right() || sy >= life_plot_.bottom();
        if (outside) { // pinned to the border, hollow
            sx = sx < life_plot_.x + 2 ? life_plot_.x + 2 : (sx >= life_plot_.right() - 2 ? life_plot_.right() - 3 : sx);
            sy = sy < life_plot_.y + 2 ? life_plot_.y + 2 : (sy >= life_plot_.bottom() - 2 ? life_plot_.bottom() - 3 : sy);
        }
        life_sx_[i] = sx;
        life_sy_[i] = sy;
        const u32 base = life_by_role_ ? role_color(genotype_role(p.genotype)) : lineage_color(p.lineage);
        const u32 col = mix_color(base, 0x15181E, 0.75 * age);
        const i32 ri = static_cast<i32>(r + 0.5);
        if (ri <= 1)
            c.fill(Rect{sx - 1, sy - 1, 2, 2}, col);
        else
            c.circle(sx, sy, ri, col, outside ? col : mix_color(col, 0, 0.35), !outside);
        if (p.lineage == sel_lineage_ && sel_lineage_)
            c.circle(sx, sy, ri + c.px(1), 0, mix_color(0xFFFFFF, 0x15181E, 0.5 + 0.4 * age), false);
        if (p.genotype == sel_geno && sel_geno)
            c.circle(sx, sy, ri + c.px(4), 0, 0xFFFFFF, false);
        const i32 dx = in.mx - sx, dy = in.my - sy;
        if (life_plot_.contains(in.mx, in.my) && dx * dx + dy * dy < best) {
            best = dx * dx + dy * dy;
            hover = static_cast<u32>(i);
        }
        ++shown;
    }
    // The roles' legend, top left in the plot (by role).
    if (life_by_role_) {
        i32 lx = life_plot_.x + c.px(6);
        const i32 ly = life_plot_.y + c.px(4);
        for (u32 r = 0; r <= kRoleCount; ++r) {
            const char* nm = r < kRoleCount ? role_name(r) : "not profiled";
            c.text(lx, ly, nm, kFontSmall, role_color(r));
            lx += c.text_w(nm, kFontSmall) + c.px(10);
        }
    }
    c.unclip();
    // Info: the axes and what's shown.
    commas(life_.ref_first(), a);
    commas(life_.ref_last(), b);
    char colour[64];
    if (life_by_role_)
        snprintf(colour, sizeof colour, "role (%llu genotypes profiled)", static_cast<unsigned long long>(roles_.size()));
    else
        snprintf(colour, sizeof colour, "lineage");
    snprintf(buf, sizeof buf,
             "%u shown of %u   axes %s on %u genotypes (ticks %s - %s): PC1 %.0f%%, PC2 %.0f%% of their variance   "
             "colour: %s   size: %s",
             shown, static_cast<unsigned>(np), life_.final() ? "fitted" : "provisional, fitted", life_.reference(), a, b,
             100.0 * life_.variance_share(0), 100.0 * life_.variance_share(1), colour,
             pops ? "living members" : "genebank saves");
    c.text(inner.x, inner.y, buf, kFontSmall, kDim, kAlignLeft, inner.w);
    if (hover != kNone) {
        const LifePoint& p = P[hover];
        char lin[32], t0[32], t1[32];
        lineage_text(p.lineage, lin, sizeof lin);
        commas(p.first, t0);
        commas(p.last, t1);
        const u32 role = genotype_role(p.genotype);
        snprintf(buf, sizeof buf,
                 "%s  (lineage %s, %u codons)\nfirst saved at tick %s on node %u, last at %s\n%u genebank saves\n"
                 "role: %s\nPC1 %.3f, PC2 %.3f\nclick: select it in the genome browser",
                 p.name, lin, p.len, t0, p.node, t1, p.saves, role < kRoleCount ? role_name(role) : "not profiled",
                 p.x, p.y);
        set_tooltip(buf);
        cursor_ = 3;
        if (in.pressed) {
            sel_lineage_ = p.lineage;
            for (usize k = saves_.size(); k-- > 0;)
                if (saves_[k].genotype == p.genotype) {
                    select_save(k);
                    break;
                }
            auto_ = false;
        }
    }
}

// --- the inspector ---------------------------------------------------------------

void Viewer::inspector(const UiInput& in, Canvas& c) {
    char buf[512], a[32], b[32];
    Rect inner;
    if (sel_node_ < cur_.node.size())
        snprintf(buf, sizeof buf, "Inspector: node %u", sel_node_);
    else
        snprintf(buf, sizeof buf, "Inspector");
    panel_box(c, insp_, buf, &inner);
    if (sel_node_ >= cur_.node.size()) {
        c.text(inner.x, inner.y, "click a node on the map", kFontUi, kDim);
        return;
    }
    const u32 node_now = sel_node_; // a click on a neighbour changes sel_node_ mid-draw
    const NodeView& v = cur_.node[node_now];
    const i32 lh = c.line_h(kFontUi);
    i32 y = inner.y;
    c.clip(inner);
    if (!v.live) {
        snprintf(buf, sizeof buf, "a fallow site: no node here now (lives so far: %u)", v.life);
        c.text(inner.x, y, buf, kFontUi, kDim);
        c.unclip();
        return;
    }
    lineage_text(v.top_lineage, a, sizeof a);
    snprintf(buf, sizeof buf, "%u bodies, %u CPUs   %u lineages (largest %s, %u members), %u colonist members",
             v.bodies, v.cpus, v.lineages, a, v.top_alive, v.colonists);
    c.text(inner.x, y, buf, kFontUi, kText, kAlignLeft, inner.w);
    y += lh;
    commas(v.soup_size, a);
    commas(v.millicores, b);
    snprintf(buf, sizeof buf, "memory %s (waste %.0f%%)   millicores %s (free %.0f%%)   GPU %u  NPU %u  disk %u   degree %u",
             a, 100.0 * node_metric(v, kColorWaste), b, 100.0 * node_metric(v, kColorFreeCpu), v.gpus, v.npus,
             v.disk, v.degree);
    c.text(inner.x, y, buf, kFontUi, kText, kAlignLeft, inner.w);
    y += lh;
    snprintf(buf, sizeof buf, "health %u, life %u   climate %.3g   sparks %.3g/tick   radiation %.3gx   births %.3g/tick",
             v.health, v.life, v.cool, v.spark_rate, v.radiation, v.births);
    c.text(inner.x, y, buf, kFontUi, kDim, kAlignLeft, inner.w);
    y += lh;
    // Its neighbours (a click on one selects it).
    {
        i32 nx = inner.x;
        c.text(nx, y, "neighbours:", kFontSmall, kDim);
        nx += c.text_w("neighbours:", kFontSmall) + c.px(6);
        u32 shown = 0;
        for (usize i = 0; i < cur_.edge.size(); ++i) {
            const EdgeView& e = cur_.edge[i];
            if (e.a != node_now && e.b != node_now)
                continue;
            const u32 other = e.a == node_now ? e.b : e.a;
            snprintf(a, sizeof a, "%u%s", other, e.down ? " (down)" : (e.long_link ? " (long)" : ""));
            const i32 tw = c.text_w(a, kFontSmall);
            if (nx + tw > inner.right())
                break;
            const Rect hit{nx, y, tw, c.line_h(kFontSmall)};
            const bool hot = hit.contains(in.mx, in.my);
            c.text(nx, y, a, kFontSmall, hot ? 0xFFFFFF : kAccent);
            if (hot) {
                cursor_ = 3;
                if (in.pressed && other < cur_.node.size()) {
                    select_node(other);
                    auto_ = false;
                }
            }
            nx += tw + c.px(10);
            ++shown;
        }
        if (!shown)
            c.text(nx, y, "none", kFontSmall, kDim);
        y += c.line_h(kFontSmall) + c.px(4);
    }

    if (cur_.archive || cur_.sel_node != sel_node_ || cur_.strip_w == 0) {
        c.text(inner.x, y, cur_.archive ? "(a finished run: memory and bodies need a live run or a snapshot)"
                                        : "(loading...)",
               kFontSmall, kDim);
        c.unclip();
        return;
    }
    // The memory strip.
    snprintf(buf, sizeof buf, "memory: %s  v", strip_by_name(strip_by_));
    const Rect sb{inner.x, y, c.text_w(buf, kFontUi) + c.px(16), lh + c.px(4)};
    if (button(in, c, sb, buf, popup_ == 2))
        open_popup(2, 0, sb);
    c.text(sb.right() + c.px(8), y + c.px(2), "one pixel per codon; click a body to inspect it", kFontSmall, kDim);
    const u32 fast = cur_.fast_end < cur_.strip_owner.size() ? cur_.fast_end : static_cast<u32>(cur_.strip_owner.size());
    if (fast) { // W3: the fast-memory region, outlined
        commas(fast - 1, a);
        snprintf(buf, sizeof buf, "fast memory: codons 0 - %s (outlined)", a);
        c.text(inner.right(), y + c.px(2), buf, kFontSmall, kFastMem, kAlignRight);
    }
    y = sb.bottom() + c.px(4);
    i32 sh = static_cast<i32>(static_cast<double>(cur_.strip_h) * inner.w / cur_.strip_w);
    const i32 cap = inner.h / 3;
    sh = sh > cap ? cap : sh;
    const Rect strip{inner.x, y, inner.w, sh};
    c.image(strip, cur_.strip.data(), static_cast<i32>(cur_.strip_w), static_cast<i32>(cur_.strip_h));
    if (fast) {
        // The boundary as a staircase: under the last fast row's part, up its
        // edge, then along the top of the slow rows.
        const double rh = static_cast<double>(strip.h) / cur_.strip_h, cw = static_cast<double>(strip.w) / cur_.strip_w;
        const u32 full = fast / cur_.strip_w, part = fast % cur_.strip_w;
        const i32 y0 = strip.y + static_cast<i32>(full * rh + 0.5), y1 = strip.y + static_cast<i32>((full + 1) * rh + 0.5);
        const i32 xp = strip.x + static_cast<i32>(part * cw + 0.5);
        const i32 w2 = c.px(2);
        if (part) {
            c.fill(Rect{strip.x, y1 - w2 / 2, xp - strip.x, w2}, kFastMem);
            c.fill(Rect{xp - w2 / 2, y0, w2, y1 - y0}, kFastMem);
            c.fill(Rect{xp, y0 - w2 / 2, strip.right() - xp, w2}, kFastMem);
        } else {
            c.fill(Rect{strip.x, y0 - w2 / 2, strip.w, w2}, kFastMem);
        }
        c.fill(Rect{strip.x, strip.y, w2, (part ? y1 : y0) - strip.y}, kFastMem); // and its left edge
    }
    if (strip.contains(in.mx, in.my)) {
        const u32 sx = static_cast<u32>(static_cast<double>(in.mx - strip.x) / strip.w * cur_.strip_w);
        const u32 sy = static_cast<u32>(static_cast<double>(in.my - strip.y) / strip.h * cur_.strip_h);
        const usize at = static_cast<usize>(sy) * cur_.strip_w + sx;
        if (at < cur_.strip_owner.size()) {
            const u32 slot = cur_.strip_owner[at];
            snprintf(buf, sizeof buf, "codon %u%s: %s", static_cast<unsigned>(at),
                     fast ? (at < fast ? " (fast memory)" : " (slow memory)") : "",
                     slot == kNone ? "waste" : "owned (click to inspect its body)");
            set_tooltip(buf);
            if (in.pressed && slot != kNone) {
                for (usize i = 0; i < cur_.bodies.size(); ++i) {
                    if (cur_.bodies[i].slot == slot) {
                        sel_body_ = slot;
                        sel_serial_ = cur_.bodies[i].serial;
                        if (cur_.bodies[i].lineage)
                            sel_lineage_ = cur_.bodies[i].lineage;
                        genome_scroll_ = 0;
                        auto_ = false;
                    }
                }
            }
        }
    }
    y = strip.bottom() + c.px(6);

    // Bodies (left) and the selected body (right).
    const i32 sh2 = c.line_h(kFontSmall) + c.px(2);
    const Rect list{inner.x, y, inner.w * 2 / 5, inner.bottom() - y};
    const Rect det{list.right() + c.px(8), y, inner.right() - list.right() - c.px(8), inner.bottom() - y};
    const i32 cols[6] = {0, c.px(58), c.px(92), c.px(118), c.px(156), c.px(196)};
    const char* heads[6] = {"body", "len", "cpu", "mcores", "mem", "lineage"};
    for (u32 k = 0; k < 6; ++k)
        c.text(list.x + cols[k], y, heads[k], kFontSmall, kDim);
    const Rect area{list.x, y + sh2, list.w, list.h - sh2};
    Rect rows, track; // the list and its scrollbar
    scroll_split(c, area, &rows, &track);
    const i32 visible = rows.h / sh2;
    const i32 count = static_cast<i32>(cur_.bodies.size());
    body_bar_.update(in, c, area, track, count, visible, 3, &body_scroll_);
    c.clip(rows);
    for (i32 r = 0; r < visible + 1 && body_scroll_ + r < count; ++r) {
        const BodyRow& br = cur_.bodies[static_cast<usize>(body_scroll_ + r)];
        const Rect rr{rows.x, rows.y + r * sh2, rows.w, sh2};
        const bool sel = br.slot == sel_body_ && br.serial == sel_serial_;
        if (sel)
            c.fill(rr, kRowSel);
        else if (rr.contains(in.mx, in.my))
            c.fill(rr, kRowHot);
        if (in.pressed && rr.contains(in.mx, in.my)) {
            sel_body_ = br.slot;
            sel_serial_ = br.serial;
            if (br.lineage)
                sel_lineage_ = br.lineage;
            genome_scroll_ = 0;
            auto_ = false;
        }
        char cells[6][32];
        snprintf(cells[0], 32, "#%llu", static_cast<unsigned long long>(br.serial));
        snprintf(cells[1], 32, "%u", br.len);
        snprintf(cells[2], 32, "%u", br.ncpu);
        snprintf(cells[3], 32, "%u", br.units);
        snprintf(cells[4], 32, "%u", br.mem);
        lineage_text(br.lineage, cells[5], 32);
        for (u32 k = 0; k < 6; ++k)
            c.text(rr.x + cols[k], rr.y, cells[k], kFontSmall, k == 5 && br.lineage ? lineage_color(br.lineage) : kText,
                   kAlignLeft, k + 1 < 6 ? cols[k + 1] - cols[k] - c.px(2) : 0);
    }
    c.clip(inner);

    if (!cur_.body_ok || cur_.sel_body != sel_body_) {
        c.text(det.x, det.y, "select a body: click it in the list or the memory strip", kFontSmall, kDim, kAlignLeft,
               det.w);
        c.unclip();
        return;
    }
    const BodyRow& B = cur_.body;
    i32 dy = det.y;
    lineage_text(B.lineage, a, sizeof a);
    snprintf(buf, sizeof buf, "body #%llu (slot %u)   genome %u codons at %u", static_cast<unsigned long long>(B.serial),
             B.slot, B.len, B.start);
    c.text(det.x, dy, buf, kFontBold, kText, kAlignLeft, det.w);
    dy += c.line_h(kFontBold);
    if (B.lineage)
        snprintf(buf, sizeof buf, "lineage %s, depth %u, genotype %016llx", a, B.depth,
                 static_cast<unsigned long long>(B.genotype));
    else
        snprintf(buf, sizeof buf, "not a member of any lineage");
    c.text(det.x, dy, buf, kFontSmall, B.lineage ? lineage_color(B.lineage) : kDim, kAlignLeft, det.w);
    dy += sh2;
    if (cur_.kinds) // Phase 9c: its cores by kind, and its threads
        snprintf(buf, sizeof buf,
                 "holds %.2f P-cores and %.2f E-cores, %u codons, %u GPU, %u NPU, %u disk; %u threads, %u warps; born on a "
                 "%s",
                 (B.units - B.eunits) / 1000.0, B.eunits / 1000.0, B.mem, B.gpus, B.npus, B.disk, B.ncpu, B.warps,
                 B.form == kFormGpu ? "GPU" : "CPU");
    else
        snprintf(buf, sizeof buf,
                 "holds %u millicores, %u codons, %u GPU, %u NPU, %u disk; %u CPUs, %u warps; born on a %s", B.units,
                 B.mem, B.gpus, B.npus, B.disk, B.ncpu, B.warps, B.form == kFormGpu ? "GPU" : "CPU");
    c.text(det.x, dy, buf, kFontSmall, kText, kAlignLeft, det.w);
    dy += sh2 + c.px(2);
    // Phase 9: an action under way, by it or on it.
    if (B.doing_op || B.threat_op) {
        char d1[64] = "", d2[64] = "";
        if (B.doing_op)
            snprintf(d1, sizeof d1, "doing %s, %u ticks left", op_name(static_cast<Op>(B.doing_op)), B.doing_left);
        if (B.threat_op)
            snprintf(d2, sizeof d2, "%s%s aimed at it, %u ticks left", B.doing_op ? "; " : "",
                     op_name(static_cast<Op>(B.threat_op)), B.threat_left);
        snprintf(buf, sizeof buf, "%s%s", d1, d2);
        c.text(det.x, dy, buf, kFontSmall, B.threat_op ? kBad : kText, kAlignLeft, det.w);
        dy += sh2;
    }
    // Phase 8: its warps' lanes, a few per warp; * marks the lanes of the
    // warp's next step (those at the lowest IP).
    if (cur_.lanes.size()) {
        c.text(det.x, dy, "warp.lane   ip    rh    wh    ax    bx    cx  sp errs state     steps", kFontMono, kDim,
               kAlignLeft, det.w);
        dy += c.line_h(kFontMono);
        u32 shown = 0, last_warp = kNone, in_warp = 0;
        for (usize i = 0; i < cur_.lanes.size() && shown < 8; ++i) {
            const LaneView& lv = cur_.lanes[i];
            in_warp = lv.warp == last_warp ? in_warp + 1 : 0;
            last_warp = lv.warp;
            if (in_warp >= 4)
                continue; // the first four lanes of each warp
            const char* state = lv.halted ? "halted" : (lv.yielded ? "yielded" : (lv.wait ? "host" : "running"));
            snprintf(buf, sizeof buf, "%c%2u.%-3u %5u %5u %5u %5u %5u %5u %3u %4u %-8s %8llu", lv.next ? '*' : ' ',
                     lv.warp, lv.lane, lv.ip, lv.rh, lv.wh, lv.ax, lv.bx, lv.cx, lv.sp, lv.errs, state,
                     static_cast<unsigned long long>(lv.steps));
            c.text(det.x, dy, buf, kFontMono, lv.next ? 0xA0E0FF : kText, kAlignLeft, det.w);
            dy += c.line_h(kFontMono);
            ++shown;
        }
        dy += c.px(2);
    }
    // CPUs: a compact table. Phase 9c: with threads or E millicores, each
    // thread's preferred kind and its share of a core last tick (E: on E-cores).
    c.text(det.x, dy,
           cur_.kinds ? "cpu   ip    rh    wh    ax    bx    cx    dx  sp errs state       executed kind core   E"
                      : "cpu   ip    rh    wh    ax    bx    cx    dx  sp errs state       executed",
           kFontMono, kDim, kAlignLeft, det.w);
    dy += c.line_h(kFontMono);
    for (usize i = 0; i < cur_.cpus.size() && i < 6; ++i) {
        const CpuView& cv = cur_.cpus[i];
        const char* state = cv.halted ? "halted" : (cv.yielded ? "yielded" : "running");
        snprintf(buf, sizeof buf, "%3u %5u %5u %5u %5u %5u %5u %5u %3u %4u %-8s %10llu", static_cast<unsigned>(i),
                 cv.ip, cv.rh, cv.wh, cv.ax, cv.bx, cv.cx, cv.dx, cv.sp, cv.errs, state,
                 static_cast<unsigned long long>(cv.executed));
        if (cur_.kinds) {
            char k[40];
            snprintf(k, sizeof k, "    %c %4.0f%% %3.0f%%", cv.kind ? 'E' : 'P', cv.share * 100.0, cv.eshare * 100.0);
            strncat(buf, k, sizeof buf - strlen(buf) - 1);
        }
        c.text(det.x, dy, buf, kFontMono, kText, kAlignLeft, det.w);
        dy += c.line_h(kFontMono);
    }
    if (cur_.cpus.size() > 6) {
        snprintf(buf, sizeof buf, "... and %u more", static_cast<unsigned>(cur_.cpus.size() - 6));
        c.text(det.x, dy, buf, kFontSmall, kDim);
        dy += sh2;
    }
    dy += c.px(4);
    // The genome, wrapped.
    const Rect garea{det.x, dy, det.w, det.bottom() - dy};
    Rect gr, gtrack; // the text and its scrollbar (in lines)
    scroll_split(c, garea, &gr, &gtrack);
    const i32 mh = c.line_h(kFontMono);
    i32 lines = 0;
    wrap_mono(c, cur_.genome.data(), cur_.genome.size(), gr.w, [&](usize, usize) { ++lines; });
    genome_bar_.update(in, c, garea, gtrack, lines, gr.h > 0 && mh > 0 ? gr.h / mh : 0, 2, &genome_scroll_);
    c.clip(gr);
    i32 line = 0;
    wrap_mono(c, cur_.genome.data(), cur_.genome.size(), gr.w, [&](usize s, usize len) {
        const i32 ly = gr.y + (line - genome_scroll_) * mh;
        if (ly >= gr.y - mh && ly < gr.bottom())
            c.text(gr.x, ly, cur_.genome.data() + s, len, kFontMono, 0xC8D0A0);
        ++line;
    });
    c.unclip();
}

// --- the genome browser ----------------------------------------------------------

void Viewer::set_filter(const char* text) {
    snprintf(filt_, sizeof filt_, "%s", text);
    apply_filter();
}

void Viewer::apply_filter() {
    len_lo_ = 0;
    len_hi_ = 0xFFFFFFFFu;
    filt_lineage_ = 0;
    const Str s = Str(filt_).trim();
    const usize dot = s.find('.'), dash = s.find('-');
    u64 a = 0, b = 0;
    if (dot < s.n) {
        if (parse_u64(s.sub(0, dot), &a) && parse_u64(s.sub(dot + 1, s.n - dot - 1), &b))
            filt_lineage_ = (a << 32) | (b & 0xFFFFFFFFull);
    } else if (dash < s.n) {
        if (parse_u64(s.sub(0, dash).trim(), &a))
            len_lo_ = static_cast<u32>(a);
        if (parse_u64(s.sub(dash + 1, s.n - dash - 1).trim(), &b))
            len_hi_ = static_cast<u32>(b);
    } else if (s.n && parse_u64(s, &a)) {
        len_lo_ = len_hi_ = static_cast<u32>(a);
    }
    order_dirty_ = true;
    browse_scroll_ = 0;
    sort_saves();
}

void Viewer::sort_saves() {
    order_.clear();
    for (usize i = 0; i < saves_.size(); ++i) {
        const GenomeEntry& e = saves_[i];
        if (lineage_only_ && sel_lineage_ && e.lineage != sel_lineage_)
            continue;
        if (filt_lineage_ && e.lineage != filt_lineage_)
            continue;
        if (e.len < len_lo_ || e.len > len_hi_)
            continue;
        order_.push(static_cast<u32>(i));
    }
    const GenomeEntry* e = saves_.data();
    const u32 col = sort_col_;
    const bool desc = sort_desc_;
    auto key_less = [e, col](u32 a, u32 b) {
        switch (col) {
        case 0: { // name: length, then the letters
            if (e[a].len != e[b].len)
                return e[a].len < e[b].len;
            return strcmp(e[a].name, e[b].name) < 0;
        }
        case 1:
            return e[a].len < e[b].len;
        case 2:
            return e[a].lineage < e[b].lineage;
        case 3:
            return e[a].tick < e[b].tick;
        case 4:
            return e[a].node < e[b].node;
        default:
            return strcmp(e[a].parent_name, e[b].parent_name) < 0;
        }
    };
    merge_sort(order_, [&](u32 a, u32 b) { return desc ? key_less(b, a) : key_less(a, b); });
    order_dirty_ = false;
}

void Viewer::browser(const UiInput& in, Canvas& c) {
    char title[96], kept[32], seen[32];
    commas(saves_.size(), kept);
    commas(saves_seen_, seen);
    if (saves_seen_ > saves_.size())
        snprintf(title, sizeof title, "Genomes: the newest %s of %s saved", kept, seen);
    else
        snprintf(title, sizeof title, "Genomes: %s saved", kept);
    Rect inner;
    panel_box(c, browse_, title, &inner);
    const i32 hh = c.line_h(kFontBold) + c.px(6);
    const char* tl = "this lineage only";
    const Rect tr{browse_.right() - c.text_w(tl, kFontUi) - c.px(24), browse_.y + c.px(2),
                  c.text_w(tl, kFontUi) + c.px(16), hh - c.px(4)};
    if (button(in, c, tr, tl, lineage_only_)) {
        lineage_only_ = !lineage_only_;
        order_dirty_ = true;
    }
    // The filter: a length, a range a-b, or a lineage n.k (Enter applies).
    {
        const Rect fr{tr.x - c.px(176), tr.y, c.px(168), tr.h};
        const bool editing = edit_ == kEditFilter;
        c.fill(fr, editing ? 0x10141A : kBg);
        c.frame(fr, editing ? kAccent : kFaint);
        char fb[48];
        u32 col = kText;
        if (editing) {
            edit_buf_[edit_len_] = '\0';
            snprintf(fb, sizeof fb, "%s|", edit_buf_);
        } else if (filt_[0]) {
            snprintf(fb, sizeof fb, "filter: %s", filt_);
        } else {
            snprintf(fb, sizeof fb, "filter: length, a-b, n.k");
            col = kDim;
        }
        c.text(fr.x + c.px(4), fr.y + (fr.h - c.line_h(kFontSmall)) / 2, fb, kFontSmall, col, kAlignLeft,
               fr.w - c.px(6));
        if (fr.contains(in.mx, in.my)) {
            cursor_ = 3;
            if (in.pressed) {
                edit_ = kEditFilter;
                snprintf(edit_buf_, sizeof edit_buf_, "%s", filt_);
                edit_len_ = static_cast<u32>(strlen(edit_buf_));
            }
        }
    }
    if (lineage_only_ && order_lineage_ != sel_lineage_)
        order_dirty_ = true;
    order_lineage_ = sel_lineage_;
    if (order_dirty_)
        sort_saves();
    if (saves_.empty()) {
        browse_track_ = Rect{};
        c.text(inner.x, inner.y, "no genotype saved yet (a lineage member's genotype is saved once it has "
                                 "detect.genebank_threshold living copies)",
               kFontSmall, kDim, kAlignLeft, inner.w);
        return;
    }
    const i32 sh = c.line_h(kFontSmall) + c.px(2);
    const i32 cols[6] = {0, c.px(74), c.px(112), c.px(186), c.px(262), c.px(304)};
    const char* heads[6] = {"name", "len", "lineage", "tick", "node", "parent"};
    for (u32 k = 0; k < 6; ++k) {
        const Rect hr{inner.x + cols[k], inner.y, (k + 1 < 6 ? cols[k + 1] : inner.w) - cols[k], sh};
        char h[32];
        snprintf(h, sizeof h, "%s%s", heads[k], sort_col_ == k ? (sort_desc_ ? " v" : " ^") : "");
        c.text(hr.x, hr.y, h, kFontSmall, sort_col_ == k ? kText : kDim);
        if (hr.contains(in.mx, in.my)) {
            cursor_ = 3;
            if (in.pressed) {
                sort_desc_ = sort_col_ == k ? !sort_desc_ : (k == 3);
                sort_col_ = k;
                order_dirty_ = true;
            }
        }
    }
    const i32 list_h = (inner.h - sh) * 3 / 5;
    const Rect area{inner.x, inner.y + sh, inner.w, list_h};
    Rect rows, track; // the list and its scrollbar
    scroll_split(c, area, &rows, &track);
    const i32 visible = rows.h / sh;
    const i32 count = static_cast<i32>(order_.size());
    browse_bar_.update(in, c, area, track, count, visible, 3, &browse_scroll_);
    browse_track_ = scroll_thumb(track.h, c.px(kScrollThumbMin), count, visible, 0).h ? track : Rect{};
    browse_shown_ = visible;
    c.clip(rows);
    for (i32 r = 0; r < visible + 1 && browse_scroll_ + r < count; ++r) {
        const u32 idx = order_[static_cast<usize>(browse_scroll_ + r)];
        const GenomeEntry& e = saves_[idx];
        const Rect rr{rows.x, rows.y + r * sh, rows.w, sh};
        if (idx == sel_save_)
            c.fill(rr, kRowSel);
        else if (rr.contains(in.mx, in.my))
            c.fill(rr, kRowHot);
        if (in.pressed && rr.contains(in.mx, in.my)) {
            select_save(idx);
            auto_ = false;
        }
        char cells[6][32];
        snprintf(cells[0], 32, "%s", e.name);
        snprintf(cells[1], 32, "%u", e.len);
        lineage_text(e.lineage, cells[2], 32);
        commas(e.tick, cells[3]);
        snprintf(cells[4], 32, "%u", e.node);
        snprintf(cells[5], 32, "%s", e.parent_name);
        for (u32 k = 0; k < 6; ++k)
            c.text(rr.x + cols[k], rr.y, cells[k], kFontSmall, k == 2 ? lineage_color(e.lineage) : kText, kAlignLeft,
                   k + 1 < 6 ? cols[k + 1] - cols[k] - c.px(2) : 0);
    }
    c.unclip();
    // The selected genome.
    const Rect gr{inner.x, rows.bottom() + c.px(6), inner.w, inner.bottom() - rows.bottom() - c.px(6)};
    c.fill(Rect{gr.x, gr.y - c.px(3), gr.w, 1}, kFaint);
    if (sel_save_ >= saves_.size()) {
        c.text(gr.x, gr.y, "select a genome to see it, its phylogeny and where its copies live (ringed on the map)",
               kFontSmall, kDim, kAlignLeft, gr.w);
        return;
    }
    const GenomeEntry& e = saves_[sel_save_];
    char buf[256], lin[32], tk[32];
    lineage_text(e.lineage, lin, sizeof lin);
    commas(e.tick, tk);
    u32 holding = 0;
    for (usize i = 0; i < cur_.node.size(); ++i)
        holding += cur_.node[i].holding ? 1 : 0;
    snprintf(buf, sizeof buf, "%s: lineage %s, saved at tick %s on node %u, parent %s;  living copies on %u nodes",
             e.name, lin, tk, e.node, e.parent_name, holding);
    c.text(gr.x, gr.y, buf, kFontSmall, kText, kAlignLeft, gr.w);
    const Rect tr2{gr.x, gr.y + sh, gr.w, gr.h - sh};
    c.clip(tr2);
    i32 line = 0;
    const i32 mh = c.line_h(kFontMono);
    wrap_mono(c, save_text_.data() + e.off, e.n, tr2.w, [&](usize s, usize len) {
        const i32 ly = tr2.y + line * mh;
        if (ly < tr2.bottom())
            c.text(tr2.x, ly, save_text_.data() + e.off + s, len, kFontMono, 0xC8D0A0);
        ++line;
    });
    c.unclip();
}

// --- the popup list and the tooltip ------------------------------------------------

void Viewer::popup(const UiInput& in, Canvas& c, bool* consumed) {
    if (!popup_)
        return;
    // 1 colour-by, 2 strip-by, 3 a chart's series (every stats.csv column,
    // from the shared column list), 4 the substrate type (W8).
    const u32 count = popup_ == 1 ? kColorCount
                                  : (popup_ == 2 ? kStripCount : (popup_ == 4 ? kSubstrateTypes : kStatsColumns));
    const i32 ih = c.line_h(kFontUi) + c.px(4);
    const i32 w = c.px(180);
    i32 max_rows = (c.height() - popup_r_.y - c.px(8)) / ih;
    // A long list (the series: well over a hundred columns) takes the window's
    // whole height (moved up) and as many columns side by side as it needs to
    // show every item at once.
    if (static_cast<i32>(count) > max_rows)
        max_rows = (c.height() - c.px(16)) / ih;
    i32 ncols = 1;
    if (max_rows > 4 && static_cast<i32>(count) > max_rows) {
        ncols = (static_cast<i32>(count) + max_rows - 1) / max_rows;
        const i32 fit = (c.width() - c.px(16)) / w;
        ncols = ncols > fit ? (fit > 1 ? fit : 1) : ncols;
    }
    const i32 per = (static_cast<i32>(count) + ncols - 1) / ncols; // items per column
    const i32 rows = per < max_rows ? per : (max_rows > 4 ? max_rows : 4);
    // A scrollbar down its right side when not every row fits (2026-11-09).
    const i32 bar_w = per > rows ? c.px(kScrollBarW) + c.px(kScrollBarGap) : 0;
    Rect r{popup_r_.x, popup_r_.y, w * ncols + bar_w, rows * ih + c.px(4)};
    if (r.right() > c.width())
        r.x = c.width() - r.w;
    if (r.x < 0)
        r.x = 0;
    if (r.bottom() > c.height())
        r.y = c.height() - r.h;
    popup_r_ = r;
    const Rect track{r.right() - c.px(2) - c.px(kScrollBarW), r.y + c.px(2), c.px(kScrollBarW), r.h - c.px(4)};
    bool held = false; // the scrollbar has the mouse: no item takes the click
    if (consumed) {
        held = popup_bar_.input(in, c, r, track, per, rows, 3, &popup_scroll_);
        if (held || (in.wheel && r.contains(in.mx, in.my)))
            *consumed = true;
    }
    popup_scroll_ = scroll_clamp(per, rows, popup_scroll_);
    popup_cols_ = ncols;
    popup_per_ = per;
    popup_rows_ = rows;
    popup_w_ = w;
    popup_ih_ = ih;
    popup_count_ = count;
    if (consumed && in.pressed && !held) {
        *consumed = true;
        if (!r.contains(in.mx, in.my)) {
            popup_ = 0;
            return;
        }
        const i32 col = (in.mx - r.x) / w;
        const u32 item = static_cast<u32>(col * per + (in.my - r.y - c.px(2)) / ih + popup_scroll_);
        if (item < count) {
            if (popup_ == 1) {
                color_by_ = item;
                popup_ = 0;
            } else if (popup_ == 2) {
                strip_by_ = item;
                popup_ = 0;
            } else if (popup_ == 4) {
                sub_type_ = item;
                popup_ = 0;
            } else {
                u32* s = series_[popup_owner_ < kCharts ? popup_owner_ : 0];
                bool removed = false;
                for (u32 k = 0; k < kSeries; ++k) {
                    if (s[k] == item) {
                        s[k] = kNone;
                        removed = true;
                    }
                }
                if (!removed) {
                    u32 slot = kSeries - 1;
                    for (u32 k = 0; k < kSeries; ++k) {
                        if (s[k] == kNone) {
                            slot = k;
                            break;
                        }
                    }
                    s[slot] = item;
                }
            }
        }
        return;
    }
    if (consumed)
        return; // the input pass; drawing comes last, over everything
    // Draw.
    c.fill(r, 0x2A2E37);
    c.frame(r, kFaint);
    popup_bar_.draw(in, c, track, per, rows, popup_scroll_);
    for (i32 col = 0; col < ncols; ++col) {
        for (i32 i = 0; i < rows; ++i) {
            const u32 item = static_cast<u32>(col * per + i + popup_scroll_);
            if (item >= count || i + popup_scroll_ >= per)
                break;
            const Rect ir{r.x + col * w + c.px(2), r.y + c.px(2) + i * ih, w - c.px(4), ih};
            bool on = false;
            const char* label = "";
            if (popup_ == 1) {
                on = item == color_by_;
                label = color_by_name(item);
            } else if (popup_ == 2) {
                on = item == strip_by_;
                label = strip_by_name(item);
            } else if (popup_ == 4) {
                on = item == sub_type_;
                label = substrate_label(item);
            } else {
                for (u32 k = 0; k < kSeries; ++k)
                    on = on || series_[popup_owner_ < kCharts ? popup_owner_ : 0][k] == item;
                label = stats_column(item);
            }
            if (ir.contains(in.mx, in.my))
                c.fill(ir, kButtonHot);
            if (on)
                c.fill(Rect{ir.x, ir.y + c.px(3), c.px(3), ih - c.px(6)}, kAccent);
            c.text(ir.x + c.px(8), ir.y + c.px(2), label, kFontUi, on ? 0xFFFFFF : kText, kAlignLeft, w - c.px(12));
        }
    }
}

void Viewer::tooltip(Canvas& c) {
    if (!tip_[0])
        return;
    const i32 lh = c.line_h(kFontSmall);
    i32 w = 0, lines = 0;
    const char* s = tip_;
    while (*s) {
        const char* e = s;
        while (*e && *e != '\n')
            ++e;
        const i32 tw = c.text_w(s, static_cast<usize>(e - s), kFontSmall);
        w = tw > w ? tw : w;
        ++lines;
        s = *e ? e + 1 : e;
    }
    Rect r{tip_x_ + c.px(14), tip_y_ + c.px(14), w + c.px(12), lines * lh + c.px(8)};
    if (r.right() > c.width())
        r.x = tip_x_ - r.w - c.px(8);
    if (r.bottom() > c.height())
        r.y = tip_y_ - r.h - c.px(8);
    c.fill(r, 0x101216);
    c.frame(r, kFaint);
    i32 y = r.y + c.px(4);
    s = tip_;
    while (*s) {
        const char* e = s;
        while (*e && *e != '\n')
            ++e;
        c.text(r.x + c.px(6), y, s, static_cast<usize>(e - s), kFontSmall, kText);
        y += lh;
        s = *e ? e + 1 : e;
    }
}

} // namespace nova
