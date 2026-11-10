#include "Overlay.h"
#include <algorithm>
#include <cmath>
#include "SpatialLayout.h"
#include <windowsx.h>
namespace sat {
namespace {
// The first four cells switch views, the next two run actions, and closing keeps the last cell.
constexpr int ActionCount = 7;
constexpr int ReselectAction = 5;
constexpr int CloseAction = 6;
constexpr int ToolbarWidth = 660;
constexpr int ToolbarHeight = 64;
constexpr int ActionRow = 38;
const wchar_t* const Labels[]{L"原位译文", L"查看原图", L"全文阅读", L"AI识别翻译",
                              L"复制译文", L"重新框选", L"关闭"};
// A light strip stays legible over both dark manga panels and pale pages, and the one deep cell marks
// closing as the single exit.
constexpr COLORREF SurfaceColor = RGB(244, 246, 250);
constexpr COLORREF LabelColor = RGB(42, 49, 64);
constexpr COLORREF ActiveLabelColor = RGB(42, 91, 215);
constexpr COLORREF ActiveFillColor = RGB(220, 230, 251);
constexpr COLORREF ActiveHeldFillColor = RGB(185, 205, 246);
constexpr COLORREF HoverFillColor = RGB(232, 237, 246);
constexpr COLORREF HeldFillColor = RGB(203, 215, 238);
constexpr COLORREF HeldLabelColor = RGB(26, 61, 148);
constexpr COLORREF CloseFillColor = RGB(31, 36, 48);
constexpr COLORREF CloseHoverColor = RGB(48, 56, 74);
constexpr COLORREF CloseHeldColor = RGB(70, 82, 108);
constexpr COLORREF CloseLabelColor = RGB(255, 255, 255);
constexpr COLORREF StatusColor = RGB(92, 100, 114);
RECT Fit(RECT r, RECT work) {
    int w = std::min(Width(r), Width(work)), h = std::min(Height(r), Height(work));
    r.left = std::clamp(r.left, work.left, work.right - w);
    r.top = std::clamp(r.top, work.top, work.bottom - h);
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}
} // namespace
void Overlay::ShowToolbar() {
    auto& data = *windows_.front();
    float scale = data.dpi / 96.f;
    auto r = data.block.screen;
    int w = int(ToolbarWidth * scale), h = int(ToolbarHeight * scale), gap = int(8 * scale);
    RECT bounds{r.left, r.bottom + gap, r.left + w, r.bottom + gap + h};
    if (bounds.bottom > data.block.workArea.bottom) {
        bounds.top = r.top - gap - h;
        bounds.bottom = r.top - gap;
    }
    bounds = Fit(bounds, data.block.workArea);
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SAT.TranslationToolbar";
    wc.lpfnWndProc = ToolbarProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    toolbarStatus_ = L"原位覆盖 · 蓝色标记处滚轮阅读，全文阅读可查看完整译文";
    if (std::none_of(data.block.positioned.begin(), data.block.positioned.end(),
                     [](const PositionedText& p) { return !p.detectedLines.empty(); }))
        toolbarStatus_ = L"AI 定位 · 如有错位，请使用全文阅读查看译文";
    if (std::any_of(data.block.positioned.begin(), data.block.positioned.end(),
                    [](const PositionedText& p) { return p.suppressed; }))
        toolbarStatus_ = L"部分区域定位存在冲突，已保留原图；请使用全文阅读查看译文";
    if (data.mode == 2)
        toolbarStatus_ = L"全文译文 · 滚轮阅读，可复制译文或切换原图";
    toolbar_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                               L"翻译操作", WS_POPUP, bounds.left, bounds.top, Width(bounds),
                               Height(bounds), data.hwnd, nullptr, wc.hInstance, this);
    if (!toolbar_)
        throw AppError("overlay", "无法创建翻译工具条");
    ShowWindow(toolbar_, SW_SHOWNOACTIVATE);
}
void Overlay::PositionToolbar() {
    if (!toolbar_ || windows_.empty())
        return;
    auto& data = *windows_.front();
    auto r = data.mode == 2 && data.reading ? data.reading->screen : data.block.screen;
    int width = MulDiv(ToolbarWidth, data.dpi, 96), height = MulDiv(ToolbarHeight, data.dpi, 96),
        gap = MulDiv(8, data.dpi, 96);
    RECT bounds{r.left, r.bottom + gap, r.left + width, r.bottom + gap + height};
    if (bounds.bottom > data.block.workArea.bottom) {
        bounds.top = r.top - gap - height;
        bounds.bottom = r.top - gap;
    }
    bounds = Fit(bounds, data.block.workArea);
    SetWindowPos(toolbar_, HWND_TOPMOST, bounds.left, bounds.top, Width(bounds), Height(bounds),
                 SWP_NOACTIVATE);
}
void Overlay::PaintToolbar() {
    PAINTSTRUCT ps{};
    auto dc = BeginPaint(toolbar_, &ps);
    RECT r{};
    GetClientRect(toolbar_, &r);
    auto surface = CreateSolidBrush(SurfaceColor);
    FillRect(dc, &r, surface);
    DeleteObject(surface);
    int dpi = windows_.front()->dpi;
    auto font = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0,
                            0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    int row = MulDiv(ActionRow, dpi, 96);
    const int mode = windows_.front()->mode;
    const int pressed = pressedAction_;
    const int hovered = hoverAction_;
    // Straight corners: the strip reads as one bar, and the active cell and the closing cell are plain
    // blocks rather than rounded pills.
    const auto fillCell = [&](RECT cell, COLORREF color) {
        InflateRect(&cell, -MulDiv(8, dpi, 96), -MulDiv(7, dpi, 96));
        auto fill = CreateSolidBrush(color);
        FillRect(dc, &cell, fill);
        DeleteObject(fill);
    };
    for (int i = 0; i < ActionCount; ++i) {
        RECT cell{i * Width(r) / ActionCount, 0, (i + 1) * Width(r) / ActionCount, row};
        const bool held = i == pressed, hover = i == hovered;
        if (i == CloseAction) {
            RECT pill = cell;
            fillCell(pill, held ? CloseHeldColor : hover ? CloseHoverColor : CloseFillColor);
            InflateRect(&pill, -MulDiv(8, dpi, 96), -MulDiv(7, dpi, 96));
            SetTextColor(dc, CloseLabelColor);
            DrawTextW(dc, Labels[i], -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            continue;
        }
        const bool active = i < 4 && i == mode;
        if (active) {
            fillCell(cell, held ? ActiveHeldFillColor : ActiveFillColor);
            SetTextColor(dc, ActiveLabelColor);
        } else if (held) {
            fillCell(cell, HeldFillColor);
            SetTextColor(dc, HeldLabelColor);
        } else if (hover) {
            fillCell(cell, HoverFillColor);
            SetTextColor(dc, LabelColor);
        } else {
            SetTextColor(dc, LabelColor);
        }
        DrawTextW(dc, Labels[i], -1, &cell, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    RECT status{MulDiv(10, dpi, 96), row, Width(r) - MulDiv(8, dpi, 96), Height(r)};
    SetTextColor(dc, StatusColor);
    DrawTextW(dc, (warning_.empty() ? toolbarStatus_ : warning_).c_str(), -1, &status,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteObject(font);
    EndPaint(toolbar_, &ps);
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
        else
            toolbarStatus_ = L"请使用截图快捷键重新框选";
    }
    if (action >= 0 && action <= 3) {
        if (action == 3 && !data.ai) {
            // The vision pass runs once per click. The controller rebuilds this view when it returns and
            // the view then switches like any other, without touching the saved settings.
            if (!aiRequest_) {
                toolbarStatus_ = L"当前连接无法读取图片";
                if (toolbar_)
                    InvalidateRect(toolbar_, nullptr, FALSE);
                return;
            }
            toolbarStatus_ = L"翻译中";
            if (toolbar_)
                InvalidateRect(toolbar_, nullptr, FALSE);
            auto cb = aiRequest_;
            cb();
            return;
        }
        const auto& active = Active(data);
        if (action == 0 && active.positioned.empty() && active.sourceResult && active.image) {
            data.block.positioned =
                LayoutPositioned(*active.sourceResult, *active.image, data.settings, data.dpi);
        }
        if (action == 2 && !data.reading) {
            auto rect = data.block.screen;
            rect.right = rect.left + std::max(Width(rect), MulDiv(560, data.dpi, 96));
            rect.bottom = rect.top + std::max(Height(rect), MulDiv(420, data.dpi, 96));
            auto readingWork = data.block.workArea;
            readingWork.bottom -= std::min(MulDiv(76, data.dpi, 96), Height(readingWork) / 4);
            rect = Fit(rect, readingWork);
            auto options = data.settings;
            options.spatialOverlay = false;
            options.outlineWidth = 0;
            options.textColor = RGB(29, 39, 57);
            options.autoFont = false;
            options.fontSize = data.settings.autoFont ? 18 : data.settings.fontSize;
            Image paper{1, 1, {253, 250, 248, 255}};
            // Rebuild the document from structured segments so short headings keep their own style; the
            // concatenated fallback covers legacy callers without stored segments.
            TranslationResult result;
            if (active.sourceResult) {
                result = *active.sourceResult;
            } else {
                result.segments.push_back({"", Utf8(active.text), {0, 0, 1000, 1000}});
            }
            Monitor monitor{L"reading", data.block.workArea, data.dpi};
            data.reading = BuildOverlay(result, paper, rect, monitor, options, false).front();
        }
        data.mode = action;
        data.scroll = 0;
        Render(data);
        PositionToolbar();
        UpdateShadeHole();
        toolbarStatus_ = action == 1   ? L"原始截图 · 拖动边框松手重译"
                         : action == 2 ? L"全文译文 · 滚轮阅读，拖边框按原选区大小重译"
                         : action == 3 ? L"AI 识别译文 · 拖动边框松手重译"
                                       : L"原位覆盖 · 拖动边框松手重译，蓝色标记处滚轮阅读";
        if (action == 0) {
            if (std::none_of(data.block.positioned.begin(), data.block.positioned.end(),
                             [](const PositionedText& p) { return !p.detectedLines.empty(); }))
                toolbarStatus_ = L"AI 定位 · 如有错位，请使用全文阅读查看译文";
            if (std::any_of(data.block.positioned.begin(), data.block.positioned.end(),
                            [](const PositionedText& p) { return p.suppressed; }))
                toolbarStatus_ = L"部分区域定位存在冲突，已保留原图；请使用全文阅读查看译文";
        }
    }
    if (action == 4) {
        const auto& text = Active(data).text;
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory) {
            toolbarStatus_ = L"复制失败，请重试";
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
            if (!copied)
                GlobalFree(memory);
            toolbarStatus_ = copied ? L"已复制完整译文" : L"剪贴板忙，请重试";
        }
    }
    if (toolbar_)
        InvalidateRect(toolbar_, nullptr, FALSE);
}
void Overlay::SetWarning(const std::wstring& text) {
    warning_ = text;
    if (toolbar_)
        InvalidateRect(toolbar_, nullptr, FALSE);
}
void Overlay::ShowAiResult(OverlayBlock block) {
    if (windows_.empty())
        return;
    auto& data = *windows_.front();
    // The one-off result joins the recognized result as another view. Both stay in memory, so switching
    // between them costs nothing and the settings keep describing the default flow.
    data.ai = std::move(block);
    data.mode = 3;
    data.scroll = 0;
    Render(data);
    PositionToolbar();
    UpdateShadeHole();
    toolbarStatus_ = L"AI 识别译文 · 拖动边框松手重译";
    if (toolbar_)
        InvalidateRect(toolbar_, nullptr, FALSE);
}
void Overlay::UpdateShadeHole() {
    if (!focusShade_ || windows_.empty())
        return;
    RECT screen{};
    GetWindowRect(focusShade_, &screen);
    const int width = Width(screen), height = Height(screen);
    if (width <= 0 || height <= 0)
        return;
    const auto& data = *windows_.front();
    std::vector<RECT> holes;
    holes.push_back(data.mode == 2 && data.reading ? data.reading->screen : data.block.screen);
    if (toolbar_ && IsWindowVisible(toolbar_)) {
        RECT bar{};
        GetWindowRect(toolbar_, &bar);
        holes.push_back(bar);
    }
    HRGN region = CreateRectRgn(0, 0, width, height);
    for (auto hole : holes) {
        OffsetRect(&hole, -screen.left, -screen.top);
        InflateRect(&hole, 2, 2);
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
    // 55% black over the desktop, with the selection and the toolbar cut out of the window region.
    SetLayeredWindowAttributes(focusShade_, 0, 140, LWA_ALPHA);
    UpdateShadeHole();
    ShowWindow(focusShade_, SW_SHOW);
    // The translation windows stay above the shade, so the replacement and its toolbar remain usable.
    for (auto& window : windows_)
        if (window->hwnd)
            SetWindowPos(window->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (toolbar_)
        SetWindowPos(toolbar_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        // The dimmed area keeps the input, so the desktop behind the translation stays still.
        SetFocus(h);
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
            self->PaintToolbar();
            return 0;
        case WM_MOUSEMOVE: {
            RECT r{};
            GetClientRect(h, &r);
            const int action = GET_Y_LPARAM(l) >= 0 &&
                                       GET_Y_LPARAM(l) < MulDiv(ActionRow, self->windows_.front()->dpi, 96)
                                   ? GET_X_LPARAM(l) * ActionCount / std::max(1, Width(r))
                                   : -1;
            if (action != self->hoverAction_) {
                self->hoverAction_ = action;
                InvalidateRect(h, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (self->hoverAction_ != -1) {
                self->hoverAction_ = -1;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            RECT r{};
            GetClientRect(h, &r);
            if (GET_Y_LPARAM(l) >= 0 && GET_Y_LPARAM(l) < MulDiv(ActionRow, self->windows_.front()->dpi, 96)) {
                self->pressedAction_ = GET_X_LPARAM(l) * ActionCount / std::max(1, Width(r));
                SetCapture(h);
                // The pressed cell darkens immediately, so a click that lands shows before the action runs.
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            RECT r{};
            GetClientRect(h, &r);
            int action = GET_X_LPARAM(l) * ActionCount / std::max(1, Width(r));
            bool run = GET_X_LPARAM(l) >= 0 && GET_X_LPARAM(l) < Width(r) && GET_Y_LPARAM(l) >= 0 &&
                       GET_Y_LPARAM(l) < MulDiv(ActionRow, self->windows_.front()->dpi, 96) &&
                       action == self->pressedAction_;
            self->pressedAction_ = -1;
            if (GetCapture() == h)
                ReleaseCapture();
            InvalidateRect(h, nullptr, FALSE);
            if (run)
                self->ToolbarAction(action);
            return 0;
        }
        case WM_COMMAND:
            self->ToolbarAction(LOWORD(w));
            return 0;
        case WM_CAPTURECHANGED:
            self->pressedAction_ = -1;
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
        self->toolbarStatus_ = L"操作未完成，请重试";
        InvalidateRect(h, nullptr, FALSE);
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace sat
