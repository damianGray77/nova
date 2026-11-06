#pragma once

// Canvas: GDI drawing into an off-screen 32-bit bitmap (Phase 6, design.md
// 6.12). The window copies it to the screen in one blit, so nothing flickers,
// and a headless render writes the same pixels to a PNG. windows.h stays out
// of this header.
#include "nova/array.h"
#include "nova/types.h"

namespace nova {

enum FontId : u32 { kFontUi, kFontBold, kFontMono, kFontSmall, kFontCount };
enum Align : u32 { kAlignLeft, kAlignCenter, kAlignRight };

struct Rect {
    i32 x = 0, y = 0, w = 0, h = 0;
    bool contains(i32 px, i32 py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    i32 right() const { return x + w; }
    i32 bottom() const { return y + h; }
};

class Canvas {
  public:
    Canvas() = default;
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    // (Re)creates the bitmap; `scale` is the DPI scale (1 at 96 dpi). Fonts are
    // recreated when the scale changes.
    bool resize(i32 w, i32 h, float scale);
    i32 width() const { return w_; }
    i32 height() const { return h_; }
    float scale() const { return scale_; }
    // Logical (96-dpi) units to pixels.
    i32 px(float v) const { return static_cast<i32>(v * scale_ + 0.5f); }

    void clip(const Rect& r); // replaces the clip
    void unclip();

    void fill(const Rect& r, u32 color);
    void frame(const Rect& r, u32 color); // a 1-pixel outline
    void line(i32 x0, i32 y0, i32 x1, i32 y1, u32 color, i32 width = 1);
    void dashed(i32 x0, i32 y0, i32 x1, i32 y1, u32 color, i32 dash);
    void circle(i32 cx, i32 cy, i32 r, u32 fill_color, u32 edge_color, bool filled);
    void polyline(const i32* xy, u32 points, u32 color, i32 width = 1);
    // Text at (x, y) = its top-left (or top-centre / top-right), clipped to
    // `max_w` pixels if given (with no ellipsis: the clip just cuts).
    void text(i32 x, i32 y, const char* s, usize n, u32 font, u32 color, u32 align = kAlignLeft, i32 max_w = 0);
    void text(i32 x, i32 y, const char* s, u32 font, u32 color, u32 align = kAlignLeft, i32 max_w = 0);
    i32 text_w(const char* s, usize n, u32 font);
    i32 text_w(const char* s, u32 font);
    i32 line_h(u32 font); // one line of the font, in pixels
    // An image of `iw` x `ih` 0x00RRGGBB pixels, scaled to fill r (nearest).
    void image(const Rect& r, const u32* pixels, i32 iw, i32 ih);

    // The bitmap's pixels, 0x00RRGGBB (the top byte is undefined), row by row
    // from the top, width() apart.
    const u32* pixels() const { return bits_; }
    // Copies the bitmap to a device context (the window's) at (0, 0).
    void present(void* hdc);
    void flush(); // finishes pending GDI drawing before pixels() is read

  private:
    void make_fonts();
    void free_fonts();

    void* dc_ = nullptr;     // HDC
    void* bmp_ = nullptr;    // HBITMAP
    void* old_bmp_ = nullptr;
    void* font_[kFontCount] = {};
    i32 font_h_[kFontCount] = {};
    u32* bits_ = nullptr;
    i32 w_ = 0, h_ = 0;
    float scale_ = 0;
};

} // namespace nova
