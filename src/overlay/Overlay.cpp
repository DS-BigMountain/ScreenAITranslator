#include "Overlay.h"
#include "SpatialLayout.h"
#include "TextRim.h"
#include "capture/Capture.h"
#include "common/TextMetrics.h"
#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <windowsx.h>
namespace sat {
namespace {
ComPtr<IDWriteFactory> WriteFactory() {
    ComPtr<IDWriteFactory> f;
    CheckHR(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(f.GetAddressOf())),
            "layout");
    return f;
}
D2D1_COLOR_F ColorF(COLORREF c, float a = 1) {
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a);
}
RECT Clamp(RECT r, RECT screen) {
    int w = std::min(Width(r), Width(screen)), h = std::min(Height(r), Height(screen));
    r.left = std::clamp(r.left, screen.left, screen.right - w);
    r.top = std::clamp(r.top, screen.top, screen.bottom - h);
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}
} // namespace
std::vector<OverlayBlock> BuildOverlay(const TranslationResult& result, Image roi, RECT screenRoi,
                                       const Monitor& monitor, const Settings& settings, bool retainSource) {
    if (result.segments.empty())
        return {};
    if (!Valid(screenRoi))
        throw AppError("layout", "译文框范围无效，请重新框选");
    auto factory = WriteFactory();
    float scale = monitor.dpi / 96.f;
    OverlayBlock block;
    block.screen = screenRoi;
    block.monitorArea = monitor.rect;
    block.workArea = monitor.rect;
    MONITORINFO info{sizeof(info)};
    auto handle = MonitorFromRect(&screenRoi, MONITOR_DEFAULTTONULL);
    if (GetMonitorInfoW(handle, &info) && EqualRect(&info.rcMonitor, &monitor.rect))
        block.workArea = info.rcWork;
    for (const auto& segment : result.segments) {
        if (!block.text.empty())
            block.text += L"\n\n";
        if (segment.untranslated)
            block.text += L"【未完成翻译，保留原文】\n";
        block.text += Wide(segment.untranslated ? segment.original : segment.translated);
    }
    auto bg = AnalyzeBackground(roi, {0, 0, roi.width, roi.height});
    block.background = bg.simple ? bg.color : (bg.luminance > 128 ? RGB(235, 235, 235) : RGB(20, 22, 27));
    block.opacity = bg.simple ? 1.f : settings.backgroundOpacity;
    const int w = Width(screenRoi), h = Height(screenRoi);
    const int pad = std::min(std::max(2, int(std::ceil(8 * scale))), std::max(1, (std::min(w, h) - 2) / 4));
    const int header = std::min(std::max(12, int(std::ceil(30 * scale))), std::max(1, h / 3));
    const int button = std::max(1, std::min(w - 2, header - 2));
    block.closeButton = {std::max(0, w - button - 1), 1, std::max(1, w - 1), 1 + button};
    block.viewport = {pad, std::min(h - 1, header + pad),
                      std::max(pad + 1, w - pad - std::max(2, int(5 * scale))),
                      std::max(header + pad + 1, h - pad)};
    block.viewport.right = std::min<LONG>(w, block.viewport.right);
    block.viewport.bottom = std::min<LONG>(h, block.viewport.bottom);
    block.fontSize = settings.autoFont ? 22 * scale : settings.fontSize * scale;
    // Reading panel: a document-style column with centred short headings and left-aligned body text,
    // mirroring the composed result window that dedicated translation apps use.
    const bool readingPanel =
        !settings.spatialOverlay && Width(block.viewport) >= 320 && Height(block.viewport) >= 240;
    if (readingPanel) {
        // Reading panel uses a solid theme plate: any translucency lets large source glyphs behind the
        // capture show through as ghosts, which reads as text overlap rather than context.
        block.background = overlay_theme::Surface;
        block.opacity = 1.f;
        const float margin = std::max(12.f, std::round(18 * scale));
        const float bodySize = std::clamp((settings.autoFont ? 18.f : settings.fontSize) * scale, 14.f, 40.f);
        const float headingSize = std::min(bodySize * 1.55f, 48.f);
        const float column = std::max(160.f, std::min(float(Width(block.viewport)) - 2 * margin, bodySize * 40.f));
        const float left = float(block.viewport.left) + (Width(block.viewport) - column) / 2;
        float y = float(block.viewport.top) + margin;
        auto isHeading = [](const std::wstring& text) {
            if (text.empty() || text.size() > 22)
                return false;
            const auto last = text.find_last_not_of(L" \t\r\n");
            if (last == std::wstring::npos)
                return false;
            const std::wstring_view endings = L"。！？.!?；;：:…";
            return endings.find(text[last]) == std::wstring_view::npos && text.find(L'\n') == std::wstring::npos;
        };
        const auto bodyOf = [](const Segment& segment) {
            return Wide(segment.untranslated ? segment.original : segment.translated);
        };
        // A manga page reaches this panel as one strip per column, and a balloon wraps its sentence across
        // neighbouring strips. Reading those strips back as one passage keeps a balloon on one paragraph.
        // Every strip on its own would also pass the short-heading test, which turns the whole page into a
        // centred list of fragments instead of readable prose.
        std::vector<std::vector<size_t>> groups;
        size_t strips = 0;
        for (const auto& segment : result.segments) {
            if (!segment.sourcePixels)
                continue;
            if (Height(*segment.sourcePixels) > Width(*segment.sourcePixels) * 6 / 5) {
                ++strips;
                continue;
            }
            // A balloon merged from neighbouring strips spans several of them, so the passage turns
            // wide while every strip stays narrow. The strips describe the page shape.
            if (!segment.sourceLines.empty() &&
                std::all_of(segment.sourceLines.begin(), segment.sourceLines.end(), [](RECT line) {
                    return Width(line) > 0 && Height(line) * 5 > Width(line) * 6;
                }))
                ++strips;
        }
        const bool columnPage =
            strips * 2 > result.segments.size() &&
            std::all_of(result.segments.begin(), result.segments.end(),
                        [](const Segment& segment) { return segment.sourcePixels.has_value(); });
        if (columnPage) {
            std::vector<size_t> order(result.segments.size());
            for (size_t i = 0; i < order.size(); ++i)
                order[i] = i;
            // A column page reads right to left, so the strips of one balloon arrive side by side.
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                const RECT ra = *result.segments[a].sourcePixels, rb = *result.segments[b].sourcePixels;
                return ra.left == rb.left ? ra.top > rb.top : ra.left > rb.left;
            });
            for (auto index : order) {
                if (!groups.empty()) {
                    const RECT next = *result.segments[index].sourcePixels;
                    const RECT tail = *result.segments[groups.back().back()].sourcePixels;
                    const float strip = float(std::max(1, Width(tail)));
                    const LONG overlap = std::min(tail.bottom, next.bottom) - std::max(tail.top, next.top);
                    const LONG spacing = tail.left - next.right;
                    // Neighbouring strips of one balloon share most of their glyph run and sit within a
                    // little over one strip width. Separate balloons or separate panels sit further apart.
                    if (spacing >= -LONG(strip * .5f) && spacing <= LONG(strip * 1.9f) &&
                        overlap * 100 > std::min(Height(tail), Height(next)) * 45) {
                        groups.back().push_back(index);
                        continue;
                    }
                }
                groups.push_back({index});
            }
        }
        std::vector<std::pair<std::wstring, bool>> paragraphs;
        if (columnPage) {
            for (const auto& group : groups) {
                std::wstring body;
                for (auto index : group)
                    body += bodyOf(result.segments[index]);
                if (!body.empty())
                    paragraphs.emplace_back(std::move(body), false);
            }
        } else {
            for (const auto& segment : result.segments) {
                auto body = bodyOf(segment);
                if (body.empty())
                    continue;
                const bool heading = isHeading(body);
                paragraphs.emplace_back(std::move(body), heading);
            }
        }
        for (const auto& [body, heading] : paragraphs) {
            const float size = heading ? headingSize : bodySize;
            const bool complexVertical = HasComplexVerticalScript(body);
            const float lineSpacing =
                std::max(size * (heading ? 1.3f : 1.5f), complexVertical ? size * 1.45f : 0.f);
            ComPtr<IDWriteTextFormat> paragraphFormat;
            CheckHR(factory->CreateTextFormat(settings.font.c_str(), nullptr,
                                              heading ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                              L"zh-CN", &paragraphFormat),
                    "layout");
            CheckHR(paragraphFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP), "layout");
            if (HasRtlScript(body))
                CheckHR(paragraphFormat->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT),
                        "layout");
            CheckHR(paragraphFormat->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineSpacing,
                                                    complexVertical ? size * .95f : size * .8f),
                    "layout");
            CheckHR(paragraphFormat->SetTextAlignment(heading ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                              : DWRITE_TEXT_ALIGNMENT_LEADING),
                    "layout");
            OverlayParagraph paragraph;
            paragraph.text = body;
            paragraph.heading = heading;
            paragraph.fontSize = size;
            CheckHR(factory->CreateTextLayout(body.c_str(), UINT32(body.size()), paragraphFormat.Get(), column,
                                              100000.f, &paragraph.layout),
                    "layout");
            DWRITE_TEXT_METRICS paragraphMetrics{};
            CheckHR(paragraph.layout->GetMetrics(&paragraphMetrics), "layout");
            paragraph.rect = {LONG(std::round(left)), LONG(std::round(y)), LONG(std::round(left + column)),
                              LONG(std::round(y + paragraphMetrics.height))};
            y += paragraphMetrics.height + (heading ? 6.f : 10.f) * scale;
            block.paragraphs.push_back(std::move(paragraph));
        }
        block.contentHeight = y - block.viewport.top + margin;
    }
    ComPtr<IDWriteTextFormat> format;
    CheckHR(factory->CreateTextFormat(settings.font.c_str(), nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, block.fontSize,
                                      L"zh-CN", &format),
            "layout");
    format->SetWordWrapping(HasComplexVerticalScript(block.text) ? DWRITE_WORD_WRAPPING_WRAP
                                                                   : DWRITE_WORD_WRAPPING_CHARACTER);
    if (HasRtlScript(block.text))
        format->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
    CheckHR(factory->CreateTextLayout(block.text.c_str(), static_cast<UINT32>(block.text.size()),
                                      format.Get(), static_cast<float>(std::max(1, Width(block.viewport))),
                                      static_cast<float>(std::max(1, Height(block.viewport))), &block.layout),
            "layout");
    DWRITE_TEXT_METRICS metrics{};
    CheckHR(block.layout->GetMetrics(&metrics), "layout");
    block.contentHeight = metrics.height;
    if (settings.spatialOverlay) {
        block.positioned = LayoutPositioned(result, roi, settings, monitor.dpi);
    }
    if (retainSource) {
        block.image = std::make_shared<Image>(std::move(roi));
        // Keep the structured result so the reading panel can rebuild its paragraph hierarchy later.
        block.sourceResult = std::make_shared<TranslationResult>(result);
    }
    return {std::move(block)};
}
Overlay::~Overlay() {
    CloseContinuousBadge();
    Close();
}
void Overlay::Close() {
    warning_.clear();
    aiPending_ = false;
    aiHighlight_ = false;
    passthrough_ = false;
    captureExcluded_ = true;
    dismissed_ = {};
    reselect_ = {};
    moved_ = {};
    noticeActive_ = false;
    noticeText_.clear();
    noticeBackdrop_ = {};
    if (focusShade_) {
        DestroyWindow(focusShade_);
        focusShade_ = nullptr;
    }
    if (toolbar_) {
        DestroyWindow(toolbar_);
        toolbar_ = nullptr;
    }
    for (auto& w : windows_) {
        w->drag.reset();
        if (w->hwnd)
            DestroyWindow(w->hwnd);
    }
    windows_.clear();
}
bool Overlay::Dragging() const {
    return std::any_of(windows_.begin(), windows_.end(),
                       [](const auto& window) { return window->drag.has_value(); });
}
std::shared_ptr<const Image> Overlay::SourceImage() const {
    if (windows_.empty())
        return nullptr;
    return windows_.front()->block.image;
}
bool Overlay::DragBorder(const WindowData& data, POINT point) const {
    if (!moved_ || !Valid(data.block.monitorArea))
        return false;
    const auto& active = Active(data);
    RECT client{0, 0, Width(active.screen), Height(active.screen)};
    if (!PtInRect(&client, point) ||
        ((!active.image || data.mode == 2) && PtInRect(&active.closeButton, point)))
        return false;
    const int edge = std::max(4, MulDiv(6, data.dpi, 96));
    return point.x < edge || point.y < edge || point.x >= client.right - edge ||
           point.y >= client.bottom - edge;
}
void Overlay::MoveDrag(WindowData& data, POINT point) {
    if (!data.drag)
        return;
    auto& drag = *data.drag;
    LONG dx = point.x - drag.start.x, dy = point.y - drag.start.y;
    const bool firstMove = !drag.moved;
    if (!drag.moved) {
        if (std::abs(dx) < GetSystemMetricsForDpi(SM_CXDRAG, data.dpi) &&
            std::abs(dy) < GetSystemMetricsForDpi(SM_CYDRAG, data.dpi))
            return;
        drag.moved = true;
    }
    // Keep both the actual capture and the visible reading frame on this monitor.
    // Moving a reading frame never replaces the capture with its expanded dimensions.
    const auto area = data.block.monitorArea;
    dx = std::clamp(dx, std::max(area.left - drag.region.left, area.left - drag.view.left),
                    std::min(area.right - drag.region.right, area.right - drag.view.right));
    dy = std::clamp(dy, std::max(area.top - drag.region.top, area.top - drag.view.top),
                    std::min(area.bottom - drag.region.bottom, area.bottom - drag.view.bottom));
    data.block.screen = drag.region;
    OffsetRect(&data.block.screen, dx, dy);
    if (data.ai)
        data.ai->screen = data.block.screen;
    if (data.reading && drag.reading) {
        data.reading->screen = *drag.reading;
        OffsetRect(&data.reading->screen, dx, dy);
    }
    RECT view = drag.view;
    OffsetRect(&view, dx, dy);
    if (firstMove) {
        data.dragPreview = true;
        if (toolbar_)
            ShowWindow(toolbar_, SW_HIDE);
        Render(data);
    } else
        SetWindowPos(data.hwnd, nullptr, view.left, view.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    PositionToolbar();
    UpdateShadeHole();
}
void Overlay::RestoreContents(WindowData& data) {
    if (!data.dragPreview)
        return;
    data.dragPreview = false;
    Render(data);
    if (toolbar_)
        ShowWindow(toolbar_, SW_SHOWNOACTIVATE);
    UpdateShadeHole();
}
void Overlay::CancelDrag(WindowData& data) {
    if (!data.drag)
        return;
    auto drag = *data.drag;
    data.drag.reset();
    data.pressed = false;
    data.block.screen = drag.region;
    if (data.ai)
        data.ai->screen = data.block.screen;
    if (data.reading && drag.reading)
        data.reading->screen = *drag.reading;
    SetWindowPos(data.hwnd, nullptr, drag.view.left, drag.view.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    PositionToolbar();
    if (GetCapture() == data.hwnd)
        ReleaseCapture();
    RestoreContents(data);
    UpdateShadeHole();
}
OverlayBlock& Overlay::Active(WindowData& data) {
    if (data.mode == 2 && data.reading)
        return *data.reading;
    if (data.mode == 3 && data.ai)
        return *data.ai;
    return data.block;
}
const OverlayBlock& Overlay::Active(const WindowData& data) const {
    if (data.mode == 2 && data.reading)
        return *data.reading;
    if (data.mode == 3 && data.ai)
        return *data.ai;
    return data.block;
}
void Overlay::Render(WindowData& window) {
    const auto& block = Active(window);
    const auto& settings = window.settings;
    const auto dpi = window.dpi;
    int w = Width(block.screen), h = Height(block.screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dc || !bmp) {
        if (dc)
            DeleteDC(dc);
        if (bmp)
            DeleteObject(bmp);
        throw AppError("overlay", "翻译层内存分配失败");
    }
    auto previous = SelectObject(dc, bmp);
    try {
        ComPtr<ID2D1Factory> factory;
        CheckHR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()), "overlay");
        ComPtr<ID2D1DCRenderTarget> rt;
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        CheckHR(factory->CreateDCRenderTarget(&props, &rt), "overlay");
        RECT r{0, 0, w, h};
        CheckHR(rt->BindDC(dc, &r), "overlay");
        ComPtr<ID2D1SolidColorBrush> background, fill, edge;
        CheckHR(rt->CreateSolidColorBrush(ColorF(block.background, block.opacity), &background), "overlay");
        auto foreground = settings.textColor;
        // Default white switches to dark on a light cover; explicitly chosen colors remain honored.
        foreground = ReadableTextColor(foreground, block.background);
        CheckHR(rt->CreateSolidColorBrush(ColorF(foreground), &fill), "overlay");
        // The inverse mode derives the rim from the rendered face colour, so the rim keeps its
        // contrast when the face flips between a dark cover and a light one.
        const COLORREF rim =
            settings.outlineColorMode == 1
                ? RGB(255 - GetRValue(foreground), 255 - GetGValue(foreground), 255 - GetBValue(foreground))
                : settings.outlineColor;
        CheckHR(rt->CreateSolidColorBrush(ColorF(rim), &edge), "overlay");
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0.f));
        auto drawSource = [&] {
            const auto& image = *block.image;
            ComPtr<ID2D1Bitmap> bitmap;
            auto bitmapProps = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            CheckHR(rt->CreateBitmap(D2D1::SizeU(image.width, image.height), image.bgra.data(),
                                     image.width * 4, bitmapProps, &bitmap),
                    "overlay");
            rt->DrawBitmap(bitmap.Get(), D2D1::RectF(0, 0, float(w), float(h)));
        };
        if (window.dragPreview) {
            // Processing retains the selected frame; moving a result keeps the desktop visible.
            if (window.progressFrame && !passthrough_ && block.image)
                drawSource();
            edge->SetColor(ColorF(overlay_theme::Accent));
            const float stroke = std::min(std::max(1.5f, 1.5f * dpi / 96.f), float(std::min(w, h)) / 2);
            rt->DrawRectangle(D2D1::RectF(stroke / 2, stroke / 2, w - stroke / 2, h - stroke / 2), edge.Get(),
                              stroke);
        } else {
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            if (block.image && window.mode != 2) {
                rt->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)),
                                  background.Get());
                drawSource();
                // Both the recognized result and the one-off vision result paint the override layer; the
                // original view and the reading panel keep their own drawings.
                if (window.mode == 0 || window.mode == 3)
                    DrawPositioned(rt.Get(), block.positioned, block.image.get(), settings.textColor, settings,
                                   dpi);
                edge->SetColor(ColorF(overlay_theme::Accent));
                const float stroke = std::max(1.5f, 1.5f * dpi / 96.f);
                rt->DrawRectangle(D2D1::RectF(stroke / 2, stroke / 2, float(w) - stroke / 2,
                                              float(h) - stroke / 2),
                                  edge.Get(), stroke);
            } else {
                const float scale = dpi / 96.f;
                const float radius = std::min(8.f * scale, float(std::min(w, h)) * .25f);
                const float stroke = std::min(std::max(1.f, 1.5f * scale), float(std::min(w, h)) / 2);
                const D2D1_ROUNDED_RECT card{
                    D2D1::RectF(stroke / 2, stroke / 2, float(w) - stroke / 2, float(h) - stroke / 2),
                    radius, radius};
                rt->FillRoundedRectangle(card, background.Get());
                // Clip all translated text to the selected region's content area. Long text scrolls.
                auto area = D2D1::RectF(float(block.viewport.left), float(block.viewport.top),
                                        float(block.viewport.right), float(block.viewport.bottom));
                rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
                const float outline = (window.reading && window.mode == 2) ? 0.f : settings.outlineWidth * scale;
                if (!block.paragraphs.empty()) {
                    // Reading panel: every paragraph owns its layout so headings centre independently.
                    for (const auto& paragraph : block.paragraphs) {
                        auto renderer = CreateRimRenderer(rt.Get(), factory.Get(), fill.Get(), edge.Get(),
                                                          outline, settings.outlineStyle, scale);
                        CheckHR(paragraph.layout->Draw(nullptr, renderer.Get(),
                                                       float(paragraph.rect.left),
                                                       float(paragraph.rect.top) - window.scroll),
                                "overlay");
                    }
                } else {
                    auto renderer = CreateRimRenderer(rt.Get(), factory.Get(), fill.Get(), edge.Get(), outline,
                                                      settings.outlineStyle, scale);
                    CheckHR(block.layout->Draw(nullptr, renderer.Get(), area.left, area.top - window.scroll),
                            "overlay");
                }
                rt->PopAxisAlignedClip();
                ComPtr<ID2D1SolidColorBrush> frame, header, divider, button, white, muted;
                CheckHR(rt->CreateSolidColorBrush(ColorF(overlay_theme::Accent), &frame), "overlay");
                CheckHR(rt->CreateSolidColorBrush(ColorF(overlay_theme::Header), &header), "overlay");
                CheckHR(rt->CreateSolidColorBrush(ColorF(overlay_theme::Border), &divider), "overlay");
                const COLORREF closeColor =
                    window.pressed ? overlay_theme::CloseHeld
                                   : (window.closeHover ? overlay_theme::CloseHover : overlay_theme::CloseRest);
                CheckHR(rt->CreateSolidColorBrush(ColorF(closeColor), &button), "overlay");
                CheckHR(rt->CreateSolidColorBrush(ColorF(overlay_theme::TextPrimary), &white), "overlay");
                CheckHR(rt->CreateSolidColorBrush(ColorF(overlay_theme::TextSecondary), &muted), "overlay");
                const float headerBottom = float(block.closeButton.bottom + 1);
                rt->PushAxisAlignedClip(D2D1::RectF(0, 0, float(w), headerBottom), D2D1_ANTIALIAS_MODE_ALIASED);
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(stroke / 2, stroke / 2, float(w) - stroke / 2,
                                                  headerBottom + radius),
                                      radius, radius),
                    header.Get());
                rt->PopAxisAlignedClip();
                rt->DrawLine(D2D1::Point2F(stroke, headerBottom), D2D1::Point2F(float(w) - stroke, headerBottom),
                             divider.Get(), std::max(1.f, scale));
                const auto& close = block.closeButton;
                const float btnPad = std::max(2.f, 3.f * scale);
                auto closeRect = D2D1::RectF(float(close.left) + btnPad, float(close.top) + btnPad,
                                             float(close.right) - btnPad, float(close.bottom) - btnPad);
                const float btnRadius = std::min(4.f * scale, (closeRect.right - closeRect.left) * .3f);
                rt->FillRoundedRectangle(D2D1::RoundedRect(closeRect, btnRadius, btnRadius), button.Get());
                float inset = std::max(1.5f, (closeRect.right - closeRect.left) * 0.32f);
                rt->DrawLine(D2D1::Point2F(closeRect.left + inset, closeRect.top + inset),
                             D2D1::Point2F(closeRect.right - inset, closeRect.bottom - inset), white.Get(),
                             std::max(1.f, 1.4f * scale));
                rt->DrawLine(D2D1::Point2F(closeRect.right - inset, closeRect.top + inset),
                             D2D1::Point2F(closeRect.left + inset, closeRect.bottom - inset), white.Get(),
                             std::max(1.f, 1.4f * scale));
                if (w > 100 * scale && headerBottom > 16 * scale) {
                    ComPtr<IDWriteTextFormat> title;
                    auto writer = WriteFactory();
                    CheckHR(writer->CreateTextFormat(settings.font.c_str(), nullptr,
                                                     DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                                                     DWRITE_FONT_STRETCH_NORMAL, 12 * scale, L"zh-CN",
                                                     &title),
                            "overlay");
                    title->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    title->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    const wchar_t* caption =
                        noticeActive_ ? (windows_.size() > 1 ? L"翻译进度" : L"提示")
                                      : (window.mode == 2 ? L"全文阅读" : L"译文");
                    rt->DrawText(caption, static_cast<UINT32>(wcslen(caption)), title.Get(),
                                 D2D1::RectF(10 * scale, 0, float(close.left) - 4 * scale, headerBottom),
                                 muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                if (block.contentHeight > Height(block.viewport)) {
                    const float track = float(Height(block.viewport)),
                                thumb =
                                    std::min(track, std::max(8 * scale, track * track / block.contentHeight));
                    const float top =
                        block.viewport.top + (track - thumb) * window.scroll / (block.contentHeight - track);
                    rt->FillRoundedRectangle(
                        D2D1::RoundedRect(
                            D2D1::RectF(float(w) - 6 * scale, top, float(w) - 2 * scale, top + thumb),
                            2 * scale, 2 * scale),
                        frame.Get());
                }
                rt->DrawRoundedRectangle(card, frame.Get(), stroke);
            }
        }
        CheckHR(rt->EndDraw(), "overlay");
        POINT position{block.screen.left, block.screen.top}, origin{};
        SIZE size{w, h};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(window.hwnd, nullptr, &position, &size, dc, &origin, 0, &blend, ULW_ALPHA))
            throw AppError("overlay", "无法显示翻译层");
    } catch (...) {
        SelectObject(dc, previous);
        DeleteObject(bmp);
        DeleteDC(dc);
        throw;
    }
    SelectObject(dc, previous);
    DeleteObject(bmp);
    DeleteDC(dc);
}
void Overlay::Show(const std::vector<OverlayBlock>& blocks, const Settings& settings, UINT dpi,
                   std::function<void()> dismissed, std::function<void()> reselect,
                   std::function<void(RECT)> moved, bool passthrough) {
    Close();
    dismissed_ = std::move(dismissed);
    reselect_ = std::move(reselect);
    moved_ = passthrough ? std::function<void(RECT)>{} : std::move(moved);
    passthrough_ = passthrough;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SAT.Overlay";
    wc.lpfnWndProc = Proc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    try {
        for (auto& b : blocks) {
            auto data = std::make_unique<WindowData>();
            data->owner = this;
            data->block = b;
            data->settings = settings;
            data->dpi = dpi;
            data->mode = (b.image || settings.spatialOverlay) ? 0 : 2;
            auto ptr = data.get();
            windows_.push_back(std::move(data));
            DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            if (passthrough)
                exStyle |= WS_EX_TRANSPARENT;
            ptr->hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"翻译结果 · 右上角关闭", WS_POPUP,
                                        b.screen.left, b.screen.top, Width(b.screen), Height(b.screen),
                                        nullptr, nullptr, wc.hInstance, ptr);
            if (!ptr->hwnd)
                throw AppError("overlay", "无法创建翻译层");
            if (passthrough)
                captureExcluded_ = SetWindowDisplayAffinity(ptr->hwnd, 0x00000011) != FALSE;
            Render(*ptr);
            ShowWindow(ptr->hwnd, SW_SHOWNOACTIVATE);
        }
        if (!blocks.empty() && blocks.front().image) {
            ShowToolbar();
        }
    } catch (...) {
        Close();
        throw;
    }
}
void Overlay::Notice(const std::wstring& text, RECT anchor, const Monitor& m,
                     std::function<void()> dismissed, bool excludeFromCapture) {
    Settings s;
    s.spatialOverlay = false;
    s.autoFont = false;
    s.fontSize = 17;
    s.outlineWidth = 0;
    // A notice carries the whole status line. Its box grows with the message instead of cutting the
    // tail off at a fixed height, which used to hide the end of every longer status.
    const int width = std::max(420, Width(anchor));
    int rows = 1;
    {
        const int usable = width - 20;
        int used = 0;
        for (wchar_t ch : text) {
            const int unit = ch < 128 ? 9 : 17;
            if (used + unit > usable) {
                ++rows;
                used = 0;
            }
            used += unit;
        }
    }
    const int height = std::min(240, 48 + rows * 22);
    Image image;
    image.width = width;
    image.height = height;
    image.bgra.resize(static_cast<size_t>(image.width) * image.height * 4);
    constexpr auto surfaceB = static_cast<unsigned char>((overlay_theme::Surface >> 16) & 0xFF);
    constexpr auto surfaceG = static_cast<unsigned char>((overlay_theme::Surface >> 8) & 0xFF);
    constexpr auto surfaceR = static_cast<unsigned char>(overlay_theme::Surface & 0xFF);
    for (size_t i = 0; i < image.bgra.size(); i += 4) {
        image.bgra[i] = surfaceB;
        image.bgra[i + 1] = surfaceG;
        image.bgra[i + 2] = surfaceR;
        image.bgra[i + 3] = 255;
    }
    TranslationResult r;
    r.segments.push_back({"", Utf8(text), {0, 0, 1000, 1000}});
    RECT roi{anchor.left, anchor.top, anchor.left + width, anchor.top + height};
    roi = Clamp(roi, m.rect);
    auto blocks = BuildOverlay(r, image, roi, m, s, false);
    // Show calls Close(), so the notice state must be armed afterward and the header repainted.
    Show(blocks, s, m.dpi, std::move(dismissed));
    if (excludeFromCapture && !windows_.empty() && windows_.front()->hwnd)
        SetWindowDisplayAffinity(windows_.front()->hwnd, 0x00000011);
    noticeActive_ = true;
    noticeText_ = text;
    noticeBackdrop_ = std::move(image);
    noticeMonitor_ = m;
    if (!windows_.empty())
        Render(*windows_.front());
}
void Overlay::SetProgressText(const std::wstring& text) {
    if (!noticeActive_ || text == noticeText_ || windows_.empty())
        return;
    noticeText_ = text;
    Settings s;
    s.spatialOverlay = false;
    s.autoFont = false;
    s.fontSize = 17;
    s.outlineWidth = 0;
    TranslationResult r;
    r.segments.push_back({"", Utf8(text), {0, 0, 1000, 1000}});
    auto& data = *windows_.front();
    data.block =
        BuildOverlay(r, noticeBackdrop_, data.block.screen, noticeMonitor_, s, false).front();
    Render(data);
}
void Overlay::Progress(RECT region, const Monitor& monitor, Image frozen,
                       std::function<void()> dismissed, bool continuous) {
    // Retain the exact translation input while the notice occupies a separate strip.
    RECT anchor = region;
    const int noticeHeight = 70;
    const int gap = MulDiv(8, monitor.dpi, 96);
    if (region.bottom + gap + noticeHeight <= monitor.rect.bottom)
        anchor.top = region.bottom + gap;
    else if (region.top - gap - noticeHeight >= monitor.rect.top)
        anchor.top = region.top - gap - noticeHeight;
    else if (region.bottom + noticeHeight <= monitor.rect.bottom)
        anchor.top = region.bottom;
    else if (region.top - noticeHeight >= monitor.rect.top)
        anchor.top = region.top - noticeHeight;
    anchor.bottom = anchor.top + noticeHeight;
    // Recognition always runs first, so the first line describes it; the controller swaps the line when
    // the work moves on to the service request.
    Notice(L"文字识别中 · 点击 × 取消", anchor, monitor, std::move(dismissed), continuous);
    passthrough_ = continuous;
    try {
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SAT.ProgressFrame";
        wc.lpfnWndProc = Proc;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        auto data = std::make_unique<WindowData>();
        data->owner = this;
        data->dpi = monitor.dpi;
        data->dragPreview = true;
        data->progressFrame = true;
        data->block.screen = region;
        data->block.image = std::make_shared<Image>(std::move(frozen));
        auto ptr = data.get();
        windows_.push_back(std::move(data));
        ptr->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                        WS_EX_TRANSPARENT,
                                    wc.lpszClassName, L"翻译处理中选区", WS_POPUP, region.left, region.top,
                                    Width(region), Height(region), nullptr, nullptr, wc.hInstance, ptr);
        if (!ptr->hwnd)
            throw AppError("overlay", "无法创建处理中的选区边框");
        if (continuous)
            captureExcluded_ = SetWindowDisplayAffinity(ptr->hwnd, 0x00000011) != FALSE;
        Render(*ptr);
        ShowWindow(ptr->hwnd, SW_SHOWNOACTIVATE);
        Render(*windows_.front());
    } catch (...) {
        Close();
        throw;
    }
}
LRESULT CALLBACK Overlay::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto data = reinterpret_cast<WindowData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        data = static_cast<WindowData*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        data->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
    }
    if (data)
        try {
            switch (m) {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_SETCURSOR:
                if (LOWORD(l) == HTCLIENT) {
                    POINT point{};
                    GetCursorPos(&point);
                    ScreenToClient(h, &point);
                    SetCursor(LoadCursorW(nullptr, data->drag || data->owner->DragBorder(*data, point)
                                                       ? IDC_SIZEALL
                                                       : IDC_ARROW));
                    return TRUE;
                }
                break;
            case WM_LBUTTONDOWN: {
                POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                const auto& active = data->owner->Active(*data);
                data->pressed =
                    (!active.image || data->mode == 2) && PtInRect(&active.closeButton, point) != FALSE;
                if (data->pressed)
                    data->owner->Render(*data);
                if (!data->drag && data->owner->DragBorder(*data, point)) {
                    ClientToScreen(h, &point);
                    data->drag =
                        DragState{point, data->block.screen, active.screen,
                                  data->reading ? std::optional<RECT>(data->reading->screen) : std::nullopt};
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                }
                SetCapture(h);
                return 0;
            }
            case WM_MOUSEMOVE: {
                POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (data->drag) {
                    if (!(w & MK_LBUTTON)) {
                        data->owner->CancelDrag(*data);
                        return 0;
                    }
                    ClientToScreen(h, &point);
                    data->owner->MoveDrag(*data, point);
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                    return 0;
                }
                const auto& active = data->owner->Active(*data);
                const bool hover =
                    (!active.image || data->mode == 2) && PtInRect(&active.closeButton, point) != FALSE;
                if (hover != data->closeHover) {
                    data->closeHover = hover;
                    data->owner->Render(*data);
                }
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
                TrackMouseEvent(&track);
                return 0;
            }
            case WM_MOUSELEAVE:
                if (data->closeHover) {
                    data->closeHover = false;
                    data->owner->Render(*data);
                }
                return 0;
            case WM_LBUTTONUP: {
                POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (data->drag) {
                    ClientToScreen(h, &point);
                    data->owner->MoveDrag(*data, point);
                    auto region = data->owner->Active(*data).screen;
                    bool changed = !EqualRect(&region, &data->drag->region);
                    data->drag.reset();
                    data->pressed = false;
                    auto cb = data->owner->moved_;
                    if (GetCapture() == h)
                        ReleaseCapture();
                    if (changed && cb)
                        cb(region);
                    else
                        data->owner->RestoreContents(*data);
                    return 0;
                }
                const auto& active = data->owner->Active(*data);
                const bool wasPressed = data->pressed;
                bool close = wasPressed && PtInRect(&active.closeButton, point);
                data->pressed = false;
                if (GetCapture() == h)
                    ReleaseCapture();
                if (close) {
                    auto cb = data->owner->dismissed_;
                    if (cb)
                        cb();
                } else if (wasPressed) {
                    data->owner->Render(*data);
                }
                return 0;
            }
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                if (data->drag) {
                    data->owner->CancelDrag(*data);
                    return 0;
                }
                SetCapture(h);
                return 0;
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
                if (GetCapture() == h)
                    ReleaseCapture();
                return 0;
            case WM_MOUSEWHEEL: {
                if (data->drag)
                    return 0;
                if (data->owner->Active(*data).image && data->mode != 2) {
                    if (data->mode == 0) {
                        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                        ScreenToClient(h, &point);
                        for (auto it = data->owner->Active(*data).positioned.rbegin(); it != data->owner->Active(*data).positioned.rend();
                             ++it)
                            if (PtInRect(&it->rect, point)) {
                                if (it->suppressed)
                                    continue;
                                auto maximum =
                                    std::max(0.f, it->contentHeight - (Height(it->rect) - 2 * it->padding));
                                it->scroll =
                                    std::clamp(it->scroll - GET_WHEEL_DELTA_WPARAM(w) / float(WHEEL_DELTA) *
                                                                it->fontSize * 3,
                                               0.f, maximum);
                                data->owner->Render(*data);
                                break;
                            }
                    }
                    return 0;
                }
                const auto& active = data->owner->Active(*data);
                auto maximum = std::max(0.f, active.contentHeight - Height(active.viewport));
                data->scroll = std::clamp(data->scroll - GET_WHEEL_DELTA_WPARAM(w) / float(WHEEL_DELTA) *
                                                             data->owner->Active(*data).fontSize * 3,
                                          0.f, maximum);
                data->owner->Render(*data);
                return 0;
            }
            case WM_MOUSEHWHEEL:
                return 0;
            case WM_CLOSE: {
                auto cb = data->owner->dismissed_;
                if (cb)
                    cb();
                return 0;
            }
            case WM_CANCELMODE:
            case WM_CAPTURECHANGED:
                data->pressed = false;
                data->owner->CancelDrag(*data);
                if (m == WM_CANCELMODE && GetCapture() == h)
                    ReleaseCapture();
                return 0;
            case WM_KEYDOWN:
                if (w == VK_ESCAPE && data->drag) {
                    data->owner->CancelDrag(*data);
                    return 0;
                }
                break;
            case WM_NCDESTROY:
                data->hwnd = nullptr;
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
                break;
            }
        } catch (...) {
            return 0;
        }
    return DefWindowProcW(h, m, w, l);
}
} // namespace sat
