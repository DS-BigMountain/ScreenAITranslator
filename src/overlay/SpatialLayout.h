#pragma once
#include "Overlay.h"
#include <d2d1.h>
namespace sat {
// Coordinates remain in captured physical pixels; DPI only controls readable font limits.
std::vector<PositionedText> LayoutPositioned(const TranslationResult&, const Image&, const Settings&, UINT dpi);
void DrawPositioned(ID2D1RenderTarget* target, const std::vector<PositionedText>& items, COLORREF textColor, bool diagnostics = false);
}
namespace sat { COLORREF ReadableTextColor(COLORREF preferred,COLORREF background); }
