#include "Selection.h"
#include <windowsx.h>
#include <algorithm>
namespace sat {
Selection::~Selection(){Close();}
void Selection::Close(){done_={};if(hwnd_){DestroyWindow(hwnd_);hwnd_=nullptr;}image_.reset();}
void Selection::Show(const Monitor& m,std::shared_ptr<Image> image,std::function<void(std::optional<RECT>)> done){
 Close();monitor_=m;image_=std::move(image);done_=std::move(done);dragging_=false;
 WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SAT.Selection";wc.lpfnWndProc=Proc;wc.hCursor=LoadCursorW(nullptr,IDC_CROSS);RegisterClassW(&wc);
 hwnd_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,wc.lpszClassName,L"拖动选择翻译区域 · Esc 取消",WS_POPUP,m.rect.left,m.rect.top,Width(m.rect),Height(m.rect),nullptr,nullptr,wc.hInstance,this);
 if(!hwnd_)throw AppError("selection","无法创建框选窗口");
 ShowWindow(hwnd_,SW_SHOW);SetForegroundWindow(hwnd_);SetFocus(hwnd_);UpdateWindow(hwnd_);
}
void Selection::Finish(std::optional<RECT> r){
 auto callback=std::move(done_);done_={};dragging_=false;ReleaseCapture();
 if(hwnd_){DestroyWindow(hwnd_);hwnd_=nullptr;}image_.reset();if(callback)callback(r);
}
void Selection::Paint(){
 PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd_,&ps);RECT client{};GetClientRect(hwnd_,&client);
 if(image_){
  HDC mem=CreateCompatibleDC(dc);HBITMAP surface=CreateCompatibleBitmap(dc,Width(client),Height(client));auto old=SelectObject(mem,surface);
  BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=image_->width;bi.bmiHeader.biHeight=-image_->height;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
  auto draw=[&](HDC target){StretchDIBits(target,0,0,Width(client),Height(client),0,0,image_->width,image_->height,image_->bgra.data(),&bi,DIB_RGB_COLORS,SRCCOPY);};
  draw(mem);
  // Constant alpha darkening keeps selected pixels at their original brightness.
  HDC shade=CreateCompatibleDC(dc);HBITMAP pixel=CreateCompatibleBitmap(dc,1,1);auto oldShade=SelectObject(shade,pixel);SetPixelV(shade,0,0,RGB(0,0,0));
  BLENDFUNCTION blend{AC_SRC_OVER,0,105,0};AlphaBlend(mem,0,0,Width(client),Height(client),shade,0,0,1,1,blend);
  SelectObject(shade,oldShade);DeleteObject(pixel);DeleteDC(shade);
  if(dragging_){
   RECT r{std::min(start_.x,current_.x),std::min(start_.y,current_.y),std::max(start_.x,current_.x),std::max(start_.y,current_.y)};
   SaveDC(mem);IntersectClipRect(mem,r.left,r.top,r.right,r.bottom);draw(mem);RestoreDC(mem,-1);
   HPEN pen=CreatePen(PS_SOLID,std::max(1,int(monitor_.dpi/48)),RGB(72,180,255));auto op=SelectObject(mem,pen);auto ob=SelectObject(mem,GetStockObject(NULL_BRUSH));Rectangle(mem,r.left,r.top,r.right,r.bottom);SelectObject(mem,ob);SelectObject(mem,op);DeleteObject(pen);
  }
  SetBkMode(mem,TRANSPARENT);SetTextColor(mem,RGB(255,255,255));auto font=CreateFontW(-MulDiv(16,monitor_.dpi,96),0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");auto of=SelectObject(mem,font);
  RECT hint{24,24,Width(client)-24,64};DrawTextW(mem,L"拖动框选 · 松开鼠标开始翻译 · Esc 取消",-1,&hint,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);SelectObject(mem,of);DeleteObject(font);
  BitBlt(dc,0,0,Width(client),Height(client),mem,0,0,SRCCOPY);SelectObject(mem,old);DeleteObject(surface);DeleteDC(mem);
 }
 EndPaint(hwnd_,&ps);
}
LRESULT CALLBACK Selection::Proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
 auto self=reinterpret_cast<Selection*>(GetWindowLongPtrW(h,GWLP_USERDATA));
 if(msg==WM_NCCREATE){self=static_cast<Selection*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->hwnd_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
 if(!self)return DefWindowProcW(h,msg,wp,lp);
 switch(msg){
 case WM_ERASEBKGND:return 1;
 case WM_PAINT:self->Paint();return 0;
 case WM_KEYDOWN:if(wp==VK_ESCAPE){self->Finish({});return 0;}break;
 case WM_RBUTTONUP:case WM_CLOSE:self->Finish({});return 0;
 case WM_LBUTTONDOWN:self->start_={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};self->current_=self->start_;self->dragging_=true;SetCapture(h);return 0;
 case WM_MOUSEMOVE:if(self->dragging_){self->current_={std::clamp(GET_X_LPARAM(lp),0,Width(self->monitor_.rect)),std::clamp(GET_Y_LPARAM(lp),0,Height(self->monitor_.rect))};InvalidateRect(h,nullptr,FALSE);}return 0;
 case WM_LBUTTONUP:if(self->dragging_){
  auto a=self->start_;POINT b{std::clamp(GET_X_LPARAM(lp),0,Width(self->monitor_.rect)),std::clamp(GET_Y_LPARAM(lp),0,Height(self->monitor_.rect))};
  RECT r{std::min(a.x,b.x),std::min(a.y,b.y),std::max(a.x,b.x),std::max(a.y,b.y)};
  if(Width(r)<3||Height(r)<3){self->dragging_=false;ReleaseCapture();InvalidateRect(h,nullptr,FALSE);return 0;}
  OffsetRect(&r,self->monitor_.rect.left,self->monitor_.rect.top);self->Finish(r);
 }return 0;
 case WM_CAPTURECHANGED:if(self->dragging_){self->dragging_=false;InvalidateRect(h,nullptr,FALSE);}return 0;
 case WM_DISPLAYCHANGE:self->Finish({});return 0;
 case WM_NCDESTROY:self->hwnd_=nullptr;SetWindowLongPtrW(h,GWLP_USERDATA,0);break;
 }
 return DefWindowProcW(h,msg,wp,lp);
}
}
