#pragma once
#include "common/Platform.h"
#include <d2d1.h>
#include <dwrite.h>
namespace sat {
// Strokes the glyph outline before filling it, so the rim sits outside the face. Rim styles:
// 0 hard edge, 1 rough edge, 2 glow. The renderer is handed to IDWriteTextLayout::Draw and paints
// every glyph run the layout produces.
ComPtr<IDWriteTextRenderer> CreateRimRenderer(ID2D1RenderTarget* target, ID2D1Factory* factory,
                                              ID2D1SolidColorBrush* fill, ID2D1SolidColorBrush* edge,
                                              float stroke, int style, float scale);
} // namespace sat
