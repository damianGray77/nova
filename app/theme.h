#pragma once

// The analysis UI's dark theme (Phase 6, design.md 6.12), shared by the
// viewer's panels and the setup page (docs/config-plan.md C2). Colours are
// 0x00RRGGBB; virtual keys are Win32's VK_ codes (windows.h stays out).
#include "nova/types.h"

namespace nova {

inline constexpr u32 kBg = 0x16181D, kPanel = 0x1F2229, kHead = 0x282C35, kText = 0xD8DEE9, kDim = 0x8A91A0,
                     kFaint = 0x4A505E, kAccent = 0x5EA1FF, kWarn = 0xE5A050, kBad = 0xE06060, kGood = 0x60C080,
                     kEdge = 0x3A404D, kEdgeLong = 0x6A5A9A, kEdgeDown = 0xA04848, kCargo = 0xFFD27F,
                     kButton = 0x30343F, kButtonHot = 0x3B4150, kButtonOn = 0x2F5C9A, kRowSel = 0x2F4A70,
                     kRowHot = 0x2A2E38, kField = 0x10141A, kPopup = 0x2A2E37, kTipBg = 0x101216;
inline constexpr u32 kSeriesColor[4] = {0x5EA1FF, 0xF2A65A, 0x7BD88F, 0xE06C9F};

inline constexpr u32 kVkBack = 0x08, kVkTab = 0x09, kVkReturn = 0x0D, kVkEscape = 0x1B, kVkDelete = 0x2E;

} // namespace nova
