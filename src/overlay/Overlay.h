#pragma once
#include "common/Platform.h"
#include <dwrite.h>
namespace sat {
struct PositionedText {
 RECT rect{},sourceRect{}; COLORREF background{}; float fontSize{},padding{},contentHeight{},lineHeight{},eraseHeight{},scroll{}; bool overflow{};
 ComPtr<IDWriteTextLayout> layout;
 bool suppressed{};
 std::optional<COLORREF> sourceColor;
 std::vector<RECT> detectedLines;
};
struct OverlayBlock {
 RECT screen{}; COLORREF background{};float opacity{1};
 float fontSize{};std::wstring text;ComPtr<IDWriteTextLayout> layout;
 RECT viewport{},closeButton{};float contentHeight{};
 std::shared_ptr<const Image> image;
 std::vector<PositionedText> positioned;
 RECT monitorArea{},workArea{};
};
std::vector<OverlayBlock> BuildOverlay(const TranslationResult&,const Image& roi,RECT screenRoi,const Monitor&,const Settings&);
class Overlay {
 struct DragState { POINT start{};RECT region{},view{};std::optional<RECT> reading;bool moved{}; };
 struct WindowData { Overlay* owner;HWND hwnd{};bool pressed{},dragPreview{},progressFrame{};OverlayBlock block;Settings settings;UINT dpi{96};float scroll{};int mode{};std::optional<OverlayBlock> reading;std::optional<DragState> drag; };
 std::vector<std::unique_ptr<WindowData>> windows_;
 std::function<void()> dismissed_;
 std::function<void()> reselect_;
 std::function<void(RECT)> moved_;
 HWND toolbar_{};int pressedAction_{-1};std::wstring toolbarStatus_;
 static LRESULT CALLBACK ToolbarProc(HWND,UINT,WPARAM,LPARAM);
 void PositionToolbar();void ShowToolbar();void ToolbarAction(int);void PaintToolbar();
 static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
 void Render(WindowData&);
 bool DragBorder(const WindowData&,POINT)const;
 void MoveDrag(WindowData&,POINT);void CancelDrag(WindowData&);void RestoreContents(WindowData&);
public:
 ~Overlay();
 void Show(const std::vector<OverlayBlock>&,const Settings&,UINT dpi,std::function<void()> dismissed,std::function<void()> reselect={},std::function<void(RECT)> moved={});
 void Notice(const std::wstring& text,RECT anchor,const Monitor&,std::function<void()> dismissed);
 void Progress(RECT region,const Monitor&,std::function<void()> dismissed);
 void Close();size_t Count()const{return windows_.size();}
 bool Dragging()const;
};
}


