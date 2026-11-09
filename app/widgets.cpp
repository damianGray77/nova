#include "widgets.h"

#include <cmath>

#include "theme.h"
#include "viewer.h" // UiInput

namespace nova {

// --- the scrollbar ---------------------------------------------------------------

i32 scroll_clamp(i32 total, i32 shown, i32 pos) {
    const i32 most = total - (shown > 0 ? shown : 0);
    pos = pos > most ? most : pos;
    return pos < 0 ? 0 : pos;
}

Thumb scroll_thumb(i32 track_h, i32 min_h, i32 total, i32 shown, i32 pos) {
    Thumb t;
    if (track_h <= 0 || shown <= 0 || total <= shown)
        return t;
    i32 h = static_cast<i32>(static_cast<i64>(track_h) * shown / total);
    h = h < min_h ? min_h : h;
    h = h > track_h ? track_h : h;
    const i32 room = track_h - h, most = total - shown;
    pos = scroll_clamp(total, shown, pos);
    t.h = h;
    t.y = static_cast<i32>((static_cast<i64>(pos) * room + most / 2) / most);
    return t;
}

i32 scroll_drag(i32 track_h, i32 min_h, i32 total, i32 shown, i32 from, i32 dy) {
    const Thumb t = scroll_thumb(track_h, min_h, total, shown, 0);
    const i32 room = track_h - t.h;
    if (!t.h || room <= 0)
        return scroll_clamp(total, shown, from);
    const i64 most = total - shown;
    const i64 q = static_cast<i64>(dy) * most; // dy pixels of the thumb's room: that share of the scroll
    i64 p = scroll_clamp(total, shown, from) + (q >= 0 ? q + room / 2 : q - room / 2) / room;
    p = p < 0 ? 0 : (p > most ? most : p);
    return static_cast<i32>(p);
}

i32 scroll_page(i32 track_h, i32 min_h, i32 total, i32 shown, i32 pos, i32 y) {
    pos = scroll_clamp(total, shown, pos);
    const Thumb t = scroll_thumb(track_h, min_h, total, shown, pos);
    if (!t.h)
        return pos;
    if (y < t.y)
        return scroll_clamp(total, shown, pos - shown);
    if (y >= t.y + t.h)
        return scroll_clamp(total, shown, pos + shown);
    return pos;
}

void scroll_split(Canvas& c, const Rect& area, Rect* content, Rect* track) {
    const i32 w = c.px(kScrollBarW), gap = c.px(kScrollBarGap);
    *track = Rect{area.right() - w, area.y, w, area.h};
    *content = Rect{area.x, area.y, area.w - w - gap, area.h};
}

bool ScrollBar::input(const UiInput& in, Canvas& c, const Rect& area, const Rect& track, i32 total, i32 shown,
                      i32 step, i32* pos) {
    if (in.wheel && area.contains(in.mx, in.my))
        *pos -= in.wheel * step;
    const i32 min_h = c.px(kScrollThumbMin);
    const Thumb t = scroll_thumb(track.h, min_h, total, shown, *pos);
    bool held = false;
    if (!t.h) {
        drag_ = false;
    } else {
        if (in.pressed && track.contains(in.mx, in.my)) {
            held = true;
            const i32 y = in.my - track.y;
            if (y >= t.y && y < t.y + t.h) {
                drag_ = true;
                grab_y_ = in.my;
                grab_pos_ = scroll_clamp(total, shown, *pos);
            } else {
                *pos = scroll_page(track.h, min_h, total, shown, *pos, y);
            }
        }
        if (!in.down)
            drag_ = false;
        if (drag_) {
            held = true;
            *pos = scroll_drag(track.h, min_h, total, shown, grab_pos_, in.my - grab_y_);
        }
    }
    *pos = scroll_clamp(total, shown, *pos);
    return held;
}

void ScrollBar::draw(const UiInput& in, Canvas& c, const Rect& track, i32 total, i32 shown, i32 pos) const {
    const Thumb t = scroll_thumb(track.h, c.px(kScrollThumbMin), total, shown, pos);
    if (!t.h)
        return;
    c.fill(track, kBg);
    const bool hot = drag_ || track.contains(in.mx, in.my);
    c.fill(Rect{track.x, track.y + t.y, track.w, t.h}, hot ? kDim : kButtonHot);
}

// --- the charts' span of ticks ---------------------------------------------------

TickSpan pan_span(TickSpan s, double dt, TickSpan full) {
    const double w = s.width();
    if (w >= full.width())
        return full;
    const double lo = s.lo + dt;
    if (lo <= full.lo)
        return TickSpan{full.lo, full.lo + w};
    if (lo + w >= full.hi)
        return TickSpan{full.hi - w, full.hi};
    return TickSpan{lo, lo + w};
}

TickSpan zoom_span(TickSpan s, double at, i32 notches, TickSpan full, double min_w) {
    const double fw = full.width();
    double w = s.width();
    if (fw <= 0 || w <= 0)
        return full;
    double nw = w * pow(kChartZoomStep, -notches);
    const double least = min_w < fw ? min_w : fw;
    nw = nw < least ? least : nw;
    if (nw >= fw)
        return full; // zoomed right out: the whole run
    double f = (at - s.lo) / w; // where `at` is across the span, kept
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    const double lo = at - f * nw;
    return pan_span(TickSpan{lo, lo + nw}, 0, full);
}

} // namespace nova
