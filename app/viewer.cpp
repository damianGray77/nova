#include "viewer.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace nova {

namespace {

// A dark theme.
const u32 kBg = 0x16181D, kPanel = 0x1F2229, kHead = 0x282C35, kText = 0xD8DEE9, kDim = 0x8A91A0,
          kFaint = 0x4A505E, kAccent = 0x5EA1FF, kWarn = 0xE5A050, kBad = 0xE06060, kGood = 0x60C080,
          kEdge = 0x3A404D, kEdgeLong = 0x6A5A9A, kEdgeDown = 0xA04848, kCargo = 0xFFD27F, kButton = 0x30343F,
          kButtonHot = 0x3B4150, kButtonOn = 0x2F5C9A, kRowSel = 0x2F4A70, kRowHot = 0x2A2E38;
const u32 kSeriesColor[4] = {0x5EA1FF, 0xF2A65A, 0x7BD88F, 0xE06C9F};

// Win32 virtual keys (windows.h stays out of the viewer).
const u32 kVkBack = 0x08, kVkReturn = 0x0D, kVkEscape = 0x1B;

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
        swap_arr(masses_, f.masses);
        memcpy(box_, f.box, sizeof box_);
    }
    cur_.knobs = f.knobs;
    memcpy(cur_.knob_key, f.knob_key, sizeof cur_.knob_key);
    memcpy(cur_.knob_value, f.knob_value, sizeof cur_.knob_value);
    cur_.sel_node = f.sel_node;
    cur_.strip_w = f.strip_w;
    cur_.strip_h = f.strip_h;
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
    }
    for (usize i = 0; i < f.saves.size(); ++i) {
        GenomeEntry e = f.saves[i];
        const u32 off = static_cast<u32>(save_text_.size());
        save_text_.append(f.save_text.data() + e.off, e.n);
        e.off = off;
        saves_.push(e);
        order_dirty_ = true;
    }
    saves_seen_ += f.saves.size();
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
    return q;
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

void Viewer::command(u32 kind, double value, const char* key, const char* text) {
    ViewCommand c = {};
    c.kind = kind;
    c.value = value;
    snprintf(c.key, sizeof c.key, "%s", key);
    snprintf(c.text, sizeof c.text, "%s", text);
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
    const i32 barh = c.px(st.live ? 66.0f : 36.0f);
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
    popup_r_ = Rect{anchor.x, anchor.bottom(), 0, 0}; // sized when drawn
}

u32 Viewer::knob_index(const char* key) const {
    for (u32 k = 0; k < cur_.knobs; ++k)
        if (strcmp(cur_.knob_key[k], key) == 0)
            return k;
    return kNone;
}

void Viewer::apply_knob_text(u32 k) {
    if (k >= cur_.knobs)
        return;
    edit_buf_[edit_len_] = '\0';
    command(kCmdKnob, 0, cur_.knob_key[k], edit_buf_);
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

    // Typing into a knob's field, or the genome browser's filter.
    if (edit_ != kNone) {
        const bool filter = edit_ == kEditFilter;
        for (u32 i = 0; i < in.nchars; ++i) {
            const char ch = in.chars[i];
            const bool ok = (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' ||
                            (!filter && (ch == 'e' || ch == 'E' || ch == '+'));
            if (ok && edit_len_ + 1 < sizeof edit_buf_ && edit_len_ + 1 < sizeof filt_)
                edit_buf_[edit_len_++] = ch;
        }
        for (u32 i = 0; i < in.nkeys; ++i) {
            if (in.keys[i] == kVkBack && edit_len_ > 0)
                --edit_len_;
            else if (in.keys[i] == kVkReturn) {
                if (filter) {
                    edit_buf_[edit_len_] = '\0';
                    set_filter(edit_buf_);
                } else {
                    apply_knob_text(edit_);
                }
                edit_ = kNone;
            } else if (in.keys[i] == kVkEscape) {
                edit_ = kNone;
            }
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

    splitters(in, c);
    if (drag_split_) {
        in.pressed = in.released = in.dbl = false;
        in.wheel = 0;
    }
    toolbar(in, c, st);
    map_panel(in, c);
    charts_panel(in, c);
    phylogeny(in, c);
    inspector(in, c);
    browser(in, c);
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
    x += c.px(56) + c.px(12);

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

    // Row 2: the live knobs.
    const i32 y2 = y + h + c.px(6);
    x = c.px(8);
    c.text(x, y2 + c.px(4), "knobs", kFontBold, kDim);
    x += c.px(52);
    for (u32 k = 0; k < cur_.knobs; ++k) {
        const char* key = cur_.knob_key[k];
        const double v = cur_.knob_value[k];
        const bool integer = strstr(key, "grant") || strstr(key, "_len");
        c.text(x, y2 + c.px(4), key, kFontSmall, kDim);
        x += c.text_w(key, kFontSmall) + c.px(4);
        const Rect field{x, y2, c.px(72), h};
        const bool editing = edit_ == k;
        c.fill(field, editing ? 0x10141A : kBg);
        c.frame(field, editing ? kAccent : kFaint);
        if (editing) {
            edit_buf_[edit_len_] = '\0';
            snprintf(buf, sizeof buf, "%s|", edit_buf_);
        } else if (integer) {
            snprintf(buf, sizeof buf, "%.0f", v);
        } else {
            snprintf(buf, sizeof buf, "%.4g", v);
        }
        c.text(field.x + c.px(4), y2 + c.px(4), buf, kFontMono, kText, kAlignLeft, field.w - c.px(6));
        if (in.pressed && field.contains(in.mx, in.my) && !st.finished) {
            edit_ = k;
            snprintf(edit_buf_, sizeof edit_buf_, integer ? "%.0f" : "%.6g", v);
            edit_len_ = static_cast<u32>(strlen(edit_buf_));
        }
        x += field.w + c.px(2);
        char val[40];
        if (button(in, c, Rect{x, y2, c.px(20), h}, "-") && !st.finished) {
            if (integer) {
                const double step = v * 0.25 < 1 ? 1 : floor(v * 0.25 + 0.5);
                snprintf(val, sizeof val, "%.0f", v - step < 1 ? 1 : v - step);
            } else {
                snprintf(val, sizeof val, "%.6g", v / 2 < 1e-9 ? 0 : v / 2);
            }
            command(kCmdKnob, 0, key, val);
        }
        x += c.px(22);
        if (button(in, c, Rect{x, y2, c.px(20), h}, "+") && !st.finished) {
            if (integer) {
                const double step = v * 0.25 < 1 ? 1 : floor(v * 0.25 + 0.5);
                snprintf(val, sizeof val, "%.0f", v + step);
            } else {
                double nv = v * 2;
                if (v <= 0)
                    nv = strstr(key, "spark") ? 0.1 : (strstr(key, "ambient") ? 0.01 : 0.0001);
                snprintf(val, sizeof val, "%.6g", nv);
            }
            command(kCmdKnob, 0, key, val);
        }
        x += c.px(20) + c.px(14);
    }
    if (cur_.knobs == 0)
        c.text(x, y2 + c.px(4), "(no live knobs in this world)", kFontSmall, kDim);
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

void Viewer::map_panel(const UiInput& in_raw, Canvas& c) {
    const UiInput& in_hdr = in_raw;
    Rect inner;
    panel_box(c, map_, "Map", &inner);
    map_inner_ = inner;
    char buf[512], num[32];
    // Header controls.
    const i32 hh = c.line_h(kFontBold) + c.px(6);
    i32 x = map_.x + c.px(56);
    snprintf(buf, sizeof buf, "colour: %s  v", color_by_name(color_by_));
    const Rect cb{x, map_.y + c.px(2), c.text_w(buf, kFontUi) + c.px(16), hh - c.px(4)};
    if (button(in_hdr, c, cb, buf, popup_ == 1))
        open_popup(1, 0, cb);
    x = cb.right() + c.px(6);
    const char* toggles[3] = {"size by bodies", "cargo", "links"};
    bool* flags[3] = {&size_by_bodies_, &show_cargo_, &show_edges_};
    for (u32 i = 0; i < 3; ++i) {
        const Rect r{x, map_.y + c.px(2), c.text_w(toggles[i], kFontUi) + c.px(16), hh - c.px(4)};
        if (button(in_hdr, c, r, toggles[i], *flags[i]))
            *flags[i] = !*flags[i];
        x = r.right() + c.px(4);
    }

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
        i32 x0, y0, x1, y1;
        to_screen(box_[0], box_[1], &x0, &y0);
        to_screen(box_[2], box_[3], &x1, &y1);
        c.fill(inner, 0x0C1520);
        c.image(Rect{x0, y0, x1 - x0, y1 - y0}, land_img_.data(), static_cast<i32>(land_w_),
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
        c.image(minimap_, land_img_.data(), static_cast<i32>(land_w_), static_cast<i32>(land_h_));
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

    // The legend and a summary line.
    const i32 ly = inner.bottom() - c.px(18);
    if (color_by_ == kColorLineage) {
        c.text(inner.x + c.px(4), ly, "each node coloured by its largest lineage", kFontSmall, kDim);
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
        set_tooltip(buf);
    }
}

// Zooms and pans so a landmass's nodes fill most of the map.
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

void Viewer::charts_panel(const UiInput& in, Canvas& c) {
    Rect inner;
    panel_box(c, charts_r_, "Charts", &inner);
    char buf[256];
    if (charts_.step() > 1) {
        snprintf(buf, sizeof buf, "every %u samples shown", charts_.step());
        c.text(charts_r_.right() - c.px(8), charts_r_.y + c.px(4), buf, kFontSmall, kDim, kAlignRight);
    }
    const usize n = charts_.size();
    const i32 each = inner.h / static_cast<i32>(kCharts);
    const i32 lab = c.px(52);
    const u64 t0 = n ? charts_.tick(0) : 0, t1 = n ? charts_.tick(n - 1) : 1;
    const double span = t1 > t0 ? static_cast<double>(t1 - t0) : 1.0;
    // The sample under the mouse (shared by every chart).
    usize hover = ~static_cast<usize>(0);
    const i32 plot_x = inner.x + lab, plot_w = inner.w - lab;
    if (n > 1 && in.mx >= plot_x && in.mx < plot_x + plot_w && inner.contains(in.mx, in.my)) {
        const double t = t0 + (static_cast<double>(in.mx - plot_x) / plot_w) * span;
        usize lo = 0, hi = n - 1;
        while (lo < hi) {
            const usize mid = (lo + hi) / 2;
            if (static_cast<double>(charts_.tick(mid)) < t)
                lo = mid + 1;
            else
                hi = mid;
        }
        hover = lo;
    }
    Array<i32> pts;
    for (u32 k = 0; k < kCharts; ++k) {
        const Rect ck{inner.x, inner.y + static_cast<i32>(k) * each, inner.w, each - c.px(4)};
        const i32 head = c.line_h(kFontSmall) + c.px(2);
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
        const Rect pr{plot_x, ck.y + head, plot_w, ck.h - head - (k + 1 == kCharts ? c.line_h(kFontSmall) : 0)};
        c.fill(pr, 0x1A1D23);
        if (n < 2 || !any) {
            if (k == 0)
                c.text(pr.x + c.px(6), pr.y + c.px(4), n < 2 ? "waiting for samples..." : "", kFontSmall, kDim);
            continue;
        }
        double vmax = 0, vmin = 0;
        for (u32 s = 0; s < kSeries; ++s) {
            const u32 col = series_[k][s];
            if (col == kNone)
                continue;
            for (usize i = 0; i < n; ++i) {
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
            if (!first_event_[e] || first_event_[e] - 1 < t0)
                continue;
            const i32 ex = pr.x + static_cast<i32>((static_cast<double>(first_event_[e] - 1) - t0) / span * pr.w);
            c.dashed(ex, pr.y, ex, pr.bottom(), kWarn, c.px(3));
            const char l[2] = {kLetter[e], 0};
            c.text(ex + c.px(2), pr.y, l, kFontSmall, kWarn);
        }
        // Collapses (red) and recoveries (green) as ticks along the bottom.
        for (usize i = events_.size(), shown = 0; i-- > 0 && shown < 4000;) {
            const ViewEvent& e = events_[i];
            if ((e.kind != kEvCollapse && e.kind != kEvRecovered) || e.tick < t0)
                continue;
            ++shown;
            const i32 ex = pr.x + static_cast<i32>((static_cast<double>(e.tick) - t0) / span * pr.w);
            c.fill(Rect{ex, pr.bottom() - c.px(4), 1, c.px(4)}, e.kind == kEvCollapse ? kBad : kGood);
        }
        for (u32 s = 0; s < kSeries; ++s) {
            const u32 col = series_[k][s];
            if (col == kNone)
                continue;
            pts.resize(2 * n);
            for (usize i = 0; i < n; ++i) {
                pts[2 * i] = pr.x + static_cast<i32>((static_cast<double>(charts_.tick(i)) - t0) / span * (pr.w - 1));
                pts[2 * i + 1] =
                    pr.bottom() - 1 - static_cast<i32>((charts_.value(i, col) - vmin) / range * (pr.h - 1));
            }
            c.polyline(pts.data(), static_cast<u32>(n), kSeriesColor[s]);
        }
        if (hover < n) {
            const i32 hx = pr.x + static_cast<i32>((static_cast<double>(charts_.tick(hover)) - t0) / span * (pr.w - 1));
            c.line(hx, pr.y, hx, pr.bottom(), kFaint);
        }
        c.unclip();
        if (k + 1 == kCharts) {
            char a[32], b[32];
            commas(t0, a);
            commas(t1, b);
            c.text(pr.x, pr.bottom(), a, kFontSmall, kDim);
            c.text(pr.right(), pr.bottom(), b, kFontSmall, kDim, kAlignRight);
            if (hover < n) {
                commas(charts_.tick(hover), a);
                snprintf(buf, sizeof buf, "tick %s", a);
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
    if (sel_lineage_)
        snprintf(title, sizeof title, "Phylogeny: lineage %s, %u genotypes", lin,
                 static_cast<unsigned>(phylo_.size()));
    else
        snprintf(title, sizeof title, "Phylogeny");
    Rect inner;
    panel_box(c, phylo_r_, title, &inner);
    if (phylo_.empty()) {
        c.text(inner.x, inner.y, sel_lineage_ ? "no saved genotypes in this lineage yet"
                                              : "select a lineage: a node, a body or a genome",
               kFontSmall, kDim, kAlignLeft, inner.w);
        return;
    }
    const i32 rh = c.line_h(kFontSmall) + c.px(3);
    const i32 rows = static_cast<i32>(phylo_.size());
    if (in.wheel && inner.contains(in.mx, in.my))
        phylo_scroll_ -= in.wheel * 3 * rh;
    const i32 max_scroll = rows * rh - inner.h;
    phylo_scroll_ = phylo_scroll_ > max_scroll ? max_scroll : phylo_scroll_;
    phylo_scroll_ = phylo_scroll_ < 0 ? 0 : phylo_scroll_;
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
    y = sb.bottom() + c.px(4);
    i32 sh = static_cast<i32>(static_cast<double>(cur_.strip_h) * inner.w / cur_.strip_w);
    const i32 cap = inner.h / 3;
    sh = sh > cap ? cap : sh;
    const Rect strip{inner.x, y, inner.w, sh};
    c.image(strip, cur_.strip.data(), static_cast<i32>(cur_.strip_w), static_cast<i32>(cur_.strip_h));
    if (strip.contains(in.mx, in.my)) {
        const u32 sx = static_cast<u32>(static_cast<double>(in.mx - strip.x) / strip.w * cur_.strip_w);
        const u32 sy = static_cast<u32>(static_cast<double>(in.my - strip.y) / strip.h * cur_.strip_h);
        const usize at = static_cast<usize>(sy) * cur_.strip_w + sx;
        if (at < cur_.strip_owner.size()) {
            const u32 slot = cur_.strip_owner[at];
            snprintf(buf, sizeof buf, "codon %u: %s", static_cast<unsigned>(at),
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
    const Rect rows{list.x, y + sh2, list.w, list.h - sh2};
    if (in.wheel && rows.contains(in.mx, in.my))
        body_scroll_ -= in.wheel * 3;
    const i32 visible = rows.h / sh2;
    const i32 count = static_cast<i32>(cur_.bodies.size());
    body_scroll_ = body_scroll_ > count - visible ? count - visible : body_scroll_;
    body_scroll_ = body_scroll_ < 0 ? 0 : body_scroll_;
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
    snprintf(buf, sizeof buf, "holds %u millicores, %u codons, %u GPU, %u NPU, %u disk; %u CPUs, %u warps; born on a %s",
             B.units, B.mem, B.gpus, B.npus, B.disk, B.ncpu, B.warps, B.form == kFormGpu ? "GPU" : "CPU");
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
    // CPUs: a compact table.
    c.text(det.x, dy, "cpu   ip    rh    wh    ax    bx    cx    dx  sp errs state       executed", kFontMono, kDim,
           kAlignLeft, det.w);
    dy += c.line_h(kFontMono);
    for (usize i = 0; i < cur_.cpus.size() && i < 6; ++i) {
        const CpuView& cv = cur_.cpus[i];
        const char* state = cv.halted ? "halted" : (cv.yielded ? "yielded" : "running");
        snprintf(buf, sizeof buf, "%3u %5u %5u %5u %5u %5u %5u %5u %3u %4u %-8s %10llu", static_cast<unsigned>(i),
                 cv.ip, cv.rh, cv.wh, cv.ax, cv.bx, cv.cx, cv.dx, cv.sp, cv.errs, state,
                 static_cast<unsigned long long>(cv.executed));
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
    const Rect gr{det.x, dy, det.w, det.bottom() - dy};
    if (in.wheel && gr.contains(in.mx, in.my))
        genome_scroll_ -= in.wheel * 2;
    genome_scroll_ = genome_scroll_ < 0 ? 0 : genome_scroll_;
    c.clip(gr);
    i32 line = 0;
    const i32 mh = c.line_h(kFontMono);
    wrap_mono(c, cur_.genome.data(), cur_.genome.size(), gr.w, [&](usize s, usize len) {
        const i32 ly = gr.y + (line - genome_scroll_) * mh;
        if (ly >= gr.y - mh && ly < gr.bottom())
            c.text(gr.x, ly, cur_.genome.data() + s, len, kFontMono, 0xC8D0A0);
        ++line;
    });
    if (genome_scroll_ > line - 1)
        genome_scroll_ = line > 0 ? line - 1 : 0;
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
    const Rect rows{inner.x, inner.y + sh, inner.w, list_h};
    if (in.wheel && rows.contains(in.mx, in.my))
        browse_scroll_ -= in.wheel * 3;
    const i32 visible = rows.h / sh;
    const i32 count = static_cast<i32>(order_.size());
    browse_scroll_ = browse_scroll_ > count - visible ? count - visible : browse_scroll_;
    browse_scroll_ = browse_scroll_ < 0 ? 0 : browse_scroll_;
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
    const u32 count = popup_ == 1 ? kColorCount : (popup_ == 2 ? kStripCount : kStatsColumns);
    const i32 ih = c.line_h(kFontUi) + c.px(4);
    i32 w = c.px(180);
    const i32 max_rows = (c.height() - popup_r_.y - c.px(8)) / ih;
    const i32 rows = static_cast<i32>(count) < max_rows ? static_cast<i32>(count) : (max_rows > 4 ? max_rows : 4);
    Rect r{popup_r_.x, popup_r_.y, w, rows * ih + c.px(4)};
    if (r.right() > c.width())
        r.x = c.width() - r.w;
    if (r.bottom() > c.height())
        r.y = c.height() - r.h;
    popup_r_ = r;
    if (consumed && in.wheel && r.contains(in.mx, in.my)) {
        popup_scroll_ -= in.wheel * 3;
        *consumed = true;
    }
    popup_scroll_ = popup_scroll_ > static_cast<i32>(count) - rows ? static_cast<i32>(count) - rows : popup_scroll_;
    popup_scroll_ = popup_scroll_ < 0 ? 0 : popup_scroll_;
    if (consumed && in.pressed) {
        *consumed = true;
        if (!r.contains(in.mx, in.my)) {
            popup_ = 0;
            return;
        }
        const u32 item = static_cast<u32>((in.my - r.y - c.px(2)) / ih + popup_scroll_);
        if (item < count) {
            if (popup_ == 1) {
                color_by_ = item;
                popup_ = 0;
            } else if (popup_ == 2) {
                strip_by_ = item;
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
    for (i32 i = 0; i < rows; ++i) {
        const u32 item = static_cast<u32>(i + popup_scroll_);
        if (item >= count)
            break;
        const Rect ir{r.x + c.px(2), r.y + c.px(2) + i * ih, r.w - c.px(4), ih};
        bool on = false;
        const char* label = "";
        if (popup_ == 1) {
            on = item == color_by_;
            label = color_by_name(item);
        } else if (popup_ == 2) {
            on = item == strip_by_;
            label = strip_by_name(item);
        } else {
            for (u32 k = 0; k < kSeries; ++k)
                on = on || series_[popup_owner_ < kCharts ? popup_owner_ : 0][k] == item;
            label = stats_column(item);
        }
        if (ir.contains(in.mx, in.my))
            c.fill(ir, kButtonHot);
        if (on)
            c.fill(Rect{ir.x, ir.y + c.px(3), c.px(3), ih - c.px(6)}, kAccent);
        c.text(ir.x + c.px(8), ir.y + c.px(2), label, kFontUi, on ? 0xFFFFFF : kText);
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
