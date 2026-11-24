#pragma once
#include "common/Platform.h"
#include <dwrite.h>
namespace sat {
namespace overlay_theme {
inline constexpr COLORREF Surface = RGB(24, 28, 36);
inline constexpr COLORREF Header = RGB(30, 36, 48);
inline constexpr COLORREF Elevated = RGB(40, 47, 61);
inline constexpr COLORREF Hover = RGB(48, 56, 73);
inline constexpr COLORREF Pressed = RGB(58, 68, 88);
inline constexpr COLORREF Border = RGB(56, 66, 86);
inline constexpr COLORREF Accent = RGB(76, 141, 246);
inline constexpr COLORREF AccentFill = RGB(42, 82, 152);
inline constexpr COLORREF AccentHeld = RGB(54, 102, 184);
inline constexpr COLORREF AccentLabel = RGB(235, 243, 255);
inline constexpr COLORREF TextPrimary = RGB(238, 242, 248);
inline constexpr COLORREF TextSecondary = RGB(154, 164, 184);
inline constexpr COLORREF WarningText = RGB(245, 182, 92);
inline constexpr COLORREF CloseRest = RGB(44, 50, 64);
inline constexpr COLORREF CloseHover = RGB(178, 52, 64);
inline constexpr COLORREF CloseHeld = RGB(148, 40, 50);
} // namespace overlay_theme
struct EraseRow {
    RECT rect{};
    COLORREF start{}, end{};
    COLORREF top{}, bottom{};
    bool vertical{};
    RECT spread{};
    // Opaque rebuilt surface for this glyph row: BGRA pixels covering rect exactly. The artwork above and
    // below the row supplies each column, so the replacement meets the untouched gaps with matching colour.
    std::vector<unsigned char> cover{};
};
// A vertical passage sets one glyph per row, and a balloon wraps its sentence across neighbouring
// strips. Each strip keeps its own layout so the replacement reuses the source column positions
// instead of squeezing the whole sentence into a single strip.
struct ColumnText {
    RECT rect{};
    ComPtr<IDWriteTextLayout> layout;
};
struct PositionedText {
    RECT rect{}, sourceRect{};
    COLORREF background{};
    float fontSize{}, padding{}, contentHeight{}, lineHeight{}, eraseHeight{}, scroll{};
    // Ink band height the fitted face was derived from; diagnostics and tests read it.
    float bandHeight{};
    // Dense core height of the source line, the second input of the face estimate.
    float coreHeight{};
    bool overflow{};
    bool centerAligned{};
    bool bold{};
    float maxOverhang{24.f};
    float maxLeftOverhang{0.f};
    ComPtr<IDWriteTextLayout> layout;
    // One entry per source strip of a vertical passage; the first entry also fills layout.
    std::vector<ColumnText> columnTexts;
    bool suppressed{};
    std::optional<COLORREF> sourceColor;
    bool hasStroke{};
    std::optional<COLORREF> strokeColor;
    bool uniformBackdrop{true};
    std::vector<RECT> detectedLines;
    std::vector<EraseRow> eraseLines{};
};
struct OverlayParagraph {
    std::wstring text;
    ComPtr<IDWriteTextLayout> layout;
    RECT rect{};
    bool heading{};
    float fontSize{};
};
struct OverlayBlock {
    RECT screen{};
    COLORREF background{};
    float opacity{1};
    float fontSize{};
    std::wstring text;
    ComPtr<IDWriteTextLayout> layout;
    RECT viewport{}, closeButton{};
    float contentHeight{};
    std::vector<OverlayParagraph> paragraphs;
    std::shared_ptr<const Image> image;
    std::shared_ptr<const TranslationResult> sourceResult;
    std::vector<PositionedText> positioned;
    RECT monitorArea{}, workArea{};
};
std::vector<OverlayBlock> BuildOverlay(const TranslationResult&, Image roi, RECT screenRoi, const Monitor&,
                                       const Settings&, bool retainSource = true);
class Overlay {
    struct DragState {
        POINT start{};
        RECT region{}, view{};
        std::optional<RECT> reading;
        bool moved{};
    };
    struct WindowData {
        Overlay* owner;
        HWND hwnd{};
        bool pressed{}, closeHover{}, dragPreview{}, progressFrame{};
        OverlayBlock block;
        Settings settings;
        UINT dpi{96};
        float scroll{};
        int mode{};
        int readingSourceMode{-1};
        std::optional<OverlayBlock> reading;
        // One-off vision result. It lives beside the recognized result so the toolbar can switch between
        // the two without repeating either request.
        std::optional<OverlayBlock> ai;
        std::optional<DragState> drag;
    };
    std::vector<std::unique_ptr<WindowData>> windows_;
    std::function<void()> dismissed_;
    std::function<void()> reselect_;
    std::function<void(RECT)> moved_;
    std::function<void()> aiRequest_;
    std::function<void(int, std::string)> languageChanged_;
    HWND toolbar_{};
    // The focus shade dims everything outside the selection while a translation is on screen. It sits
    // under the translation windows and swallows clicks on the dimmed area.
    HWND focusShade_{};
    static LRESULT CALLBACK ShadeProc(HWND, UINT, WPARAM, LPARAM);
    void UpdateShadeHole();
    void RaiseOverlayWindows();
    int pressedAction_{-1};
    int hoverAction_{-1};
    bool aiPending_{};
    bool aiHighlight_{};
    bool passthrough_{};
    bool captureExcluded_{true};
    std::wstring warning_;
    HWND badge_{};
    std::wstring badgeText_;
    UINT badgeDpi_{96};
    RECT badgeCloseRect_{};
    bool badgeHover_{}, badgePressed_{};
    std::function<void()> badgeClosed_;
    // The progress notice swaps its fixed status line in place, so the strip and the frozen frame beside
    // it keep running while the work moves from recognition to the service request.
    bool noticeActive_{};
    std::wstring noticeText_;
    Image noticeBackdrop_;
    Monitor noticeMonitor_{};
    static LRESULT CALLBACK ToolbarProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK BadgeProc(HWND, UINT, WPARAM, LPARAM);
    std::wstring ToolbarStatusText() const;
    bool HasToolbarStatusRow() const;
    const std::vector<std::string>* ActiveDetectedLanguages() const;
    void PositionToolbar();
    void RefreshToolbar();
    void ShowToolbar();
    void ToolbarAction(int);
    void ShowLanguageMenu(bool sourceMenu);
    int ToolbarHitTest(POINT clientPt) const;
    void PaintToolbar();
    void PaintBadge();
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    // The block the current view draws: the one-off vision result, the reading panel, or the recognized
    // result behind them.
    OverlayBlock& Active(WindowData&);
    const OverlayBlock& Active(const WindowData&) const;
    void Render(WindowData&);
    bool DragBorder(const WindowData&, POINT) const;
    void MoveDrag(WindowData&, POINT);
    void CancelDrag(WindowData&);
    void RestoreContents(WindowData&);

public:
    ~Overlay();
    void Show(const std::vector<OverlayBlock>&, const Settings&, UINT dpi, std::function<void()> dismissed,
              std::function<void()> reselect = {}, std::function<void(RECT)> moved = {},
              bool passthrough = false);
    void ShowContinuousBadge(RECT anchor, const Monitor&, const std::wstring& text,
                             std::function<void()> dismissed);
    void CloseContinuousBadge();
    bool IsControlPoint(POINT screenPt) const;
    void Notice(const std::wstring& text, RECT anchor, const Monitor&, std::function<void()> dismissed,
                bool excludeFromCapture = false);
    // Rewrites the running notice with another fixed status line. Callers use one phrase per stage.
    void SetProgressText(const std::wstring& text);
    void Progress(RECT region, const Monitor&, Image frozen, std::function<void()> dismissed,
                  bool continuous = false);
    bool CaptureExcluded() const {
        return captureExcluded_;
    }
    void Close();
    void SetWarning(const std::wstring& text);
    // The vision pass is requested from the toolbar and lands here as a switchable view.
    void SetAiRequest(std::function<void()> request) {
        aiRequest_ = std::move(request);
    }
    void SetLanguageHandler(std::function<void(int, std::string)> handler) {
        languageChanged_ = std::move(handler);
    }
    void ShowAiResult(OverlayBlock block);
    // Original cropped selection image retained for one-off vision passes without re-capturing screen.
    std::shared_ptr<const Image> SourceImage() const;
    // Dims the desktop around the selection once a translation is on screen.
    void ShowFocusShade();
    size_t Count() const {
        return windows_.size();
    }
    bool Dragging() const;
};
} // namespace sat
