#include "Overlay.h"
#include "SpatialLayout.h"
#include "common/OcrLanguage.h"
#include "providers/ContentTranslation.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <d2d1.h>
#include <windowsx.h>
namespace sat {
namespace {
// The first four cells switch views, the next two run actions, closing keeps cell 6,
// and cells 7..8 host local OCR source language and target language selectors.
constexpr int ActionCount = 7;
constexpr int ReselectAction = 5;
constexpr int CloseAction = 6;
constexpr int SourceLangAction = 7;
constexpr int TargetLangAction = 8;
constexpr int TotalItemCount = 9;
constexpr int ToolbarWidth = 910;
constexpr int ToolbarHeight = 68;
constexpr int ActionRow = 40;
constexpr DWORD ExcludeFromCapture = 0x00000011;
const wchar_t* const Labels[]{L"原位译文", L"查看原图", L"全文阅读", L"AI识图翻译",
                              L"复制译文", L"重新框选", L"关闭"};
constexpr std::array<int, TotalItemCount> ItemWeights{82, 82, 82, 92, 82, 82, 68, 128, 112};
struct ToolbarGeometry {
    std::array<D2D1_RECT_F, TotalItemCount> items{};
    float dividerX{};
};
ToolbarGeometry ComputeToolbarGeometry(int width, int dpi) {
    ToolbarGeometry g;
    const float scale = dpi / 96.f;
    const float outerPadX = 6.f * scale;
    const float gap = 2.f * scale;
    const float dividerGap = 8.f * scale;
    const float usable = std::max(100.f, float(width) - outerPadX * 2 - dividerGap - gap * (TotalItemCount - 2));
    int weightSum = 0;
    for (int w : ItemWeights)
        weightSum += w;
    // Order left-to-right: [SourceLang] [TargetLang] | [0..6 actions]
    constexpr std::array<int, TotalItemCount> drawOrder{SourceLangAction, TargetLangAction,
                                                        0, 1, 2, 3, 4, 5, 6};
    float x = outerPadX;
    for (size_t pos = 0; pos < drawOrder.size(); ++pos) {
        const int id = drawOrder[pos];
        const float itemW = usable * float(ItemWeights[size_t(id)]) / float(weightSum);
        g.items[size_t(id)] = D2D1::RectF(x, 0.f, x + itemW, 0.f);
        x += itemW;
        if (pos == 1) {
            g.dividerX = x + dividerGap * .5f;
            x += dividerGap;
        } else if (pos + 1 < drawOrder.size()) {
            x += gap;
        }
    }
    return g;
}
std::wstring SourceDisplayLabel(const Settings& s) {
    return OcrLanguageShortLabel(s.ocrMode);
}
D2D1_COLOR_F ToolbarColor(COLORREF c, float a = 1.f) {
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a);
}
RECT Fit(RECT r, RECT work) {
    int w = std::min(Width(r), Width(work)), h = std::min(Height(r), Height(work));
    r.left = std::clamp(r.left, work.left, work.right - w);
    r.top = std::clamp(r.top, work.top, work.bottom - h);
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}
RECT WorkAreaForAnchor(RECT anchor, const Monitor& monitor) {
    RECT workArea = monitor.rect;
    MONITORINFO info{sizeof(info)};
    if (auto handle = MonitorFromRect(&anchor, MONITOR_DEFAULTTONULL);
        handle && GetMonitorInfoW(handle, &info))
        workArea = info.rcWork;
    return workArea;
}
RECT PlaceBarBounds(RECT anchor, RECT workArea, int width, int height, int gap) {
    const int w = std::min(width, Width(workArea));
    const int h = std::min(height, Height(workArea));
    // Default below the selection; if taskbar/work-area blocks below, place above; if neither
    // fits cleanly inside workArea, embed near the bottom edge of the selection above taskbar.
    RECT below{anchor.left, anchor.bottom + gap, anchor.left + w, anchor.bottom + gap + h};
    if (below.top >= workArea.top && below.bottom <= workArea.bottom)
        return Fit(below, workArea);
    RECT above{anchor.left, anchor.top - gap - h, anchor.left + w, anchor.top - gap};
    if (above.top >= workArea.top && above.bottom <= workArea.bottom)
        return Fit(above, workArea);
    const LONG bottom = std::min<LONG>(anchor.bottom - gap, workArea.bottom - gap);
    const LONG top = std::max<LONG>(workArea.top, bottom - h);
    return Fit(RECT{anchor.left, top, anchor.left + w, top + h}, workArea);
}
} // namespace
std::wstring Overlay::ToolbarStatusText() const {
    if (!warning_.empty())
        return warning_;
    if (noticeActive_ && !noticeText_.empty())
        return noticeText_;
    if (aiPending_)
        return L"AI识图翻译中 · 请稍候...";
    return {};
}
const std::vector<std::string>* Overlay::ActiveDetectedLanguages() const {
    if (toolbarBusy_ || windows_.empty())
        return nullptr;
    const auto& block = Active(*windows_.front());
    if (block.sourceResult && !block.sourceResult->detectedLanguages.empty())
        return &block.sourceResult->detectedLanguages;
    if (windows_.front()->block.sourceResult && !windows_.front()->block.sourceResult->detectedLanguages.empty())
        return &windows_.front()->block.sourceResult->detectedLanguages;
    return nullptr;
}
bool Overlay::HasToolbarStatusRow() const {
    return true;
}
UINT Overlay::ActiveToolbarDpi() const {
    if (!toolbarStandby_ && !windows_.empty())
        return windows_.front()->dpi;
    return toolbarDpi_ ? toolbarDpi_ : 96;
}
RECT Overlay::ActiveToolbarAnchor() const {
    if (!toolbarStandby_ && !windows_.empty()) {
        const auto& data = *windows_.front();
        return data.mode == 2 && data.reading ? data.reading->screen : data.block.screen;
    }
    return toolbarAnchor_;
}
RECT Overlay::ActiveToolbarWorkArea() const {
    if (!toolbarStandby_ && !windows_.empty())
        return windows_.front()->block.workArea;
    return Valid(toolbarWorkArea_) ? toolbarWorkArea_ : RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}
const Settings& Overlay::ActiveToolbarSettings() const {
    if (!toolbarStandby_ && !windows_.empty())
        return windows_.front()->settings;
    return toolbarSettings_;
}
bool Overlay::ToolbarActionEnabled(int action) const {
    if (action == CloseAction)
        return true;
    if (action == ReselectAction)
        return bool(reselect_);
    return !toolbarBusy_ && !toolbarStandby_ && !windows_.empty() && !windows_.front()->progressFrame;
}
void Overlay::ShowToolbar() {
    if (toolbar_) {
        PositionToolbar();
        PaintToolbar();
        ShowWindow(toolbar_, SW_SHOWNOACTIVATE);
        return;
    }
    const UINT dpi = ActiveToolbarDpi();
    const RECT anchor = ActiveToolbarAnchor();
    const RECT workArea = ActiveToolbarWorkArea();
    const int logicalHeight = HasToolbarStatusRow() ? ToolbarHeight : ActionRow;
    const RECT bounds = PlaceBarBounds(anchor, workArea, MulDiv(ToolbarWidth, dpi, 96),
                                       MulDiv(logicalHeight, dpi, 96), MulDiv(8, dpi, 96));
    HWND ownerHwnd = !windows_.empty() ? windows_.front()->hwnd : nullptr;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SAT.TranslationToolbar";
    wc.lpfnWndProc = ToolbarProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    toolbar_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               wc.lpszClassName, L"翻译操作", WS_POPUP, bounds.left, bounds.top,
                               Width(bounds), Height(bounds), ownerHwnd, nullptr, wc.hInstance, this);
    if (!toolbar_)
        throw AppError("overlay", "无法创建翻译工具条");
    if (passthrough_)
        SetWindowDisplayAffinity(toolbar_, ExcludeFromCapture);
    PaintToolbar();
    ShowWindow(toolbar_, SW_SHOWNOACTIVATE);
}
void Overlay::PositionToolbar() {
    if (!toolbar_ || (!toolbarStandby_ && windows_.empty()))
        return;
    const UINT dpi = ActiveToolbarDpi();
    const RECT anchor = ActiveToolbarAnchor();
    const RECT workArea = ActiveToolbarWorkArea();
    const int logicalHeight = HasToolbarStatusRow() ? ToolbarHeight : ActionRow;
    const RECT bounds = PlaceBarBounds(anchor, workArea, MulDiv(ToolbarWidth, dpi, 96),
                                       MulDiv(logicalHeight, dpi, 96), MulDiv(8, dpi, 96));
    SetWindowPos(toolbar_, HWND_TOPMOST, bounds.left, bounds.top, Width(bounds), Height(bounds),
                 SWP_NOACTIVATE);
}
void Overlay::RefreshToolbar() {
    if (!toolbar_)
        return;
    PositionToolbar();
    UpdateShadeHole();
    PaintToolbar();
}
void Overlay::PaintToolbar() {
    if (!toolbar_ || (!toolbarStandby_ && windows_.empty()))
        return;
    RECT wndRect{};
    GetWindowRect(toolbar_, &wndRect);
    const int w = Width(wndRect), h = Height(wndRect);
    if (w <= 0 || h <= 0)
        return;
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
        return;
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
        RECT client{0, 0, w, h};
        CheckHR(rt->BindDC(dc, &client), "overlay");
        ComPtr<IDWriteFactory> writeFactory;
        CheckHR(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                    reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf())),
                "overlay");
        const int dpi = ActiveToolbarDpi();
        const float scale = dpi / 96.f;
        const float stroke = std::max(1.f, 1.2f * scale);
        const float outerRadius = std::min(8.f * scale, float(std::min(w, h)) * .25f);
        ComPtr<ID2D1SolidColorBrush> brush;
        CheckHR(rt->CreateSolidColorBrush(ToolbarColor(overlay_theme::Surface), &brush), "overlay");
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0.f));
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        const D2D1_ROUNDED_RECT panel{
            D2D1::RectF(stroke / 2, stroke / 2, float(w) - stroke / 2, float(h) - stroke / 2), outerRadius,
            outerRadius};
        rt->FillRoundedRectangle(panel, brush.Get());
        const float row = float(MulDiv(ActionRow, dpi, 96));
        const int mode = (!toolbarBusy_ && !toolbarStandby_ && !windows_.empty()) ? windows_.front()->mode : -1;
        const int highlighted = aiPending_ && aiHighlight_ ? 3 : mode;
        const int pressed = pressedAction_;
        const int hovered = hoverAction_;
        ComPtr<IDWriteTextFormat> buttonFormat;
        CheckHR(writeFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                               13.f * scale, L"zh-CN", &buttonFormat),
                "overlay");
        CheckHR(buttonFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER), "overlay");
        CheckHR(buttonFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER), "overlay");
        CheckHR(buttonFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "overlay");
        const float padY = 5.f * scale, pillRadius = 6.f * scale;
        const auto geom = ComputeToolbarGeometry(w, dpi);
        const auto& activeSettings = ActiveToolbarSettings();
        const std::wstring sourceLabel = SourceDisplayLabel(activeSettings);
        const std::wstring targetLabel = FindTextTarget(activeSettings.screenTargetLanguage).label;
        auto drawChevron = [&](float cx, float cy, float sz, COLORREF col, float alpha = 1.f) {
            brush->SetColor(ToolbarColor(col, alpha));
            rt->DrawLine(D2D1::Point2F(cx - sz, cy - sz * .45f), D2D1::Point2F(cx, cy + sz * .45f),
                         brush.Get(), std::max(1.1f, 1.3f * scale));
            rt->DrawLine(D2D1::Point2F(cx, cy + sz * .45f), D2D1::Point2F(cx + sz, cy - sz * .45f),
                         brush.Get(), std::max(1.1f, 1.3f * scale));
        };
        ComPtr<IDWriteTextFormat> langFormat;
        CheckHR(writeFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                               12.5f * scale, L"zh-CN", &langFormat),
                "overlay");
        CheckHR(langFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER), "overlay");
        CheckHR(langFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER), "overlay");
        CheckHR(langFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "overlay");
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsisObj;
        if (SUCCEEDED(writeFactory->CreateEllipsisTrimmingSign(langFormat.Get(), &ellipsisObj)))
            langFormat->SetTrimming(&trim, ellipsisObj.Get());
        for (int id : {SourceLangAction, TargetLangAction}) {
            const auto& r = geom.items[size_t(id)];
            const bool enabled = ToolbarActionEnabled(id);
            const D2D1_RECT_F pillRect = D2D1::RectF(r.left, padY, r.right, row - padY);
            const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(pillRect, pillRadius, pillRadius);
            const bool held = enabled && id == pressed, hover = enabled && id == hovered;
            if (held || hover) {
                brush->SetColor(ToolbarColor(held ? overlay_theme::Pressed : overlay_theme::Hover));
                rt->FillRoundedRectangle(pill, brush.Get());
            }
            const std::wstring& txt = id == SourceLangAction ? sourceLabel : targetLabel;
            const float chevronReserve = 15.f * scale;
            const D2D1_RECT_F textRect =
                D2D1::RectF(pillRect.left + 4.f * scale, pillRect.top, pillRect.right - chevronReserve, pillRect.bottom);
            brush->SetColor(ToolbarColor(enabled ? overlay_theme::TextPrimary : overlay_theme::TextSecondary,
                                         enabled ? 1.f : .42f));
            rt->DrawText(txt.c_str(), UINT32(txt.size()), langFormat.Get(), textRect, brush.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
            drawChevron(pillRect.right - 9.f * scale, (pillRect.top + pillRect.bottom) * .5f,
                        3.5f * scale, overlay_theme::TextSecondary, enabled ? 1.f : .42f);
        }
        brush->SetColor(ToolbarColor(overlay_theme::Border));
        rt->DrawLine(D2D1::Point2F(geom.dividerX, 9.f * scale), D2D1::Point2F(geom.dividerX, row - 9.f * scale),
                     brush.Get(), std::max(1.f, scale));
        for (int i = 0; i < ActionCount; ++i) {
            const auto& r = geom.items[size_t(i)];
            const bool enabled = ToolbarActionEnabled(i);
            const D2D1_RECT_F pillRect = D2D1::RectF(r.left, padY, r.right, row - padY);
            const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(pillRect, pillRadius, pillRadius);
            const bool held = enabled && i == pressed, hover = enabled && i == hovered;
            COLORREF textColor = enabled ? overlay_theme::TextPrimary : overlay_theme::TextSecondary;
            float textAlpha = enabled ? 1.f : .42f;
            if (i == CloseAction) {
                const COLORREF fill = held  ? overlay_theme::CloseHeld
                                      : hover ? overlay_theme::CloseHover
                                              : overlay_theme::CloseRest;
                brush->SetColor(ToolbarColor(fill));
                rt->FillRoundedRectangle(pill, brush.Get());
                textColor = overlay_theme::TextPrimary;
                textAlpha = 1.f;
            } else if (enabled && i < 4 && i == highlighted) {
                brush->SetColor(ToolbarColor(held ? overlay_theme::AccentHeld : overlay_theme::AccentFill));
                rt->FillRoundedRectangle(pill, brush.Get());
                brush->SetColor(ToolbarColor(overlay_theme::Accent));
                rt->DrawRoundedRectangle(pill, brush.Get(), std::max(1.f, scale));
                textColor = overlay_theme::AccentLabel;
            } else if (held || hover) {
                brush->SetColor(ToolbarColor(held ? overlay_theme::Pressed : overlay_theme::Hover));
                rt->FillRoundedRectangle(pill, brush.Get());
            }
            brush->SetColor(ToolbarColor(textColor, textAlpha));
            rt->DrawText(Labels[i], static_cast<UINT32>(wcslen(Labels[i])), buttonFormat.Get(), pillRect,
                         brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        const std::wstring statusText = ToolbarStatusText();
        const auto* detectedLangs = ActiveDetectedLanguages();
        if (!statusText.empty()) {
            brush->SetColor(ToolbarColor(overlay_theme::Border));
            rt->DrawLine(D2D1::Point2F(10.f * scale, row), D2D1::Point2F(float(w) - 10.f * scale, row),
                         brush.Get(), std::max(1.f, scale));
            ComPtr<IDWriteTextFormat> statusFormat;
            CheckHR(writeFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                   12.f * scale, L"zh-CN", &statusFormat),
                    "overlay");
            CheckHR(statusFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER), "overlay");
            CheckHR(statusFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "overlay");
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(writeFactory->CreateEllipsisTrimmingSign(statusFormat.Get(), &ellipsis)))
                statusFormat->SetTrimming(&trimming, ellipsis.Get());
            const COLORREF statusColor =
                !warning_.empty() ? overlay_theme::WarningText : overlay_theme::Accent;
            brush->SetColor(ToolbarColor(statusColor));
            const D2D1_RECT_F statusRect =
                D2D1::RectF(12.f * scale, row, float(w) - 12.f * scale, float(h) - 2.f * scale);
            rt->DrawText(statusText.c_str(), static_cast<UINT32>(statusText.size()), statusFormat.Get(),
                         statusRect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else if (detectedLangs && !detectedLangs->empty()) {
            brush->SetColor(ToolbarColor(overlay_theme::Border));
            rt->DrawLine(D2D1::Point2F(10.f * scale, row), D2D1::Point2F(float(w) - 10.f * scale, row),
                         brush.Get(), std::max(1.f, scale));
            ComPtr<IDWriteTextFormat> infoFormat;
            CheckHR(writeFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                   11.5f * scale, L"zh-CN", &infoFormat),
                    "overlay");
            CheckHR(infoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER), "overlay");
            CheckHR(infoFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "overlay");
            const std::wstring prefix = L"识别到";
            ComPtr<IDWriteTextLayout> prefixLayout;
            CheckHR(writeFactory->CreateTextLayout(prefix.c_str(), UINT32(prefix.size()), infoFormat.Get(),
                                                   200.f * scale, float(h) - row, &prefixLayout),
                    "overlay");
            DWRITE_TEXT_METRICS pm{};
            CheckHR(prefixLayout->GetMetrics(&pm), "overlay");
            float curX = 12.f * scale;
            const float midY = (row + float(h) - 2.f * scale) * .5f;
            brush->SetColor(ToolbarColor(overlay_theme::TextSecondary));
            rt->DrawText(prefix.c_str(), UINT32(prefix.size()), infoFormat.Get(),
                         D2D1::RectF(curX, row, curX + pm.width + 6.f * scale, float(h) - 2.f * scale),
                         brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            curX += pm.width + 8.f * scale;
            const float chipH = 19.f * scale;
            const float chipTop = midY - chipH * .5f;
            const float chipBottom = midY + chipH * .5f;
            const float chipPadX = 8.f * scale;
            const float chipGap = 6.f * scale;
            const float maxX = float(w) - 12.f * scale;
            ComPtr<IDWriteTextFormat> chipFormat;
            CheckHR(writeFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                   11.5f * scale, L"zh-CN", &chipFormat),
                    "overlay");
            CheckHR(chipFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER), "overlay");
            CheckHR(chipFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER), "overlay");
            CheckHR(chipFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "overlay");
            for (const auto& nameUtf8 : *detectedLangs) {
                const std::wstring chipText = Wide(nameUtf8);
                ComPtr<IDWriteTextLayout> chipLayout;
                CheckHR(writeFactory->CreateTextLayout(chipText.c_str(), UINT32(chipText.size()),
                                                       chipFormat.Get(), 300.f * scale, chipH, &chipLayout),
                        "overlay");
                DWRITE_TEXT_METRICS cm{};
                CheckHR(chipLayout->GetMetrics(&cm), "overlay");
                const float chipW = cm.width + chipPadX * 2.f;
                if (curX + chipW > maxX)
                    break;
                const D2D1_ROUNDED_RECT chipRect = D2D1::RoundedRect(
                    D2D1::RectF(curX, chipTop, curX + chipW, chipBottom), 4.f * scale, 4.f * scale);
                brush->SetColor(ToolbarColor(overlay_theme::Elevated));
                rt->FillRoundedRectangle(chipRect, brush.Get());
                brush->SetColor(ToolbarColor(overlay_theme::Border));
                rt->DrawRoundedRectangle(chipRect, brush.Get(), std::max(1.f, scale));
                brush->SetColor(ToolbarColor(overlay_theme::AccentLabel));
                rt->DrawText(chipText.c_str(), UINT32(chipText.size()), chipFormat.Get(), chipRect.rect,
                             brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                curX += chipW + chipGap;
            }
        } else {
            brush->SetColor(ToolbarColor(overlay_theme::Border));
            rt->DrawLine(D2D1::Point2F(10.f * scale, row), D2D1::Point2F(float(w) - 10.f * scale, row),
                         brush.Get(), std::max(1.f, scale));
            ComPtr<IDWriteTextFormat> idleFormat;
            CheckHR(writeFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                   11.5f * scale, L"zh-CN", &idleFormat),
                    "overlay");
            CheckHR(idleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER), "overlay");
            CheckHR(idleFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "overlay");
            const wchar_t* hint = passthrough_ ? L"连续翻译中 · 点击「关闭」或按 Esc 退出"
                                               : L"拖动边缘可移动选区 · 点击「关闭」或按 Esc 退出";
            brush->SetColor(ToolbarColor(overlay_theme::TextSecondary));
            rt->DrawText(hint, static_cast<UINT32>(wcslen(hint)), idleFormat.Get(),
                         D2D1::RectF(12.f * scale, row, float(w) - 12.f * scale, float(h) - 2.f * scale),
                         brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        brush->SetColor(ToolbarColor(overlay_theme::Border));
        rt->DrawRoundedRectangle(panel, brush.Get(), stroke);
        CheckHR(rt->EndDraw(), "overlay");
        POINT position{wndRect.left, wndRect.top}, origin{};
        SIZE size{w, h};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(toolbar_, nullptr, &position, &size, dc, &origin, 0, &blend, ULW_ALPHA);
    } catch (...) {
    }
    SelectObject(dc, previous);
    DeleteObject(bmp);
    DeleteDC(dc);
}
int Overlay::ToolbarHitTest(POINT clientPt) const {
    if (!toolbar_ || (!toolbarStandby_ && windows_.empty()))
        return -1;
    RECT r{};
    GetClientRect(toolbar_, &r);
    const int dpi = ActiveToolbarDpi();
    const int rowH = MulDiv(ActionRow, dpi, 96);
    if (clientPt.y < 0 || clientPt.y >= rowH || clientPt.x < 0 || clientPt.x >= Width(r))
        return -1;
    const auto geom = ComputeToolbarGeometry(Width(r), dpi);
    for (int id = 0; id < TotalItemCount; ++id) {
        if (!ToolbarActionEnabled(id))
            continue;
        const auto& box = geom.items[size_t(id)];
        if (float(clientPt.x) >= box.left && float(clientPt.x) <= box.right)
            return id;
    }
    return -1;
}
void Overlay::ShowLanguageMenu(bool sourceMenu) {
    if (!toolbar_ || windows_.empty())
        return;
    auto& data = *windows_.front();
    RECT wndRect{};
    GetWindowRect(toolbar_, &wndRect);
    const auto geom = ComputeToolbarGeometry(Width(wndRect), data.dpi);
    const auto& pill = geom.items[size_t(sourceMenu ? SourceLangAction : TargetLangAction)];
    const int anchorX = wndRect.left + int(std::lround(pill.left));
    const bool above = wndRect.bottom + 240 > data.block.workArea.bottom;
    const int anchorY = above ? wndRect.top : wndRect.bottom;
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    if (sourceMenu) {
        // 外面截图翻译仅提供本地 OCR 引擎实际支持的语种，避免与引擎能力脱节。
        for (size_t i = 0; i < kOcrSelectionModes.size(); ++i) {
            const int m = kOcrSelectionModes[i];
            const UINT flags = MF_STRING | (data.settings.ocrMode == m ? MF_CHECKED : MF_UNCHECKED);
            AppendMenuW(menu, flags, UINT_PTR(100 + i), OcrLanguageMenuLabel(m));
        }
    } else {
        const auto& targets = TextTargetLanguages();
        const std::string& activeId = data.settings.screenTargetLanguage;
        constexpr const char* kCommonIds[]{"zh-Hans", "zh-Hant", "en", "ja", "ko", "ru", "fr", "de", "es"};
        for (const char* cid : kCommonIds) {
            for (size_t i = 0; i < targets.size(); ++i) {
                if (std::string_view(targets[i].id) == cid) {
                    const UINT flags = MF_STRING | (activeId == targets[i].id ? MF_CHECKED : MF_UNCHECKED);
                    AppendMenuW(menu, flags, UINT_PTR(200 + i), targets[i].label);
                    break;
                }
            }
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        struct GroupRange {
            const wchar_t* title;
            char first;
            char last;
        };
        constexpr GroupRange kGroups[]{{L"A - F", 'A', 'F'},
                                       {L"G - L", 'G', 'L'},
                                       {L"M - R", 'M', 'R'},
                                       {L"S - Z", 'S', 'Z'}};
        for (const auto& grp : kGroups) {
            HMENU sub = CreatePopupMenu();
            if (!sub)
                continue;
            for (size_t i = 0; i < targets.size(); ++i) {
                if (targets[i].group >= grp.first && targets[i].group <= grp.last) {
                    const UINT flags = MF_STRING | (activeId == targets[i].id ? MF_CHECKED : MF_UNCHECKED);
                    AppendMenuW(sub, flags, UINT_PTR(200 + i), targets[i].label);
                }
            }
            AppendMenuW(menu, MF_POPUP, UINT_PTR(sub), grp.title);
        }
    }
    SetForegroundWindow(toolbar_);
    const UINT cmd = TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | (above ? TPM_BOTTOMALIGN : TPM_TOPALIGN),
        anchorX, anchorY, toolbar_, nullptr);
    DestroyMenu(menu);
    PostMessageW(toolbar_, WM_NULL, 0, 0);
    RaiseOverlayWindows();
    if (!cmd || !languageChanged_)
        return;
    if (sourceMenu && cmd >= 100 && cmd < 100 + UINT(kOcrSelectionModes.size())) {
        const int nextMode = kOcrSelectionModes[size_t(cmd - 100)];
        if (nextMode != data.settings.ocrMode) {
            auto cb = languageChanged_;
            cb(nextMode, data.settings.screenTargetLanguage);
        }
    } else if (!sourceMenu && cmd >= 200 && cmd < 200 + UINT(TextTargetLanguages().size())) {
        const std::string chosenId = TextTargetLanguages()[size_t(cmd - 200)].id;
        if (chosenId != data.settings.screenTargetLanguage) {
            auto cb = languageChanged_;
            cb(data.settings.ocrMode, chosenId);
        }
    }
}
void Overlay::ToolbarAction(int action) {
    if (!ToolbarActionEnabled(action))
        return;
    if (action == CloseAction) {
        auto cb = dismissed_;
        if (cb)
            cb();
        return;
    }
    if (action == ReselectAction) {
        auto cb = reselect_;
        if (cb)
            cb();
        return;
    }
    if (windows_.empty())
        return;
    auto& data = *windows_.front();
    if (data.drag)
        CancelDrag(data);
    if (action == SourceLangAction) {
        ShowLanguageMenu(true);
        return;
    }
    if (action == TargetLangAction) {
        ShowLanguageMenu(false);
        return;
    }
    if (action >= 0 && action <= 3) {
        if (action == 3 && !data.ai) {
            auto showOriginalWhilePending = [&] {
                if (data.mode != 1 || data.scroll != 0) {
                    data.mode = 1;
                    data.scroll = 0;
                    Render(data);
                }
            };
            if (aiPending_) {
                aiHighlight_ = true;
                warning_.clear();
                showOriginalWhilePending();
                RefreshToolbar();
                return;
            }
            // The vision pass runs once per click. The controller rebuilds this view when it returns and
            // the view then switches like any other, without touching the saved settings.
            if (!aiRequest_) {
                warning_ = L"当前连接无法读取图片";
                RefreshToolbar();
                return;
            }
            aiPending_ = true;
            aiHighlight_ = true;
            warning_.clear();
            showOriginalWhilePending();
            RefreshToolbar();
            auto cb = aiRequest_;
            cb();
            return;
        }
        aiHighlight_ = false;
        const int sourceMode = (data.mode == 3 && data.ai) ? 3 : 0;
        const auto& sourceBlock = (sourceMode == 3 && data.ai) ? *data.ai : data.block;
        if (action == 0 && data.block.positioned.empty() && data.block.sourceResult && data.block.image) {
            data.block.positioned =
                LayoutPositioned(*data.block.sourceResult, *data.block.image, data.settings, data.dpi);
        }
        if (action == 2 && (!data.reading || data.readingSourceMode != sourceMode)) {
            auto rect = data.block.screen;
            rect.right = rect.left + std::max(Width(rect), MulDiv(560, data.dpi, 96));
            rect.bottom = rect.top + std::max(Height(rect), MulDiv(420, data.dpi, 96));
            auto readingWork = data.block.workArea;
            readingWork.bottom -= std::min(MulDiv(76, data.dpi, 96), Height(readingWork) / 4);
            rect = Fit(rect, readingWork);
            auto options = data.settings;
            options.spatialOverlay = false;
            options.outlineWidth = 0;
            options.textColor = overlay_theme::TextPrimary;
            options.autoFont = false;
            options.fontSize = data.settings.autoFont ? 18 : data.settings.fontSize;
            Image paper{1, 1,
                        {static_cast<unsigned char>((overlay_theme::Surface >> 16) & 0xFF),
                         static_cast<unsigned char>((overlay_theme::Surface >> 8) & 0xFF),
                         static_cast<unsigned char>(overlay_theme::Surface & 0xFF), 255}};
            // Rebuild the document from structured segments so short headings keep their own style; the
            // concatenated fallback covers legacy callers without stored segments.
            TranslationResult result;
            if (sourceBlock.sourceResult) {
                result = *sourceBlock.sourceResult;
            } else {
                result.segments.push_back({"", Utf8(sourceBlock.text), {0, 0, 1000, 1000}});
            }
            Monitor monitor{L"reading", data.block.workArea, data.dpi};
            data.reading = BuildOverlay(result, paper, rect, monitor, options, false).front();
            data.readingSourceMode = sourceMode;
        }
        data.mode = action;
        data.scroll = 0;
        warning_.clear();
        Render(data);
    }
    if (action == 4) {
        const auto& text = Active(data).text;
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory) {
            warning_ = L"复制失败，请重试";
        } else {
            auto target = GlobalLock(memory);
            bool copied = false;
            if (target) {
                memcpy(target, text.c_str(), bytes);
                GlobalUnlock(memory);
                if (OpenClipboard(toolbar_)) {
                    if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory))
                        copied = true;
                    CloseClipboard();
                }
            }
            if (!copied) {
                GlobalFree(memory);
                warning_ = L"剪贴板忙，请重试";
            } else {
                warning_.clear();
            }
        }
    }
    RefreshToolbar();
}
void Overlay::SetWarning(const std::wstring& text) {
    aiPending_ = false;
    aiHighlight_ = false;
    warning_ = text;
    RefreshToolbar();
}
void Overlay::ShowAiResult(OverlayBlock block) {
    if (windows_.empty())
        return;
    auto& data = *windows_.front();
    // The one-off result joins the recognized result as another view. Both stay in memory, so switching
    // between them costs nothing and the settings keep describing the default flow.
    aiPending_ = false;
    aiHighlight_ = false;
    warning_.clear();
    data.ai = std::move(block);
    data.reading.reset();
    data.readingSourceMode = -1;
    data.mode = 3;
    data.scroll = 0;
    Render(data);
    RefreshToolbar();
}
void Overlay::RaiseOverlayWindows() {
    for (auto& window : windows_)
        if (window->hwnd)
            SetWindowPos(window->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (toolbar_)
        SetWindowPos(toolbar_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
void Overlay::UpdateShadeHole() {
    if (!focusShade_ || windows_.empty())
        return;
    RECT screen{};
    GetWindowRect(focusShade_, &screen);
    const int width = Width(screen), height = Height(screen);
    if (width <= 0 || height <= 0)
        return;
    // Cut exact holes only for sharp-edged frames or transparent drag previews so no border gap exposes
    // the desktop. Opaque rounded overlays (toolbar and reading card) stay above the shade, leaving
    // the shade behind their transparent outer corners to block clicks.
    HRGN region = CreateRectRgn(0, 0, width, height);
    for (const auto& window : windows_) {
        if (window->progressFrame)
            continue;
        const auto& active = Active(*window);
        const bool sharpRect = active.image && window->mode != 2;
        if (!sharpRect && !window->dragPreview && active.opacity >= 1.f)
            continue;
        RECT hole = active.screen;
        OffsetRect(&hole, -screen.left, -screen.top);
        HRGN cut = CreateRectRgn(hole.left, hole.top, hole.right, hole.bottom);
        CombineRgn(region, region, cut, RGN_DIFF);
        DeleteObject(cut);
    }
    SetWindowRgn(focusShade_, region, TRUE);
}
void Overlay::ShowFocusShade() {
    if (windows_.empty() || focusShade_)
        return;
    auto& data = *windows_.front();
    const RECT area = data.block.monitorArea;
    if (!Valid(area))
        return;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SAT.FocusShade";
    wc.lpfnWndProc = ShadeProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    focusShade_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName,
                                  L"聚焦遮罩", WS_POPUP, area.left, area.top, Width(area), Height(area),
                                  nullptr, nullptr, wc.hInstance, this);
    if (!focusShade_)
        return;
    // 55% black over the desktop, with the selection and the toolbar kept above the shade.
    SetLayeredWindowAttributes(focusShade_, 0, 140, LWA_ALPHA);
    UpdateShadeHole();
    ShowWindow(focusShade_, SW_SHOW);
    RaiseOverlayWindows();
    SetForegroundWindow(focusShade_);
    SetFocus(focusShade_);
}
LRESULT CALLBACK Overlay::ShadeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        RECT r{};
        GetClientRect(h, &r);
        auto black = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &r, black);
        DeleteObject(black);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE && self) {
            auto cb = self->dismissed_;
            if (cb)
                cb();
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE && self)
            self->RaiseOverlayWindows();
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        // The dimmed area keeps the input, so the desktop behind the translation stays still.
        SetFocus(h);
        if (self)
            self->RaiseOverlayWindows();
        return 0;
    case WM_NCDESTROY:
        if (self)
            self->focusShade_ = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK Overlay::ToolbarProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(h, m, w, l);
    try {
        switch (m) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ValidateRect(h, nullptr);
            self->PaintToolbar();
            return 0;
        case WM_MOUSEMOVE: {
            if (!self->toolbarStandby_ && self->windows_.empty())
                return 0;
            const int action = self->ToolbarHitTest({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            if (action != self->hoverAction_) {
                self->hoverAction_ = action;
                self->PaintToolbar();
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (self->hoverAction_ != -1) {
                self->hoverAction_ = -1;
                self->PaintToolbar();
            }
            return 0;
        case WM_LBUTTONDOWN: {
            if (!self->toolbarStandby_ && self->windows_.empty())
                return 0;
            const int action = self->ToolbarHitTest({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            if (action >= 0) {
                self->pressedAction_ = action;
                SetCapture(h);
                // The pressed cell darkens immediately, so a click that lands shows before the action runs.
                self->PaintToolbar();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!self->toolbarStandby_ && self->windows_.empty()) {
                self->pressedAction_ = -1;
                if (GetCapture() == h)
                    ReleaseCapture();
                return 0;
            }
            const int action = self->ToolbarHitTest({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            const bool run = action >= 0 && action == self->pressedAction_;
            self->pressedAction_ = -1;
            if (GetCapture() == h)
                ReleaseCapture();
            self->PaintToolbar();
            if (run)
                self->ToolbarAction(action);
            return 0;
        }
        case WM_COMMAND:
            self->ToolbarAction(LOWORD(w));
            return 0;
        case WM_CAPTURECHANGED:
            self->pressedAction_ = -1;
            self->PaintToolbar();
            return 0;
        case WM_CLOSE: {
            auto cb = self->dismissed_;
            if (cb)
                cb();
            return 0;
        }
        case WM_NCDESTROY:
            self->toolbar_ = nullptr;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return 0;
        }
    } catch (...) {
        self->aiPending_ = false;
        self->aiHighlight_ = false;
        self->warning_ = L"操作未完成，请重试";
        self->RefreshToolbar();
    }
    return DefWindowProcW(h, m, w, l);
}
void Overlay::ShowContinuousBadge(RECT anchor, const Monitor& monitor, const std::wstring& text,
                                  std::function<void()> dismissed, const Settings& settings,
                                  std::function<void()> reselect) {
    toolbarStandby_ = true;
    toolbarBusy_ = true;
    toolbarAnchor_ = anchor;
    toolbarWorkArea_ = WorkAreaForAnchor(anchor, monitor);
    toolbarDpi_ = monitor.dpi;
    toolbarSettings_ = settings;
    noticeActive_ = true;
    noticeText_ = text;
    passthrough_ = true;
    dismissed_ = std::move(dismissed);
    if (reselect)
        reselect_ = std::move(reselect);
    hoverAction_ = -1;
    pressedAction_ = -1;
    ShowToolbar();
}
void Overlay::CloseContinuousBadge() {
    if (!toolbarStandby_)
        return;
    toolbarStandby_ = false;
    toolbarBusy_ = false;
    noticeActive_ = false;
    noticeText_.clear();
    toolbarAnchor_ = {};
    toolbarWorkArea_ = {};
    if (toolbar_) {
        DestroyWindow(toolbar_);
        toolbar_ = nullptr;
    }
}
bool Overlay::IsControlPoint(POINT screenPt) const {
    auto hits = [screenPt](HWND hwnd) {
        if (!hwnd || !IsWindowVisible(hwnd))
            return false;
        RECT r{};
        return GetWindowRect(hwnd, &r) && PtInRect(&r, screenPt) != FALSE;
    };
    if (hits(toolbar_))
        return true;
    for (const auto& w : windows_) {
        if (!w->progressFrame && !(GetWindowLongPtrW(w->hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) &&
            hits(w->hwnd))
            return true;
    }
    return false;
}
} // namespace sat
