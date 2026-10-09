#include "Overlay.h"
#include <algorithm>
#include <windowsx.h>
namespace sat {
namespace {
constexpr int ActionCount=6;
const wchar_t* const Labels[]{L"原位译文",L"查看原图",L"全文阅读",L"复制译文",L"重新框选",L"关闭"};
RECT Fit(RECT r,RECT work){int w=std::min(Width(r),Width(work)),h=std::min(Height(r),Height(work));r.left=std::clamp(r.left,work.left,work.right-w);r.top=std::clamp(r.top,work.top,work.bottom-h);r.right=r.left+w;r.bottom=r.top+h;return r;}
}
void Overlay::ShowToolbar(){
 auto& data=*windows_.front();float scale=data.dpi/96.f;auto r=data.block.screen;int w=int(516*scale),h=int(64*scale),gap=int(8*scale);
 RECT bounds{r.left,r.bottom+gap,r.left+w,r.bottom+gap+h};
 if(bounds.bottom>data.block.workArea.bottom){bounds.top=r.top-gap-h;bounds.bottom=r.top-gap;}
 bounds=Fit(bounds,data.block.workArea);
 WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SAT.TranslationToolbar";wc.lpfnWndProc=ToolbarProc;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
 toolbarStatus_=L"原位覆盖 · 蓝色标记处滚轮阅读，全文阅读可查看完整译文";
 if(std::none_of(data.block.positioned.begin(),data.block.positioned.end(),[](const PositionedText& p){return !p.detectedLines.empty();}))toolbarStatus_=L"AI 定位 · 如有错位，请使用全文阅读查看译文";
 if(std::any_of(data.block.positioned.begin(),data.block.positioned.end(),[](const PositionedText& p){return p.suppressed;}))toolbarStatus_=L"部分区域定位存在冲突，已保留原图；请使用全文阅读查看译文";
 toolbar_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"屏幕翻译操作",WS_POPUP,bounds.left,bounds.top,Width(bounds),Height(bounds),data.hwnd,nullptr,wc.hInstance,this);
 if(!toolbar_)throw AppError("overlay","无法创建翻译工具条");ShowWindow(toolbar_,SW_SHOWNOACTIVATE);
}
void Overlay::PositionToolbar(){
 if(!toolbar_||windows_.empty())return;auto& data=*windows_.front();auto r=data.mode==2&&data.reading?data.reading->screen:data.block.screen;
 int width=MulDiv(516,data.dpi,96),height=MulDiv(64,data.dpi,96),gap=MulDiv(8,data.dpi,96);
 RECT bounds{r.left,r.bottom+gap,r.left+width,r.bottom+gap+height};if(bounds.bottom>data.block.workArea.bottom){bounds.top=r.top-gap-height;bounds.bottom=r.top-gap;}
 bounds=Fit(bounds,data.block.workArea);SetWindowPos(toolbar_,HWND_TOPMOST,bounds.left,bounds.top,Width(bounds),Height(bounds),SWP_NOACTIVATE);
}
void Overlay::PaintToolbar(){
 PAINTSTRUCT ps{};auto dc=BeginPaint(toolbar_,&ps);RECT r{};GetClientRect(toolbar_,&r);auto brush=CreateSolidBrush(RGB(246,248,253));FillRect(dc,&r,brush);DeleteObject(brush);
 int dpi=windows_.front()->dpi;auto font=CreateFontW(-MulDiv(13,dpi,96),0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");auto old=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
 int row=MulDiv(38,dpi,96);
 for(int i=0;i<ActionCount;++i){RECT cell{i*Width(r)/ActionCount,0,(i+1)*Width(r)/ActionCount,row};
  bool active=i==windows_.front()->mode;
  if(active){auto fill=CreateSolidBrush(RGB(228,234,255));FillRect(dc,&cell,fill);DeleteObject(fill);}
  SetTextColor(dc,i==5?RGB(177,54,73):active?RGB(54,80,194):RGB(49,61,82));DrawTextW(dc,Labels[i],-1,&cell,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
 }
 RECT status{MulDiv(10,dpi,96),row,Width(r)-4,Height(r)};SetTextColor(dc,RGB(104,116,137));DrawTextW(dc,toolbarStatus_.c_str(),-1,&status,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
 SelectObject(dc,old);DeleteObject(font);EndPaint(toolbar_,&ps);
}
void Overlay::ToolbarAction(int action){
 if(windows_.empty())return;auto& data=*windows_.front();
 if(data.drag)CancelDrag(data);
 if(action==5){auto cb=dismissed_;if(cb)cb();return;}
 if(action==4){auto cb=reselect_;if(cb)cb();else toolbarStatus_=L"请使用截图快捷键重新框选";}
 if(action>=0&&action<=2){
  if(action==2&&!data.reading){
   auto rect=data.block.screen;rect.right=rect.left+std::max(Width(rect),MulDiv(560,data.dpi,96));rect.bottom=rect.top+std::max(Height(rect),MulDiv(420,data.dpi,96));auto readingWork=data.block.workArea;readingWork.bottom-=std::min(MulDiv(76,data.dpi,96),Height(readingWork)/4);rect=Fit(rect,readingWork);
   auto options=data.settings;options.spatialOverlay=false;options.outlineWidth=0;options.textColor=RGB(29,39,57);options.autoFont=false;options.fontSize=data.settings.autoFont?18:data.settings.fontSize;
   Image paper{1,1,{253,250,248,255}};TranslationResult result;result.segments.push_back({"",Utf8(data.block.text),{0,0,1000,1000}});
   Monitor monitor{L"reading",data.block.workArea,data.dpi};data.reading=BuildOverlay(result,paper,rect,monitor,options).front();data.reading->background=RGB(248,250,253);data.reading->opacity=1;

  }
  data.mode=action;data.scroll=0;Render(data);PositionToolbar();
  toolbarStatus_=action==1?L"原始截图 · 拖动边框松手重译":action==2?L"全文译文 · 滚轮阅读，拖边框按原选区大小重译":L"原位覆盖 · 拖动边框松手重译，蓝色标记处滚轮阅读";
  if(action==0){
   if(std::none_of(data.block.positioned.begin(),data.block.positioned.end(),[](const PositionedText& p){return !p.detectedLines.empty();}))toolbarStatus_=L"AI 定位 · 如有错位，请使用全文阅读查看译文";
   if(std::any_of(data.block.positioned.begin(),data.block.positioned.end(),[](const PositionedText& p){return p.suppressed;}))toolbarStatus_=L"部分区域定位存在冲突，已保留原图；请使用全文阅读查看译文";
  }
 }
 if(action==3){
  const auto& text=data.block.text;size_t bytes=(text.size()+1)*sizeof(wchar_t);auto memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
  if(!memory){toolbarStatus_=L"复制失败，请重试";}
  else {auto target=GlobalLock(memory);bool copied=false;if(target){memcpy(target,text.c_str(),bytes);GlobalUnlock(memory);
    if(OpenClipboard(toolbar_)){if(EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,memory))copied=true;CloseClipboard();}}
   if(!copied)GlobalFree(memory);toolbarStatus_=copied?L"已复制完整译文":L"剪贴板忙，请重试";
  }
 }
 if(toolbar_)InvalidateRect(toolbar_,nullptr,FALSE);
}
LRESULT CALLBACK Overlay::ToolbarProc(HWND h,UINT m,WPARAM w,LPARAM l){
 auto self=reinterpret_cast<Overlay*>(GetWindowLongPtrW(h,GWLP_USERDATA));
 if(m==WM_NCCREATE){self=static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
 if(!self)return DefWindowProcW(h,m,w,l);
 try{switch(m){
 case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
 case WM_ERASEBKGND:return 1;
 case WM_PAINT:self->PaintToolbar();return 0;
 case WM_LBUTTONDOWN:{RECT r{};GetClientRect(h,&r);if(GET_Y_LPARAM(l)>=0&&GET_Y_LPARAM(l)<MulDiv(38,self->windows_.front()->dpi,96)){self->pressedAction_=GET_X_LPARAM(l)*ActionCount/std::max(1,Width(r));SetCapture(h);}return 0;}
 case WM_LBUTTONUP:{RECT r{};GetClientRect(h,&r);int action=GET_X_LPARAM(l)*ActionCount/std::max(1,Width(r));bool run=GET_X_LPARAM(l)>=0&&GET_X_LPARAM(l)<Width(r)&&GET_Y_LPARAM(l)>=0&&GET_Y_LPARAM(l)<MulDiv(38,self->windows_.front()->dpi,96)&&action==self->pressedAction_;self->pressedAction_=-1;if(GetCapture()==h)ReleaseCapture();if(run)self->ToolbarAction(action);return 0;}
 case WM_COMMAND:self->ToolbarAction(LOWORD(w));return 0;
 case WM_CAPTURECHANGED:self->pressedAction_=-1;return 0;
 case WM_CLOSE:{auto cb=self->dismissed_;if(cb)cb();return 0;}
 case WM_NCDESTROY:self->toolbar_=nullptr;SetWindowLongPtrW(h,GWLP_USERDATA,0);return 0;
 }}catch(...){self->toolbarStatus_=L"操作未完成，请重试";InvalidateRect(h,nullptr,FALSE);}
 return DefWindowProcW(h,m,w,l);
}
}



