#pragma once

// Widgets the viewer's panels and the setup page share (2026-11-09): the
// scrollbar beside everything the wheel scrolls, and the charts' zoomable span
// of ticks. Their arithmetic is apart from the drawing so the tests can check
// it directly.
#include "canvas.h"

namespace nova {

struct UiInput; // viewer.h

// --- the scrollbar ---------------------------------------------------------------
//
// Content `total` units long (rows, lines or pixels) of which `shown` fit,
// scrolled `pos` units (0 to total - shown). The thumb's length is the shown
// share of the track (at least min_h pixels) and its place the scroll's share
// of the most it can scroll. The wheel scrolls as it did before the bars;
// dragging the thumb scrolls in proportion, a click on the track above or
// below it pages (by `shown` units).

struct Thumb {
    i32 y = 0, h = 0; // its top, from the track's top, and its length in pixels; h = 0: all shown, no bar
};
Thumb scroll_thumb(i32 track_h, i32 min_h, i32 total, i32 shown, i32 pos);
// A drag: the scroll once the thumb, grabbed at scroll `from`, has moved `dy`
// pixels (the thumb's room on the track is the whole scroll), clamped. A grab
// that hasn't moved changes nothing.
i32 scroll_drag(i32 track_h, i32 min_h, i32 total, i32 shown, i32 from, i32 dy);
// A click `y` pixels down the track: above or below the thumb, a page that way;
// on it, no change.
i32 scroll_page(i32 track_h, i32 min_h, i32 total, i32 shown, i32 pos, i32 y);
// pos within [0, total - shown] (0 if everything fits).
i32 scroll_clamp(i32 total, i32 shown, i32 pos);

// The bar's width and its gap from the content, and the shortest thumb (96-dpi pixels).
inline constexpr float kScrollBarW = 8.0f, kScrollBarGap = 3.0f, kScrollThumbMin = 20.0f;

// `area` with the bar's track cut from its right edge: the content's rect and the track's.
void scroll_split(Canvas& c, const Rect& area, Rect* content, Rect* track);

class ScrollBar {
  public:
    // A frame's input: the wheel over `area` (`step` units a notch), then a
    // press on the track (on the thumb: a drag; off it: a page) and the drag;
    // *pos ends clamped. True while the bar has the mouse (a press on its track
    // this frame, or its thumb held), so the content can leave that click alone.
    bool input(const UiInput& in, Canvas& c, const Rect& area, const Rect& track, i32 total, i32 shown, i32 step,
               i32* pos);
    // Draws the track and the thumb (nothing if everything fits).
    void draw(const UiInput& in, Canvas& c, const Rect& track, i32 total, i32 shown, i32 pos) const;
    // Both, for a panel that takes its input as it draws.
    bool update(const UiInput& in, Canvas& c, const Rect& area, const Rect& track, i32 total, i32 shown, i32 step,
                i32* pos) {
        const bool held = input(in, c, area, track, total, shown, step, pos);
        draw(in, c, track, total, shown, *pos);
        return held;
    }
    bool dragging() const { return drag_; }
    void release() { drag_ = false; }

  private:
    bool drag_ = false;
    i32 grab_y_ = 0, grab_pos_ = 0; // the mouse's y and the scroll when the thumb was grabbed
};

// --- the charts' span of ticks ---------------------------------------------------
//
// The span [lo, hi] the charts show: the whole run's, or a part of it the wheel
// zoomed into. Each notch away from the user narrows it by kChartZoomStep
// about the tick under the mouse (which stays under it), each toward widens it;
// it never gets wider than the whole run (zoomed right out it is the whole
// run) nor narrower than min_w, and it stays inside the run.

struct TickSpan {
    double lo = 0, hi = 0;
    double width() const { return hi - lo; }
};
inline constexpr double kChartZoomStep = 1.25; // a notch (the map's step too)
TickSpan zoom_span(TickSpan s, double at, i32 notches, TickSpan full, double min_w);
// The span moved by dt ticks, its width kept, inside `full` (dragging pans it).
TickSpan pan_span(TickSpan s, double dt, TickSpan full);

} // namespace nova
