#include "Overlay.h"
#include "SpatialLayout.h"
#include "capture/Capture.h"
#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <windowsx.h>
namespace sat {
namespace {
ComPtr<IDWriteFactory> WriteFactory(){ComPtr<IDWriteFactory> f;CheckHR(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(f.GetAddressOf())),"layout");return f;}
D2D1_COLOR_F ColorF(COLORREF c,float a=1){return D2D1::ColorF(GetRValue(c)/255.f,GetGValue(c)/255.f,GetBValue(c)/255.f,a);}
RECT Clamp(RECT r,RECT screen){int w=std::min(Width(r),Width(screen)),h=std::min(Height(r),Height(screen));r.left=std::clamp(r.left,screen.left,screen.right-w);r.top=std::clamp(r.top,screen.top,screen.bottom-h);r.right=r.left+w;r.bottom=r.top+h;return r;}
// DirectWrite sends shaped glyph runs here; outline comes from the glyph geometry.
class OutlinedRenderer final:public IDWriteTextRenderer {
 ULONG refs_{1};ID2D1RenderTarget* target_;ID2D1Factory* factory_;ID2D1Brush* fill_;ID2D1Brush* edge_;float stroke_;
public:
 OutlinedRenderer(ID2D1RenderTarget* t,ID2D1Factory* f,ID2D1Brush* fill,ID2D1Brush* edge,float stroke):target_(t),factory_(f),fill_(fill),edge_(edge),stroke_(stroke){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID i,void** out)override{if(!out)return E_POINTER;if(i==__uuidof(IUnknown)||i==__uuidof(IDWritePixelSnapping)||i==__uuidof(IDWriteTextRenderer)){*out=static_cast<IDWriteTextRenderer*>(this);AddRef();return S_OK;}*out=nullptr;return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}ULONG STDMETHODCALLTYPE Release()override{auto n=--refs_;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* value)override{*value=FALSE;return S_OK;}
 HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* value)override{D2D1_MATRIX_3X2_F m;target_->GetTransform(&m);static_assert(sizeof(m)==sizeof(*value));memcpy(value,&m,sizeof(m));return S_OK;}
 HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* value)override{*value=1;return S_OK;}
 HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT x,FLOAT y,DWRITE_MEASURING_MODE,const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION*,IUnknown*)override{
  ComPtr<ID2D1PathGeometry> geometry;HRESULT hr=factory_->CreatePathGeometry(&geometry);if(FAILED(hr))return hr;ComPtr<ID2D1GeometrySink> sink;hr=geometry->Open(&sink);if(FAILED(hr))return hr;
  hr=run->fontFace->GetGlyphRunOutline(run->fontEmSize,run->glyphIndices,run->glyphAdvances,run->glyphOffsets,run->glyphCount,run->isSideways,run->bidiLevel%2,sink.Get());if(FAILED(hr))return hr;hr=sink->Close();if(FAILED(hr))return hr;
  D2D1_MATRIX_3X2_F previous;target_->GetTransform(&previous);target_->SetTransform(D2D1::Matrix3x2F::Translation(x,y)*previous);
  if(stroke_>0)target_->DrawGeometry(geometry.Get(),edge_,2*stroke_);target_->FillGeometry(geometry.Get(),fill_);target_->SetTransform(previous);return S_OK;
 }
 HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE DrawInlineObject(void*,FLOAT,FLOAT,IDWriteInlineObject*,BOOL,BOOL,IUnknown*)override{return E_NOTIMPL;}
};
}
std::vector<OverlayBlock> BuildOverlay(const TranslationResult& result,const Image& roi,RECT screenRoi,const Monitor& monitor,const Settings& settings){
 if(result.segments.empty())return {};
 if(!Valid(screenRoi))throw AppError("layout","译文框范围无效，请重新框选");
 auto factory=WriteFactory();float scale=monitor.dpi/96.f;
 OverlayBlock block;block.screen=screenRoi;block.monitorArea=monitor.rect;block.workArea=monitor.rect;
 MONITORINFO info{sizeof(info)};auto handle=MonitorFromRect(&screenRoi,MONITOR_DEFAULTTONULL);if(GetMonitorInfoW(handle,&info)&&EqualRect(&info.rcMonitor,&monitor.rect))block.workArea=info.rcWork;
 for(const auto& segment:result.segments){if(!block.text.empty())block.text+=L"\n\n";block.text+=Wide(segment.translated);}
 auto bg=AnalyzeBackground(roi,{0,0,roi.width,roi.height});
 block.background=bg.simple?bg.color:(bg.luminance>128?RGB(235,235,235):RGB(20,22,27));block.opacity=bg.simple?1.f:settings.backgroundOpacity;
 const int w=Width(screenRoi),h=Height(screenRoi);
 const int pad=std::min(std::max(2,int(std::ceil(8*scale))),std::max(1,(std::min(w,h)-2)/4));
 const int header=std::min(std::max(12,int(std::ceil(30*scale))),std::max(1,h/3));
 const int button=std::max(1,std::min(w-2,header-2));
 block.closeButton={std::max(0,w-button-1),1,std::max(1,w-1),1+button};
 block.viewport={pad,std::min(h-1,header+pad),std::max(pad+1,w-pad-std::max(2,int(5*scale))),std::max(header+pad+1,h-pad)};
 block.viewport.right=std::min<LONG>(w,block.viewport.right);block.viewport.bottom=std::min<LONG>(h,block.viewport.bottom);
 block.fontSize=settings.autoFont?22*scale:settings.fontSize*scale;
 ComPtr<IDWriteTextFormat> format;CheckHR(factory->CreateTextFormat(settings.font.c_str(),nullptr,DWRITE_FONT_WEIGHT_MEDIUM,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,block.fontSize,L"zh-CN",&format),"layout");
 format->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);
 CheckHR(factory->CreateTextLayout(block.text.c_str(),static_cast<UINT32>(block.text.size()),format.Get(),static_cast<float>(std::max(1,Width(block.viewport))),static_cast<float>(std::max(1,Height(block.viewport))),&block.layout),"layout");
 DWRITE_TEXT_METRICS metrics{};CheckHR(block.layout->GetMetrics(&metrics),"layout");block.contentHeight=metrics.height;
  if(settings.spatialOverlay){
  block.image=std::make_shared<Image>(roi);block.positioned=LayoutPositioned(result,roi,settings,monitor.dpi);
 }
 return {std::move(block)};
}
Overlay::~Overlay(){Close();}
void Overlay::Close(){dismissed_={};reselect_={};moved_={};if(toolbar_){DestroyWindow(toolbar_);toolbar_=nullptr;}for(auto& w:windows_){w->drag.reset();if(w->hwnd)DestroyWindow(w->hwnd);}windows_.clear();}
bool Overlay::Dragging()const{return std::any_of(windows_.begin(),windows_.end(),[](const auto& window){return window->drag.has_value();});}
bool Overlay::DragBorder(const WindowData& data,POINT point)const{
 if(!moved_||!Valid(data.block.monitorArea))return false;
 const auto& active=data.reading&&data.mode==2?*data.reading:data.block;
 RECT client{0,0,Width(active.screen),Height(active.screen)};
 if(!PtInRect(&client,point)||((!data.block.image||data.mode==2)&&PtInRect(&active.closeButton,point)))return false;
 const int edge=std::max(4,MulDiv(6,data.dpi,96));
 return point.x<edge||point.y<edge||point.x>=client.right-edge||point.y>=client.bottom-edge;
}
void Overlay::MoveDrag(WindowData& data,POINT point){
 if(!data.drag)return;auto& drag=*data.drag;
 LONG dx=point.x-drag.start.x,dy=point.y-drag.start.y;
 const bool firstMove=!drag.moved;
 if(!drag.moved){
  if(std::abs(dx)<GetSystemMetricsForDpi(SM_CXDRAG,data.dpi)&&std::abs(dy)<GetSystemMetricsForDpi(SM_CYDRAG,data.dpi))return;
  drag.moved=true;
 }
 // Keep both the actual capture and the visible reading frame on this monitor.
 // Moving a reading frame never replaces the capture with its expanded dimensions.
 const auto area=data.block.monitorArea;
 dx=std::clamp(dx,std::max(area.left-drag.region.left,area.left-drag.view.left),std::min(area.right-drag.region.right,area.right-drag.view.right));
 dy=std::clamp(dy,std::max(area.top-drag.region.top,area.top-drag.view.top),std::min(area.bottom-drag.region.bottom,area.bottom-drag.view.bottom));
 data.block.screen=drag.region;OffsetRect(&data.block.screen,dx,dy);
 if(data.reading&&drag.reading){data.reading->screen=*drag.reading;OffsetRect(&data.reading->screen,dx,dy);}
 RECT view=drag.view;OffsetRect(&view,dx,dy);
 if(firstMove){
  data.dragPreview=true;if(toolbar_)ShowWindow(toolbar_,SW_HIDE);Render(data);
 }else SetWindowPos(data.hwnd,nullptr,view.left,view.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
 PositionToolbar();
}
void Overlay::RestoreContents(WindowData& data){
 if(!data.dragPreview)return;data.dragPreview=false;Render(data);
 if(toolbar_)ShowWindow(toolbar_,SW_SHOWNOACTIVATE);
}
void Overlay::CancelDrag(WindowData& data){
 if(!data.drag)return;auto drag=*data.drag;data.drag.reset();data.pressed=false;
 data.block.screen=drag.region;if(data.reading&&drag.reading)data.reading->screen=*drag.reading;
 SetWindowPos(data.hwnd,nullptr,drag.view.left,drag.view.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);PositionToolbar();
 if(GetCapture()==data.hwnd)ReleaseCapture();
 RestoreContents(data);
}
void Overlay::Render(WindowData& window){
 const auto& block=window.reading&&window.mode==2?*window.reading:window.block;const auto& settings=window.settings;const auto dpi=window.dpi;
 int w=Width(block.screen),h=Height(block.screen);BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
 HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);void* bits=nullptr;HBITMAP bmp=CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&bits,nullptr,0);ReleaseDC(nullptr,screen);
 if(!dc||!bmp){if(dc)DeleteDC(dc);if(bmp)DeleteObject(bmp);throw AppError("overlay","翻译层内存分配失败");}auto previous=SelectObject(dc,bmp);
 try{
  ComPtr<ID2D1Factory> factory;CheckHR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()),"overlay");
  ComPtr<ID2D1DCRenderTarget> rt;auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);CheckHR(factory->CreateDCRenderTarget(&props,&rt),"overlay");RECT r{0,0,w,h};CheckHR(rt->BindDC(dc,&r),"overlay");
  ComPtr<ID2D1SolidColorBrush> background,fill,edge;CheckHR(rt->CreateSolidColorBrush(ColorF(block.background,block.opacity),&background),"overlay");
  auto foreground=settings.textColor;
  // Default white switches to dark on a light cover; explicitly chosen colors remain honored.
  foreground=ReadableTextColor(foreground,block.background);
  CheckHR(rt->CreateSolidColorBrush(ColorF(foreground),&fill),"overlay");CheckHR(rt->CreateSolidColorBrush(ColorF(settings.outlineColor),&edge),"overlay");
  rt->BeginDraw();rt->Clear(D2D1::ColorF(0,0.f));
  if(window.dragPreview){
   // Leave every interior pixel transparent so the live desktop stays visible.
   edge->SetColor(ColorF(window.progressFrame?RGB(72,180,255):RGB(88,119,235)));
   const float stroke=std::min(std::max(1.f,2*dpi/96.f),float(std::min(w,h))/2);
   rt->DrawRectangle(D2D1::RectF(stroke/2,stroke/2,w-stroke/2,h-stroke/2),edge.Get(),stroke);
  }else{
  rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);rt->FillRectangle(D2D1::RectF(0,0,static_cast<float>(w),static_cast<float>(h)),background.Get());
  if(block.image&&window.mode!=2){
   const auto& image=*block.image;ComPtr<ID2D1Bitmap> bitmap;
   auto bitmapProps=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE));
   CheckHR(rt->CreateBitmap(D2D1::SizeU(image.width,image.height),image.bgra.data(),image.width*4,bitmapProps,&bitmap),"overlay");
   rt->DrawBitmap(bitmap.Get(),D2D1::RectF(0,0,float(w),float(h)));
   if(window.mode==0)DrawPositioned(rt.Get(),block.positioned,settings.textColor);
   edge->SetColor(ColorF(RGB(88,119,235)));rt->DrawRectangle(D2D1::RectF(1,1,float(w)-1,float(h)-1),edge.Get(),2);
  }else{
  // Clip all translated text to the selected region's content area. Long text scrolls.
  auto area=D2D1::RectF(float(block.viewport.left),float(block.viewport.top),float(block.viewport.right),float(block.viewport.bottom));
  rt->PushAxisAlignedClip(area,D2D1_ANTIALIAS_MODE_ALIASED);
  ComPtr<OutlinedRenderer> renderer;renderer.Attach(new OutlinedRenderer(rt.Get(),factory.Get(),fill.Get(),edge.Get(),window.reading&&window.mode==2?0.f:settings.outlineWidth*dpi/96.f));
  CheckHR(block.layout->Draw(nullptr,renderer.Get(),area.left,area.top-window.scroll),"overlay");rt->PopAxisAlignedClip();
  const float scale=dpi/96.f;
  ComPtr<ID2D1SolidColorBrush> frame,header,button,white;
  CheckHR(rt->CreateSolidColorBrush(D2D1::ColorF(0.2f,0.65f,1.f,1.f),&frame),"overlay");
  CheckHR(rt->CreateSolidColorBrush(D2D1::ColorF(0.08f,0.12f,0.19f,1.f),&header),"overlay");
  CheckHR(rt->CreateSolidColorBrush(D2D1::ColorF(0.65f,0.16f,0.2f,1.f),&button),"overlay");
  CheckHR(rt->CreateSolidColorBrush(D2D1::ColorF(1.f,1.f,1.f,1.f),&white),"overlay");
  float headerBottom=float(block.closeButton.bottom+1);
  rt->FillRectangle(D2D1::RectF(0,0,float(w),headerBottom),header.Get());
  const auto& close=block.closeButton;auto closeRect=D2D1::RectF(float(close.left),float(close.top),float(close.right),float(close.bottom));
  rt->FillRectangle(closeRect,button.Get());float inset=std::max(1.f,Width(close)*0.3f);
  rt->DrawLine(D2D1::Point2F(closeRect.left+inset,closeRect.top+inset),D2D1::Point2F(closeRect.right-inset,closeRect.bottom-inset),white.Get(),std::max(1.f,1.5f*scale));
  rt->DrawLine(D2D1::Point2F(closeRect.right-inset,closeRect.top+inset),D2D1::Point2F(closeRect.left+inset,closeRect.bottom-inset),white.Get(),std::max(1.f,1.5f*scale));
  if(w>100*scale&&headerBottom>16*scale){
   ComPtr<IDWriteTextFormat> title;auto writer=WriteFactory();CheckHR(writer->CreateTextFormat(settings.font.c_str(),nullptr,DWRITE_FONT_WEIGHT_MEDIUM,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,12*scale,L"zh-CN",&title),"overlay");
   title->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);title->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
   const wchar_t* caption=moved_?L"译文 · 拖动边框，松手重译":block.contentHeight>Height(block.viewport)?L"译文 · 滚轮查看":L"译文";
   rt->DrawText(caption,static_cast<UINT32>(wcslen(caption)),title.Get(),D2D1::RectF(8*scale,0,float(close.left)-4*scale,headerBottom),white.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  if(block.contentHeight>Height(block.viewport)){
   const float track=float(Height(block.viewport)),thumb=std::min(track,std::max(8*scale,track*track/block.contentHeight));
   const float top=block.viewport.top+(track-thumb)*window.scroll/(block.contentHeight-track);
   rt->FillRectangle(D2D1::RectF(float(w)-5*scale,top,float(w)-2*scale,top+thumb),frame.Get());
  }
  const float stroke=std::min(std::max(1.f,2*scale),float(std::min(w,h))/2);
  rt->DrawRectangle(D2D1::RectF(stroke/2,stroke/2,w-stroke/2,h-stroke/2),frame.Get(),stroke);
  }
  }
  CheckHR(rt->EndDraw(),"overlay");
  POINT position{block.screen.left,block.screen.top},origin{};SIZE size{w,h};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};if(!UpdateLayeredWindow(window.hwnd,nullptr,&position,&size,dc,&origin,0,&blend,ULW_ALPHA))throw AppError("overlay","无法显示翻译层");
 }catch(...){SelectObject(dc,previous);DeleteObject(bmp);DeleteDC(dc);throw;}
 SelectObject(dc,previous);DeleteObject(bmp);DeleteDC(dc);
}
void Overlay::Show(const std::vector<OverlayBlock>& blocks,const Settings& settings,UINT dpi,std::function<void()> dismissed,std::function<void()> reselect,std::function<void(RECT)> moved){
 Close();dismissed_=std::move(dismissed);reselect_=std::move(reselect);moved_=std::move(moved);WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SAT.Overlay";wc.lpfnWndProc=Proc;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
 try{for(auto& b:blocks){auto data=std::make_unique<WindowData>();data->owner=this;data->block=b;data->settings=settings;data->dpi=dpi;auto ptr=data.get();windows_.push_back(std::move(data));
   ptr->hwnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"翻译结果 · 右上角关闭",WS_POPUP,b.screen.left,b.screen.top,Width(b.screen),Height(b.screen),nullptr,nullptr,wc.hInstance,ptr);if(!ptr->hwnd)throw AppError("overlay","无法创建翻译层");Render(*ptr);ShowWindow(ptr->hwnd,SW_SHOWNOACTIVATE);
 }if(!blocks.empty()&&blocks.front().image){ShowToolbar();}}catch(...){Close();throw;}
}
void Overlay::Notice(const std::wstring& text,RECT anchor,const Monitor& m,std::function<void()> dismissed){
 Settings s;s.spatialOverlay=false;s.autoFont=false;s.fontSize=17;s.outlineWidth=0;Image image;image.width=std::max(300,Width(anchor));image.height=70;image.bgra.resize(static_cast<size_t>(image.width)*image.height*4,28);for(size_t i=3;i<image.bgra.size();i+=4)image.bgra[i]=255;
 TranslationResult r;r.segments.push_back({"",Utf8(text),{0,0,1000,1000}});RECT roi{anchor.left,anchor.top,anchor.left+image.width,anchor.top+70};roi=Clamp(roi,m.rect);auto blocks=BuildOverlay(r,image,roi,m,s);Show(blocks,s,m.dpi,std::move(dismissed));
}
void Overlay::Progress(RECT region,const Monitor& monitor,std::function<void()> dismissed){
 // Keep the full selection outline while the notice occupies a separate strip.
 // The transparent interior leaves the original desktop readable during OCR.
 RECT anchor=region;const int noticeHeight=70;
 if(region.bottom+noticeHeight<=monitor.rect.bottom)anchor.top=region.bottom;
 else if(region.top-noticeHeight>=monitor.rect.top)anchor.top=region.top-noticeHeight;
 anchor.bottom=anchor.top+noticeHeight;
 Notice(L"正在翻译… 点击 × 取消",anchor,monitor,std::move(dismissed));
 try{
  WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SAT.ProgressFrame";wc.lpfnWndProc=Proc;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
  auto data=std::make_unique<WindowData>();data->owner=this;data->dpi=monitor.dpi;data->dragPreview=true;data->progressFrame=true;data->block.screen=region;
  auto ptr=data.get();windows_.push_back(std::move(data));
  ptr->hwnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,wc.lpszClassName,L"翻译处理中选区",WS_POPUP,region.left,region.top,Width(region),Height(region),nullptr,nullptr,wc.hInstance,ptr);
  if(!ptr->hwnd)throw AppError("overlay","无法创建处理中的选区边框");
  Render(*ptr);ShowWindow(ptr->hwnd,SW_SHOWNOACTIVATE);
 }catch(...){Close();throw;}
}
LRESULT CALLBACK Overlay::Proc(HWND h,UINT m,WPARAM w,LPARAM l){
 auto data=reinterpret_cast<WindowData*>(GetWindowLongPtrW(h,GWLP_USERDATA));if(m==WM_NCCREATE){data=static_cast<WindowData*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);data->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(data));}
 if(data)try{switch(m){case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
 case WM_SETCURSOR:if(LOWORD(l)==HTCLIENT){POINT point{};GetCursorPos(&point);ScreenToClient(h,&point);SetCursor(LoadCursorW(nullptr,data->drag||data->owner->DragBorder(*data,point)?IDC_SIZEALL:IDC_ARROW));return TRUE;}break;
 case WM_LBUTTONDOWN:{POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};const auto& active=data->reading&&data->mode==2?*data->reading:data->block;data->pressed=(!data->block.image||data->mode==2)&&PtInRect(&active.closeButton,point)!=FALSE;
  if(!data->drag&&data->owner->DragBorder(*data,point)){ClientToScreen(h,&point);data->drag=DragState{point,data->block.screen,active.screen,data->reading?std::optional<RECT>(data->reading->screen):std::nullopt};SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));}
  SetCapture(h);return 0;}
 case WM_MOUSEMOVE:if(data->drag){if(!(w&MK_LBUTTON)){data->owner->CancelDrag(*data);return 0;}POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(h,&point);data->owner->MoveDrag(*data,point);SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));}return 0;
 case WM_LBUTTONUP:{POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
  if(data->drag){ClientToScreen(h,&point);data->owner->MoveDrag(*data,point);auto region=data->block.screen;bool changed=!EqualRect(&region,&data->drag->region);data->drag.reset();data->pressed=false;auto cb=data->owner->moved_;if(GetCapture()==h)ReleaseCapture();if(changed&&cb)cb(region);else data->owner->RestoreContents(*data);return 0;}
  const auto& active=data->reading&&data->mode==2?*data->reading:data->block;bool close=data->pressed&&PtInRect(&active.closeButton,point);data->pressed=false;if(GetCapture()==h)ReleaseCapture();if(close){auto cb=data->owner->dismissed_;if(cb)cb();}return 0;}
 case WM_RBUTTONDOWN:case WM_MBUTTONDOWN:if(data->drag){data->owner->CancelDrag(*data);return 0;}SetCapture(h);return 0;
 case WM_RBUTTONUP:case WM_MBUTTONUP:if(GetCapture()==h)ReleaseCapture();return 0;
 case WM_MOUSEWHEEL:{if(data->drag)return 0;if(data->block.image&&data->mode!=2){
   if(data->mode==0){POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(h,&point);
    for(auto it=data->block.positioned.rbegin();it!=data->block.positioned.rend();++it)if(PtInRect(&it->rect,point)){
     if(it->suppressed)continue;auto maximum=std::max(0.f,it->contentHeight-(Height(it->rect)-2*it->padding));it->scroll=std::clamp(it->scroll-GET_WHEEL_DELTA_WPARAM(w)/float(WHEEL_DELTA)*it->fontSize*3,0.f,maximum);data->owner->Render(*data);break;}
   }return 0;
  }const auto& active=data->reading&&data->mode==2?*data->reading:data->block;auto maximum=std::max(0.f,active.contentHeight-Height(active.viewport));data->scroll=std::clamp(data->scroll-GET_WHEEL_DELTA_WPARAM(w)/float(WHEEL_DELTA)*data->block.fontSize*3,0.f,maximum);data->owner->Render(*data);return 0;}
 case WM_MOUSEHWHEEL:return 0;
 case WM_CLOSE:{auto cb=data->owner->dismissed_;if(cb)cb();return 0;}
 case WM_CANCELMODE:case WM_CAPTURECHANGED:data->pressed=false;data->owner->CancelDrag(*data);if(m==WM_CANCELMODE&&GetCapture()==h)ReleaseCapture();return 0;
 case WM_KEYDOWN:if(w==VK_ESCAPE&&data->drag){data->owner->CancelDrag(*data);return 0;}break;
 case WM_NCDESTROY:data->hwnd=nullptr;SetWindowLongPtrW(h,GWLP_USERDATA,0);break;
 }}catch(...){return 0;}
 return DefWindowProcW(h,m,w,l);
}
}




