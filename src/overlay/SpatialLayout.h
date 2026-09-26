#pragma once
#include "Overlay.h"
namespace sat {
// Coordinates remain in captured physical pixels; DPI only controls readable font limits.
std::vector<PositionedText> LayoutPositioned(const TranslationResult&, const Image&, const Settings&, UINT dpi);
}
namespace sat { COLORREF ReadableTextColor(COLORREF preferred,COLORREF background); }
