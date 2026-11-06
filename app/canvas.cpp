#include "canvas.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstring>

namespace nova {

namespace {

COLORREF cref(u32 c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }

} // namespace

Canvas::~Canvas() {
    free_fonts();
    if (dc_) {
        SelectObject(static_cast<HDC>(dc_), static_cast<HGDIOBJ>(old_bmp_));
        DeleteDC(static_cast<HDC>(dc_));
    }
    if (bmp_)
        DeleteObject(static_cast<HGDIOBJ>(bmp_));
}

void Canvas::free_fonts() {
    for (u32 i = 0; i < kFontCount; ++i) {
        if (font_[i])
            DeleteObject(static_cast<HGDIOBJ>(font_[i]));
        font_[i] = nullptr;
    }
}

void Canvas::make_fonts() {
    free_fonts();
    struct Spec {
        const wchar_t* face;
        float size;
        int weight;
    };
    const Spec specs[kFontCount] = {{L"Segoe UI", 13.0f, FW_NORMAL},
                                    {L"Segoe UI", 13.0f, FW_SEMIBOLD},
                                    {L"Consolas", 12.5f, FW_NORMAL},
                                    {L"Segoe UI", 11.0f, FW_NORMAL}};
    for (u32 i = 0; i < kFontCount; ++i) {
        LOGFONTW lf = {};
        lf.lfHeight = -px(specs[i].size);
        lf.lfWeight = specs[i].weight;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH;
        wcsncpy_s(lf.lfFaceName, specs[i].face, LF_FACESIZE - 1);
        font_[i] = CreateFontIndirectW(&lf);
        HDC dc = static_cast<HDC>(dc_);
        HGDIOBJ old = SelectObject(dc, static_cast<HGDIOBJ>(font_[i]));
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        font_h_[i] = tm.tmHeight;
        SelectObject(dc, old);
    }
}

bool Canvas::resize(i32 w, i32 h, float scale) {
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (!dc_) {
        dc_ = CreateCompatibleDC(nullptr);
        if (!dc_)
            return false;
        SetBkMode(static_cast<HDC>(dc_), TRANSPARENT);
    }
    HDC dc = static_cast<HDC>(dc_);
    if (w != w_ || h != h_ || !bmp_) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp)
            return false;
        HGDIOBJ old = SelectObject(dc, bmp);
        if (!old_bmp_)
            old_bmp_ = old;
        if (bmp_)
            DeleteObject(static_cast<HGDIOBJ>(bmp_));
        bmp_ = bmp;
        bits_ = static_cast<u32*>(bits);
        w_ = w;
        h_ = h;
    }
    if (scale != scale_) {
        scale_ = scale;
        make_fonts();
    }
    return true;
}

void Canvas::clip(const Rect& r) {
    HRGN rgn = CreateRectRgn(r.x, r.y, r.x + r.w, r.y + r.h);
    SelectClipRgn(static_cast<HDC>(dc_), rgn);
    DeleteObject(rgn);
}

void Canvas::unclip() { SelectClipRgn(static_cast<HDC>(dc_), nullptr); }

void Canvas::fill(const Rect& r, u32 color) {
    if (r.w <= 0 || r.h <= 0)
        return;
    HDC dc = static_cast<HDC>(dc_);
    SetBkColor(dc, cref(color));
    RECT rc = {r.x, r.y, r.x + r.w, r.y + r.h};
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr); // the fastest solid fill in GDI
}

void Canvas::frame(const Rect& r, u32 color) {
    fill(Rect{r.x, r.y, r.w, 1}, color);
    fill(Rect{r.x, r.y + r.h - 1, r.w, 1}, color);
    fill(Rect{r.x, r.y, 1, r.h}, color);
    fill(Rect{r.x + r.w - 1, r.y, 1, r.h}, color);
}

void Canvas::line(i32 x0, i32 y0, i32 x1, i32 y1, u32 color, i32 width) {
    HDC dc = static_cast<HDC>(dc_);
    if (width <= 1) {
        HGDIOBJ old = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, cref(color));
        MoveToEx(dc, x0, y0, nullptr);
        LineTo(dc, x1, y1);
        SetPixelV(dc, x1, y1, cref(color)); // LineTo leaves the last pixel out
        SelectObject(dc, old);
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, width, cref(color));
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x0, y0, nullptr);
    LineTo(dc, x1, y1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void Canvas::dashed(i32 x0, i32 y0, i32 x1, i32 y1, u32 color, i32 dash) {
    const double dx = x1 - x0, dy = y1 - y0;
    const double len = sqrt(dx * dx + dy * dy);
    if (len <= 0)
        return;
    if (dash < 1)
        dash = 1;
    for (double t = 0; t < len; t += 2.0 * dash) {
        const double t1 = t + dash < len ? t + dash : len;
        line(x0 + static_cast<i32>(dx * t / len), y0 + static_cast<i32>(dy * t / len),
             x0 + static_cast<i32>(dx * t1 / len), y0 + static_cast<i32>(dy * t1 / len), color);
    }
}

void Canvas::circle(i32 cx, i32 cy, i32 r, u32 fill_color, u32 edge_color, bool filled) {
    HDC dc = static_cast<HDC>(dc_);
    if (r < 1)
        r = 1;
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    HGDIOBJ ob = SelectObject(dc, GetStockObject(filled ? DC_BRUSH : NULL_BRUSH));
    SetDCPenColor(dc, cref(edge_color));
    SetDCBrushColor(dc, cref(fill_color));
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

void Canvas::polyline(const i32* xy, u32 points, u32 color, i32 width) {
    if (points < 2)
        return;
    HDC dc = static_cast<HDC>(dc_);
    HPEN pen = width > 1 ? CreatePen(PS_SOLID, width, cref(color)) : nullptr;
    HGDIOBJ old = SelectObject(dc, pen ? static_cast<HGDIOBJ>(pen) : GetStockObject(DC_PEN));
    if (!pen)
        SetDCPenColor(dc, cref(color));
    // Polyline wants POINTs; draw in chunks from a small stack buffer.
    POINT buf[256];
    u32 at = 0;
    while (at + 1 < points) {
        u32 k = 0;
        for (; k < 256 && at + k < points; ++k) {
            buf[k].x = xy[2 * (at + k)];
            buf[k].y = xy[2 * (at + k) + 1];
        }
        Polyline(dc, buf, static_cast<int>(k));
        at += k - 1; // the next chunk starts at this one's last point
        if (k < 256)
            break;
    }
    SelectObject(dc, old);
    if (pen)
        DeleteObject(pen);
}

void Canvas::text(i32 x, i32 y, const char* s, usize n, u32 font, u32 color, u32 align, i32 max_w) {
    if (n == 0)
        return;
    HDC dc = static_cast<HDC>(dc_);
    HGDIOBJ old = SelectObject(dc, static_cast<HGDIOBJ>(font_[font < kFontCount ? font : 0]));
    SetTextColor(dc, cref(color));
    SIZE sz = {};
    GetTextExtentPoint32A(dc, s, static_cast<int>(n), &sz);
    i32 tx = x;
    if (align == kAlignCenter)
        tx = x - sz.cx / 2;
    else if (align == kAlignRight)
        tx = x - sz.cx;
    if (max_w > 0) {
        RECT rc = {tx, y, tx + max_w, y + sz.cy};
        if (align == kAlignRight) {
            rc.left = x - max_w;
            rc.right = x;
        }
        ExtTextOutA(dc, tx, y, ETO_CLIPPED, &rc, s, static_cast<UINT>(n), nullptr);
    } else {
        ExtTextOutA(dc, tx, y, 0, nullptr, s, static_cast<UINT>(n), nullptr);
    }
    SelectObject(dc, old);
}

void Canvas::text(i32 x, i32 y, const char* s, u32 font, u32 color, u32 align, i32 max_w) {
    text(x, y, s, strlen(s), font, color, align, max_w);
}

i32 Canvas::text_w(const char* s, usize n, u32 font) {
    if (n == 0)
        return 0;
    HDC dc = static_cast<HDC>(dc_);
    HGDIOBJ old = SelectObject(dc, static_cast<HGDIOBJ>(font_[font < kFontCount ? font : 0]));
    SIZE sz = {};
    GetTextExtentPoint32A(dc, s, static_cast<int>(n), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

i32 Canvas::text_w(const char* s, u32 font) { return text_w(s, strlen(s), font); }

i32 Canvas::line_h(u32 font) { return font_h_[font < kFontCount ? font : 0]; }

void Canvas::image(const Rect& r, const u32* pixels, i32 iw, i32 ih) {
    if (r.w <= 0 || r.h <= 0 || iw <= 0 || ih <= 0)
        return;
    HDC dc = static_cast<HDC>(dc_);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = iw;
    bi.bmiHeader.biHeight = -ih;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, r.x, r.y, r.w, r.h, 0, 0, iw, ih, pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
}

void Canvas::present(void* hdc) {
    GdiFlush();
    BitBlt(static_cast<HDC>(hdc), 0, 0, w_, h_, static_cast<HDC>(dc_), 0, 0, SRCCOPY);
}

void Canvas::flush() { GdiFlush(); }

} // namespace nova
