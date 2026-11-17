#include "Overlay.h"
#include <algorithm>
#include <cmath>
#include <d2d1.h>
#include "SpatialLayout.h"
#include <windowsx.h>
namespace sat {
namespace {
// The first four cells switch views, the next two run actions, and closing keeps the last cell.
constexpr int ActionCount = 7;
constexpr int ReselectAction = 5;
constexpr int CloseAction = 6;
constexpr int ToolbarWidth = 680;
constexpr int ToolbarHeight = 68;
constexpr int ActionRow = 40;
constexpr int BadgeWidth = 290;
constexpr int BadgeHeight = 36;
constexpr DWORD ExcludeFromCapture = 0x00000011;
const wchar_t* const Labels[]{L"原位译文", L"查看原图", L"全文阅读", L"AI识图翻译",
                              L"复制译文", L"重新框选", L"关闭"};
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
RECT PlaceBarBounds(RECT anchor, RECT workArea, int width, int height, int gap) {
    RECT bounds{anchor.left, anchor.bottom + gap, anchor.left + width, anchor.bottom + gap + height};
    if (bounds.bottom > workArea.bottom) {
        bounds.top = anchor.top - gap - height;
        bounds.bottom = anchor.top - gap;
    }
    return Fit(bounds, workArea);
}
} // namespace
std::wstring Overlay::ToolbarStatusText() const {
    if (!warning_.empty())
        return warning_;
    if (aiPending_)
        return L"AI识图翻译中 · 请稍候...";
    return {};
}
void Overlay::ShowToolbar() {
    auto& data = *windows_.front();
    const int logicalHeight = ToolbarStatusText().empty() ? ActionRow : ToolbarHeight;
    const RECT bounds = PlaceBarBounds(data.block.screen, data.block.workArea,
                                       MulDiv(ToolbarWidth, data.dpi, 96),
                                       MulDiv(logicalHeight, data.dpi, 96), MulDiv(8, data.dpi, 96));
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SAT.TranslationToolbar";
    wc.lpfnWndProc = ToolbarProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    toolbar_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               wc.lpszClassName, L"翻译操作", WS_POPUP, bounds.left, bounds.top,
                               Width(bounds), Height(bounds), data.hwnd, nullptr, wc.hInstance, this);
    if (!toolbar_)
        throw AppError("overlay", "无法创建翻译工具条");
    if (passthrough_)
        SetWindowDisplayAffinity(toolbar_, ExcludeFromCapture);
    PaintToolbar();
    ShowWindow(toolbar_, SW_SHOWNOACTIVATE);
}
void Overlay::PositionToolbar() {
    if (!toolbar_ || windows_.empty())
        return;
    auto& data = *windows_.front();
    auto r = data.mode == 2 && data.reading ? data.reading->screen : data.block.screen;
    const int logicalHeight = ToolbarStatusText().empty() ? ActionRow : ToolbarHeight;
    const RECT bounds = PlaceBarBounds(r, data.block.workArea, MulDiv(ToolbarWidth, data.dpi, 96),
                                       MulDiv(logicalHeight, data.dpi, 96), MulDiv(8, data.dpi, 96));
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
    if (!toolbar_ || windows_.empty())
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
        const int dpi = windows_.front()->dpi;
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
        const int mode = windows_.front()->mode;
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
        const float padX = 4.f * scale, padY = 5.f * scale, pillRadius = 6.f * scale;
        for (int i = 0; i < ActionCount; ++i) {
            const float cellLeft = float(i * w / ActionCount);
            const float cellRight = float((i + 1) * w / ActionCount);
            const D2D1_RECT_F pillRect = D2D1::RectF(cellLeft + padX, padY, cellRight - padX, row - padY);
            const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(pillRect, pillRadius, pillRadius);
            const bool held = i == pressed, hover = i == hovered;
            COLORREF textColor = overlay_theme::TextPrimary;
            if (i == CloseAction) {
                const COLORREF fill = held  ? overlay_theme::CloseHeld
                                      : hover ? overlay_theme::CloseHover
                                              : overlay_theme::CloseRest;
                brush->SetColor(ToolbarColor(fill));
                rt->FillRoundedRectangle(pill, brush.Get());
            } else if (i < 4 && i == highlighted) {
                brush->SetColor(ToolbarColor(held ? overlay_theme::AccentHeld : overlay_theme::AccentFill));
                rt->FillRoundedRectangle(pill, brush.Get());
                brush->SetColor(ToolbarColor(overlay_theme::Accent));
                rt->DrawRoundedRectangle(pill, brush.Get(), std::max(1.f, scale));
                textColor = overlay_theme::AccentLabel;
            } else if (held || hover) {
                brush->SetColor(ToolbarColor(held ? overlay_theme::Pressed : overlay_theme::Hover));
                rt->FillRoundedRectangle(pill, brush.Get());
            }
            brush->SetColor(ToolbarColor(textColor));
            rt->DrawText(Labels[i], static_cast<UINT32>(wcslen(Labels[i])), buttonFormat.Get(), pillRect,
                         brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        const std::wstring statusText = ToolbarStatusText();
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
void Overlay::ToolbarAction(int action) {
    if (windows_.empty())
        return;
    auto& data = *windows_.front();
    if (data.drag)
        CancelDrag(data);
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
            if (self->windows_.empty())
                return 0;
            RECT r{};
            GetClientRect(h, &r);
            const int action = GET_Y_LPARAM(l) >= 0 &&
                                       GET_Y_LPARAM(l) < MulDiv(ActionRow, self->windows_.front()->dpi, 96)
                                   ? GET_X_LPARAM(l) * ActionCount / std::max(1, Width(r))
                                   : -1;
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
            if (self->windows_.empty())
                return 0;
            RECT r{};
            GetClientRect(h, &r);
            if (GET_Y_LPARAM(l) >= 0 && GET_Y_LPARAM(l) < MulDiv(ActionRow, self->windows_.front()->dpi, 96)) {
                self->pressedAction_ = GET_X_LPARAM(l) * ActionCount / std::max(1, Width(r));
                SetCapture(h);
                // The pressed cell darkens immediately, so a click that lands shows before the action runs.
                self->PaintToolbar();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (self->windows_.empty()) {
                self->pressedAction_ = -1;
                if (GetCapture() == h)
                    ReleaseCapture();
                return 0;
            }
            RECT r{};
            GetClientRect(h, &r);
            int action = GET_X_LPARAM(l) * ActionCount / std::max(1, Width(r));
            bool run = GET_X_LPARAM(l) >= 0 && GET_X_LPARAM(l) < Width(r) && GET_Y_LPARAM(l) >= 0 &&
                       GET_Y_LPARAM(l) < MulDiv(ActionRow, self->windows_.front()->dpi, 96) &&
                       action == self->pressedAction_;
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
void Overlay::PaintBadge() {
    if (!badge_)
        return;
    RECT wndRect{};
    GetWindowRect(badge_, &wndRect);
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
        const float scale = badgeDpi_ / 96.f;
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
        const float dotR = 3.5f * scale;
        brush->SetColor(ToolbarColor(overlay_theme::Accent));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(14.f * scale, float(h) * .5f), dotR, dotR), brush.Get());
        const int closeWidth = MulDiv(58, badgeDpi_, 96);
        const int closePad = MulDiv(5, badgeDpi_, 96);
        badgeCloseRect_ = {w - closeWidth - closePad, closePad, w - closePad, h - closePad};
        const COLORREF closeFill = badgePressed_ ? overlay_theme::CloseHeld
                                   : badgeHover_ ? overlay_theme::CloseHover
                                                 : overlay_theme::CloseRest;
        const D2D1_RECT_F closeRf =
            D2D1::RectF(float(badgeCloseRect_.left), float(badgeCloseRect_.top),
                        float(badgeCloseRect_.right), float(badgeCloseRect_.bottom));
        brush->SetColor(ToolbarColor(closeFill));
        rt->FillRoundedRectangle(D2D1::RoundedRect(closeRf, 6.f * scale, 6.f * scale), brush.Get());
        ComPtr<IDWriteTextFormat> textFormat;
        CheckHR(writeFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                               12.5f * scale, L"zh-CN", &textFormat),
                "overlay");
        CheckHR(textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER), "overlay");
        CheckHR(textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "overlay");
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(writeFactory->CreateEllipsisTrimmingSign(textFormat.Get(), &ellipsis)))
            textFormat->SetTrimming(&trimming, ellipsis.Get());
        brush->SetColor(ToolbarColor(overlay_theme::TextPrimary));
        const D2D1_RECT_F labelRect =
            D2D1::RectF(24.f * scale, 0.f, float(badgeCloseRect_.left) - 8.f * scale, float(h));
        rt->DrawText(badgeText_.c_str(), static_cast<UINT32>(badgeText_.size()), textFormat.Get(), labelRect,
                     brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        CheckHR(textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER), "overlay");
        rt->DrawText(L"关闭", 2, textFormat.Get(), closeRf, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        brush->SetColor(ToolbarColor(overlay_theme::Border));
        rt->DrawRoundedRectangle(panel, brush.Get(), stroke);
        CheckHR(rt->EndDraw(), "overlay");
        POINT position{wndRect.left, wndRect.top}, origin{};
        SIZE size{w, h};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(badge_, nullptr, &position, &size, dc, &origin, 0, &blend, ULW_ALPHA);
    } catch (...) {
    }
    SelectObject(dc, previous);
    DeleteObject(bmp);
    DeleteDC(dc);
}
void Overlay::ShowContinuousBadge(RECT anchor, const Monitor& monitor, const std::wstring& text,
                                  std::function<void()> dismissed) {
    badgeText_ = text;
    badgeDpi_ = monitor.dpi;
    badgeClosed_ = std::move(dismissed);
    badgeHover_ = false;
    badgePressed_ = false;
    RECT workArea = monitor.rect;
    MONITORINFO info{sizeof(info)};
    if (auto handle = MonitorFromRect(&anchor, MONITOR_DEFAULTTONULL);
        handle && GetMonitorInfoW(handle, &info) && EqualRect(&info.rcMonitor, &monitor.rect))
        workArea = info.rcWork;
    const RECT bounds = PlaceBarBounds(anchor, workArea, MulDiv(BadgeWidth, badgeDpi_, 96),
                                       MulDiv(BadgeHeight, badgeDpi_, 96), MulDiv(8, badgeDpi_, 96));
    if (!badge_) {
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SAT.ContinuousBadge";
        wc.lpfnWndProc = BadgeProc;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        badge_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 wc.lpszClassName, L"连续翻译状态", WS_POPUP, bounds.left, bounds.top,
                                 Width(bounds), Height(bounds), nullptr, nullptr, wc.hInstance, this);
        if (!badge_)
            return;
        SetWindowDisplayAffinity(badge_, ExcludeFromCapture);
    } else {
        SetWindowPos(badge_, HWND_TOPMOST, bounds.left, bounds.top, Width(bounds), Height(bounds),
                     SWP_NOACTIVATE);
    }
    PaintBadge();
    ShowWindow(badge_, SW_SHOWNOACTIVATE);
}
void Overlay::CloseContinuousBadge() {
    badgeClosed_ = {};
    badgeHover_ = false;
    badgePressed_ = false;
    badgeText_.clear();
    if (badge_) {
        DestroyWindow(badge_);
        badge_ = nullptr;
    }
}
bool Overlay::IsControlPoint(POINT screenPt) const {
    auto hits = [screenPt](HWND hwnd) {
        if (!hwnd || !IsWindowVisible(hwnd))
            return false;
        RECT r{};
        return GetWindowRect(hwnd, &r) && PtInRect(&r, screenPt) != FALSE;
    };
    if (hits(toolbar_) || hits(badge_))
        return true;
    for (const auto& w : windows_) {
        if (!w->progressFrame && !(GetWindowLongPtrW(w->hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) &&
            hits(w->hwnd))
            return true;
    }
    return false;
}
LRESULT CALLBACK Overlay::BadgeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(h, nullptr);
        self->PaintBadge();
        return 0;
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const bool hover = PtInRect(&self->badgeCloseRect_, pt) != FALSE;
        if (hover != self->badgeHover_) {
            self->badgeHover_ = hover;
            self->PaintBadge();
        }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (self->badgeHover_) {
            self->badgeHover_ = false;
            self->PaintBadge();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (PtInRect(&self->badgeCloseRect_, pt)) {
            self->badgePressed_ = true;
            SetCapture(h);
            self->PaintBadge();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const bool run = self->badgePressed_ && PtInRect(&self->badgeCloseRect_, pt);
        self->badgePressed_ = false;
        if (GetCapture() == h)
            ReleaseCapture();
        self->PaintBadge();
        if (run && self->badgeClosed_)
            self->badgeClosed_();
        return 0;
    }
    case WM_CAPTURECHANGED:
        self->badgePressed_ = false;
        self->PaintBadge();
        return 0;
    case WM_NCDESTROY:
        self->badge_ = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace sat
