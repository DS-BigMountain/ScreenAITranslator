#include "common/Platform.h"
#include "overlay/Overlay.h"
#include "capture/Capture.h"
#include "overlay/SpatialLayout.h"
#include "selection/Selection.h"
#include "settings/SettingsWindow.h"
#include <commctrl.h>
#include <iostream>
#include <algorithm>
#include <dwrite.h>
#include <dwmapi.h>
#include <wincodec.h>
namespace {
int checks{};
void Require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void Pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
unsigned long long Snapshot(HWND window,const std::wstring& name){
 RECT r{};GetWindowRect(window,&r);HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,sat::Width(r),sat::Height(r));ReleaseDC(nullptr,screen);auto old=SelectObject(dc,bitmap);SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);UpdateWindow(window);Sleep(100);bool layered=(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_LAYERED)!=0;HDC ownWindow=layered?GetDC(nullptr):GetWindowDC(window);Require(BitBlt(dc,0,0,sat::Width(r),sat::Height(r),ownWindow,layered?r.left:0,layered?r.top:0,SRCCOPY|CAPTUREBLT)!=FALSE,"cannot inspect native settings rendering");GdiFlush();ReleaseDC(layered?nullptr:window,ownWindow);SetWindowPos(window,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
 sat::ComPtr<IWICImagingFactory> factory;sat::CheckHR(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"test");sat::ComPtr<IWICBitmap> image;sat::CheckHR(factory->CreateBitmapFromHBITMAP(bitmap,nullptr,WICBitmapIgnoreAlpha,&image),"test");SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
 std::filesystem::create_directories("test-output");auto path=std::filesystem::path("test-output")/name;sat::ComPtr<IWICStream> stream;sat::CheckHR(factory->CreateStream(&stream),"test");sat::CheckHR(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"test");sat::ComPtr<IWICBitmapEncoder> encoder;sat::CheckHR(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"test");sat::CheckHR(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"test");sat::ComPtr<IWICBitmapFrameEncode> frame;sat::CheckHR(encoder->CreateNewFrame(&frame,nullptr),"test");sat::CheckHR(frame->Initialize(nullptr),"test");sat::CheckHR(frame->WriteSource(image.Get(),nullptr),"test");sat::CheckHR(frame->Commit(),"test");sat::CheckHR(encoder->Commit(),"test");
 std::vector<unsigned char> pixels(static_cast<size_t>(sat::Width(r))*sat::Height(r)*4);sat::CheckHR(image->CopyPixels(nullptr,sat::Width(r)*4,static_cast<UINT>(pixels.size()),pixels.data()),"test");unsigned long long hash=1469598103934665603ULL;for(auto pixel:pixels){hash^=pixel;hash*=1099511628211ULL;}return hash;
}
std::vector<HWND> Windows(const wchar_t* name){std::vector<HWND> result;HWND h=nullptr;while((h=FindWindowExW(nullptr,h,name,nullptr))!=nullptr){DWORD pid{};GetWindowThreadProcessId(h,&pid);if(pid==GetCurrentProcessId())result.push_back(h);}return result;}
RECT Bounds(HWND window){RECT r{};Require(GetWindowRect(window,&r)!=FALSE,"window bounds unavailable");return r;}
void MouseAt(HWND window,UINT message,WPARAM buttons,POINT screen){ScreenToClient(window,&screen);SendMessageW(window,message,buttons,MAKELPARAM(screen.x,screen.y));}
void RequireBounds(HWND window,RECT expected,const char* message){auto actual=Bounds(window);Require(EqualRect(&actual,&expected)!=FALSE,message);}
void TestLayout(){
 sat::Image image{600,160,std::vector<unsigned char>(600*160*4,255)};sat::Monitor monitor{L"test",{-1920,-200,0,880},144};RECT roi{-1860,20,-1260,180};sat::Settings settings;settings.spatialOverlay=false;settings.autoFont=false;settings.fontSize=22;
 sat::TranslationResult result{"ja",{{"字幕","这是一段应当完整显示的中文翻译。",{10,10,280,240}},{"名前","角色名称",{300,10,200,220}}}};
 auto blocks=sat::BuildOverlay(result,image,roi,monitor,settings);Require(blocks.size()==1,"selection must produce one frame");
 auto& block=blocks.front();Require(EqualRect(&block.screen,&roi)!=FALSE,"frame differs from selection including negative monitor coordinates");Require(block.fontSize==33,"fixed DIP font not scaled");Require(block.text.find(L"角色名称")!=std::wstring::npos,"multiple segments lost");
 Require(block.closeButton.left>=0&&block.closeButton.right<=sat::Width(roi)&&block.closeButton.bottom<=block.viewport.top,"close button is outside frame or overlaps text");
 result.segments.resize(1);result.segments[0].translated=std::string{};for(int i=0;i<100;++i)result.segments[0].translated+="长译文要完整显示，不可删除。";
 auto longText=sat::BuildOverlay(result,image,roi,monitor,settings);Require(longText[0].text==sat::Wide(result.segments[0].translated),"long text lost");Require(EqualRect(&longText[0].screen,&roi)!=FALSE,"long text expanded beyond selection");Require(longText[0].contentHeight>sat::Height(longText[0].viewport),"long text lacks scroll extent");Require(longText[0].fontSize==33,"long text shrunk to unreadable size");
}
void TestOverlay(){
 sat::Image image{440,240,std::vector<unsigned char>(440*240*4,25)};sat::Monitor monitor{L"test",{0,0,1200,800},96};RECT roi{100,100,540,340};sat::Settings settings;settings.spatialOverlay=false;settings.outlineWidth=0;
 Require(!settings.autoHide,"translations auto-close by default");
 std::string text="第一段：译文框与截图选区的位置、大小完全一致。";for(int i=0;i<12;++i)text+="\n译文内容保留在框内，滚动查看，点击正文不会关闭。";text+="\n最后一行：全部译文已保留。";
 sat::TranslationResult result{"ja",{{"原文",text,{0,0,1000,1000}}}};auto blocks=sat::BuildOverlay(result,image,roi,monitor,settings);sat::Overlay overlay;int dismissed{};overlay.Show(blocks,settings,96,[&]{++dismissed;});Pump();auto windows=Windows(L"SAT.Overlay");Require(windows.size()==1,"overlay missing");auto h=windows[0];auto style=GetWindowLongPtrW(h,GWL_EXSTYLE);Require(!(style&WS_EX_TRANSPARENT),"overlay click-through flag present");Require(style&WS_EX_LAYERED,"overlay not layered");Require(style&WS_EX_NOACTIVATE,"overlay steals focus");
 RECT actual{};GetWindowRect(h,&actual);Require(EqualRect(&actual,&roi)!=FALSE,"native window does not match selection");
 auto before=Snapshot(h,L"translation-frame-top.png");
 SendMessageW(h,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(20,80));SendMessageW(h,WM_LBUTTONUP,0,MAKELPARAM(20,80));Require(dismissed==0,"body click dismisses translation");
 SendMessageW(h,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-12000)),0);auto after=Snapshot(h,L"translation-frame-bottom.png");Require(before!=after,"scroll did not visibly change rendered text");Require(dismissed==0,"scroll dismisses translation");
 auto close=blocks[0].closeButton;auto click=MAKELPARAM((close.left+close.right)/2,(close.top+close.bottom)/2);
 SendMessageW(h,WM_LBUTTONDOWN,MK_LBUTTON,click);SendMessageW(h,WM_LBUTTONUP,0,MAKELPARAM(20,80));Require(dismissed==0,"dragging out of close button dismisses translation");
 SendMessageW(h,WM_LBUTTONDOWN,MK_LBUTTON,click);Require(dismissed==0,"close button closes before release");SendMessageW(h,WM_LBUTTONUP,0,click);Require(dismissed==1,"close button does not dismiss");overlay.Close();Require(Windows(L"SAT.Overlay").empty(),"overlay destruction failed");
}
void TestSpatialOverlay(){
 sat::Image image{600,300,std::vector<unsigned char>(600*300*4,245)};for(size_t i=3;i<image.bgra.size();i+=4)image.bgra[i]=255;
 // Public synthetic UI fixture: color sections are retained around translated labels.
 for(int y=0;y<300;++y)for(int x=420;x<600;++x){auto n=(y*600+x)*4;image.bgra[n]=210;image.bgra[n+1]=180;image.bgra[n+2]=120;}
 HDC fixtureDc=CreateCompatibleDC(nullptr);BITMAPINFO fixtureInfo{};fixtureInfo.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);fixtureInfo.bmiHeader.biWidth=600;fixtureInfo.bmiHeader.biHeight=-300;fixtureInfo.bmiHeader.biPlanes=1;fixtureInfo.bmiHeader.biBitCount=32;void* fixtureBits{};
 auto fixtureBitmap=CreateDIBSection(fixtureDc,&fixtureInfo,DIB_RGB_COLORS,&fixtureBits,nullptr,0);auto fixtureOld=SelectObject(fixtureDc,fixtureBitmap);memcpy(fixtureBits,image.bgra.data(),image.bgra.size());
 auto fixtureFont=CreateFontW(-28,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");auto fixtureOldFont=SelectObject(fixtureDc,fixtureFont);SetBkMode(fixtureDc,TRANSPARENT);SetTextColor(fixtureDc,RGB(30,40,60));TextOutW(fixtureDc,30,30,L"Welcome back",12);SetTextColor(fixtureDc,RGB(255,255,255));TextOutW(fixtureDc,450,30,L"Settings",8);SetTextColor(fixtureDc,RGB(30,40,60));TextOutW(fixtureDc,30,180,L"Details",7);GdiFlush();memcpy(image.bgra.data(),fixtureBits,image.bgra.size());for(size_t i=3;i<image.bgra.size();i+=4)image.bgra[i]=255;
 SelectObject(fixtureDc,fixtureOldFont);DeleteObject(fixtureFont);SelectObject(fixtureDc,fixtureOld);DeleteObject(fixtureBitmap);DeleteDC(fixtureDc);
 sat::Settings settings;settings.outlineWidth=0;sat::Monitor monitor{L"fixture",{0,0,1280,900},96};RECT roi{60,120,660,420};
 sat::TranslationResult result{"en",{{"Welcome back","欢迎回来",{50,100,500,150}},{"Settings","设置",{750,100,200,140}},{"Details","这是一段很长的说明，完整内容应该在全文阅读中保留。",{50,600,260,90}}}};
 auto blocks=sat::BuildOverlay(result,image,roi,monitor,settings);Require(blocks.size()==1&&blocks[0].image&&blocks[0].positioned.size()==3,"spatial document missing image or regions");
 auto& items=blocks[0].positioned;Require(items[0].sourceRect.left>=28&&items[0].sourceRect.left<40&&items[0].sourceRect.top>=28&&items[0].sourceRect.top<45&&items[1].sourceRect.left>=448&&items[1].sourceRect.left<460,"AI box refinement left the actual source glyphs");Require(items[2].overflow&&std::abs(items[2].rect.top-items[2].sourceRect.top)<=10,"long spatial text left its original region");Require(items[2].fontSize>=10,"auto layout shrank text below legibility floor");
 sat::Overlay overlay;int dismissed{},reselected{};overlay.Show(blocks,settings,96,[&]{++dismissed;},[&]{++reselected;});Pump();auto h=Windows(L"SAT.Overlay").front();auto bars=Windows(L"SAT.TranslationToolbar");Require(bars.size()==1,"spatial toolbar missing");auto bar=bars.front();
 auto translated=Snapshot(h,L"spatial-translated.png");Snapshot(bar,L"spatial-toolbar.png");SendMessageW(bar,WM_COMMAND,1,0);auto original=Snapshot(h,L"spatial-original.png");Require(original!=translated,"source toggle did not change image");
 SendMessageW(bar,WM_COMMAND,2,0);RECT reading{};GetWindowRect(h,&reading);Require(sat::Height(reading)>=320,"reading view did not expand small capture");Snapshot(h,L"spatial-reading.png");
 SendMessageW(bar,WM_COMMAND,0,0);RECT restored{};GetWindowRect(h,&restored);Require(EqualRect(&restored,&roi)!=FALSE,"spatial view did not restore exact selection coordinates");
 SendMessageW(bar,WM_COMMAND,4,0);Require(reselected==1,"reselect action not connected");SendMessageW(bar,WM_COMMAND,5,0);Require(dismissed==1,"toolbar close action not connected");overlay.Close();Require(Windows(L"SAT.TranslationToolbar").empty(),"toolbar leaked on close");
 sat::Monitor negative{L"negative",{-1920,-1080,0,0},144};RECT tinyRegion{-1900,-100,-1850,-50};auto edge=sat::BuildOverlay(result,image,tinyRegion,negative,settings);Require(EqualRect(&edge[0].screen,&tinyRegion)!=FALSE,"negative screen position changed");
 result.segments={{"bad","边界",{-500,-500,5000,5000}}};auto clamped=sat::BuildOverlay(result,image,roi,monitor,settings);auto rect=clamped[0].positioned[0].rect;Require(rect.left==0&&rect.top==0&&rect.right==600&&rect.bottom==300,"out-of-range model box was not clamped");
}
void TestOverlayDragging(){
 sat::Image image{440,240,std::vector<unsigned char>(440*240*4,245)};
 sat::TranslationResult result{"en",{{"move","拖动边框后重新翻译",{100,100,600,400}}}};
 for(bool spatial:{false,true})for(UINT dpi:{96u,192u}){
  sat::Monitor monitor{L"drag-test",{-300,-200,1300,1000},dpi};RECT roi{100,100,540,340};sat::Settings settings;settings.spatialOverlay=spatial;
  auto blocks=sat::BuildOverlay(result,image,roi,monitor,settings);sat::Overlay overlay;int moved{},dismissed{};RECT translated{};
  overlay.Show(blocks,settings,dpi,[&]{++dismissed;},{},[&](RECT r){translated=r;++moved;});Pump();auto h=Windows(L"SAT.Overlay").front();
  auto start=[&](POINT local){auto r=Bounds(h);POINT p{r.left+local.x,r.top+local.y};MouseAt(h,WM_LBUTTONDOWN,MK_LBUTTON,p);return p;};
  // Text, close buttons, and a stationary/jittering border click are not drags.
  auto body=start({40,100});POINT destination{body.x+40,body.y+30};MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,destination);MouseAt(h,WM_LBUTTONUP,0,destination);
  RequireBounds(h,roi,"body drag moved the translation");Require(moved==0&&!overlay.Dragging(),"body drag requested translation");
  auto edge=start({1,60});Require(overlay.Dragging(),"border press did not arm drag");destination={edge.x+1,edge.y+1};MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,destination);MouseAt(h,WM_LBUTTONUP,0,destination);
  RequireBounds(h,roi,"border jitter moved the translation");Require(moved==0,"border click requested translation");
  // Coordinates stay in screen pixels even after the window has already moved.
  edge=start({1,60});destination={edge.x+45,edge.y+25};MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,destination);auto preview=roi;OffsetRect(&preview,45,25);
  RequireBounds(h,preview,"drag preview has wrong screen coordinates");Require(moved==0,"drag called translation before release");
  destination={edge.x+70,edge.y+45};MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,destination);OffsetRect(&roi,70,45);RequireBounds(h,roi,"moving window introduced coordinate drift");
  MouseAt(h,WM_LBUTTONUP,0,destination);MouseAt(h,WM_LBUTTONUP,0,destination);Require(moved==1&&EqualRect(&translated,&roi),"release did not translate exactly once at the final position");Require(!overlay.Dragging()&&GetCapture()!=h,"release retained mouse capture");
  // Returning to the original location and losing capture must not commit a region.
  edge=start({1,60});destination={edge.x-40,edge.y-20};MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,destination);MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,edge);MouseAt(h,WM_LBUTTONUP,0,edge);
  RequireBounds(h,roi,"round-trip drag did not restore position");Require(moved==1,"round-trip drag requested translation");
  for(UINT cancel:{WM_CANCELMODE,WM_CAPTURECHANGED,WM_RBUTTONDOWN}){
   edge=start({1,60});destination={edge.x+30,edge.y+20};MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,destination);
   if(cancel==WM_CAPTURECHANGED)ReleaseCapture();else SendMessageW(h,cancel,0,0);
   RequireBounds(h,roi,"cancelled drag retained preview position");Require(moved==1&&!overlay.Dragging(),"cancelled drag committed a translation");
  }
  // All four borders move the whole frame, without resizing it.
  for(POINT border:std::vector<POINT>{{220,1},{439,100},{220,239}}){
   edge=start(border);destination={edge.x-20,edge.y-10};MouseAt(h,WM_LBUTTONUP,0,destination);OffsetRect(&roi,-20,-10);
   RequireBounds(h,roi,"border release did not apply its final coordinates");Require(EqualRect(&translated,&roi)!=FALSE,"border drag changed capture size");
  }
  auto beforeClamp=moved;edge=start({1,60});destination={-2000,-2000};MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,destination);MouseAt(h,WM_LBUTTONUP,0,destination);
  roi={monitor.rect.left,monitor.rect.top,monitor.rect.left+440,monitor.rect.top+240};RequireBounds(h,roi,"drag escaped monitor at negative coordinates");Require(moved==beforeClamp+1&&EqualRect(&translated,&roi),"clamped drag did not retain capture dimensions");
  // Move back into the monitor to leave room for an expanded reading frame.
  edge=start({1,60});destination={edge.x+450,edge.y+350};MouseAt(h,WM_LBUTTONUP,0,destination);OffsetRect(&roi,450,350);
  if(spatial){
   auto bar=Windows(L"SAT.TranslationToolbar").front();SendMessageW(bar,WM_COMMAND,1,0);auto toolbarBefore=Bounds(bar);
   edge=start({1,60});destination={edge.x+15,edge.y+10};MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,destination);auto toolbarAfter=Bounds(bar);
   Require(toolbarAfter.left-toolbarBefore.left==15&&toolbarAfter.top-toolbarBefore.top==10,"toolbar did not follow drag");MouseAt(h,WM_LBUTTONUP,0,destination);OffsetRect(&roi,15,10);
   SendMessageW(bar,WM_COMMAND,2,0);auto reading=Bounds(h);Require(sat::Height(reading)>sat::Height(roi),"reading test did not expand the frame");
   edge=start({1,60});destination={edge.x-15,edge.y-10};MouseAt(h,WM_LBUTTONUP,0,destination);OffsetRect(&roi,-15,-10);OffsetRect(&reading,-15,-10);
   RequireBounds(h,reading,"reading frame did not move by the pointer delta");Require(EqualRect(&translated,&roi)!=FALSE,"reading dimensions leaked into translation region");
   SendMessageW(bar,WM_COMMAND,0,0);RequireBounds(h,roi,"returning from reading lost moved capture coordinates");
  }else{
   auto beforeClose=moved;auto close=blocks.front().closeButton;auto point=start({close.right-1,close.top+1});Require(!overlay.Dragging(),"close button started border drag");MouseAt(h,WM_LBUTTONUP,0,point);Require(dismissed==1&&moved==beforeClose,"close button committed drag");
  }
  auto beforeClose=moved;edge=start({1,60});destination={edge.x+20,edge.y+20};MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,destination);overlay.Close();Require(moved==beforeClose&&!overlay.Dragging(),"closing a drag committed its preview");
  overlay.Notice(L"正在翻译",roi,monitor,[]{});h=Windows(L"SAT.Overlay").front();edge=start({1,40});destination={edge.x+30,edge.y+20};MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,destination);MouseAt(h,WM_LBUTTONUP,0,destination);Require(!overlay.Dragging()&&moved==beforeClose,"status notice retained translation drag callback");overlay.Close();
 }
}
class DragBackdrop {
 HWND window_{};
 static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){
  if(m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(h,&ps);RECT r{};GetClientRect(h,&r);auto brush=CreateSolidBrush(static_cast<COLORREF>(GetWindowLongPtrW(h,GWLP_USERDATA)));FillRect(dc,&r,brush);DeleteObject(brush);EndPaint(h,&ps);return 0;}
  return DefWindowProcW(h,m,w,l);
 }
public:
 explicit DragBackdrop(RECT r){
  WNDCLASSW wc{};wc.lpfnWndProc=Proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SAT.DragBackdrop";RegisterClassW(&wc);
  window_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"Drag transparency fixture",WS_POPUP,r.left,r.top,sat::Width(r),sat::Height(r),nullptr,nullptr,wc.hInstance,nullptr);Require(window_!=nullptr,"drag backdrop creation failed");ShowWindow(window_,SW_SHOWNOACTIVATE);
 }
 ~DragBackdrop(){DestroyWindow(window_);}
 void Color(COLORREF color){SetWindowLongPtrW(window_,GWLP_USERDATA,color);InvalidateRect(window_,nullptr,FALSE);UpdateWindow(window_);}
};
bool InteriorShowsBackdrop(HWND window){
 auto r=Bounds(window);int width=sat::Width(r),height=sat::Height(r);DwmFlush();Sleep(40);
 HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=width;bi.bmiHeader.biHeight=-height;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
 void* pixels{};auto bitmap=CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);
 if(!screen||!dc||!bitmap){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);if(screen)ReleaseDC(nullptr,screen);throw std::runtime_error("drag pixel probe allocation failed");}
 auto old=SelectObject(dc,bitmap);bool copied=BitBlt(dc,0,0,width,height,screen,r.left,r.top,SRCCOPY|CAPTUREBLT)!=FALSE;GdiFlush();
 // 屏幕捕获可能包含 HDR 或系统颜色变换；采用同一帧中框外的背景像素作为参照。
 auto color=GetPixel(screen,r.left-8,r.top+20);Require(color!=CLR_INVALID,"backdrop reference pixel unavailable");
 bool clear=true,border=false;auto bytes=static_cast<const unsigned char*>(pixels);
 for(int y=0;y<height;++y)for(int x=0;x<width;++x){auto p=(static_cast<size_t>(y)*width+x)*4;bool match=bytes[p]==GetBValue(color)&&bytes[p+1]==GetGValue(color)&&bytes[p+2]==GetRValue(color);if(x>=6&&x<width-6&&y>=6&&y<height-6)clear&=match;else border|=!match;}
 SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);Require(copied,"drag pixel probe failed");Require(border,"drag preview lost its visible border");return clear;
}
void TestDragTransparency(){
 auto monitor=sat::MonitorAtCursor();monitor.dpi=96;DragBackdrop backdrop(monitor.rect);
 sat::Image image{240,160,std::vector<unsigned char>(240*160*4,245)};sat::TranslationResult result{"en",{{"source","拖动时不应遮住下面的内容",{100,100,800,700}}}};
 RECT roi{monitor.rect.left+80,monitor.rect.top+80,monitor.rect.left+320,monitor.rect.top+240};
 // Check traditional, spatial, source, and expanded reading views against a live background.
 for(int mode=-1;mode<3;++mode){
  auto color=RGB(31,97,149);backdrop.Color(color);sat::Settings settings;settings.spatialOverlay=mode>=0;auto blocks=sat::BuildOverlay(result,image,roi,monitor,settings);
  sat::Overlay overlay;int moved{};overlay.Show(blocks,settings,96,[]{},{},[&](RECT){++moved;});Pump();auto h=Windows(L"SAT.Overlay").front();HWND toolbar=mode>=0?Windows(L"SAT.TranslationToolbar").front():nullptr;if(toolbar)SendMessageW(toolbar,WM_COMMAND,mode,0);
  auto view=Bounds(h);POINT start{view.left+1,view.top+60},end{start.x+20,start.y+15};
  MouseAt(h,WM_LBUTTONDOWN,MK_LBUTTON,start);MouseAt(h,WM_LBUTTONUP,0,start);Require(!InteriorShowsBackdrop(h),"stationary border click hid contents");
  MouseAt(h,WM_LBUTTONDOWN,MK_LBUTTON,start);MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,end);
  if(!InteriorShowsBackdrop(h)){Snapshot(h,L"drag-failure.png");throw std::runtime_error("drag retained screenshot, text, or background pixels");}Require(!toolbar||!IsWindowVisible(toolbar),"toolbar obstructs the drag preview");Require(moved==0,"clearing drag contents submitted translation");
  color=RGB(183,61,37);backdrop.Color(color);Require(InteriorShowsBackdrop(h),"drag shows a frozen backdrop instead of live transparency");
  SendMessageW(h,WM_CANCELMODE,0,0);RequireBounds(h,view,"cancel changed the original position");Require(!InteriorShowsBackdrop(h),"cancel failed to restore contents");Require(!toolbar||IsWindowVisible(toolbar),"cancel failed to restore toolbar");
  MouseAt(h,WM_LBUTTONDOWN,MK_LBUTTON,start);MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,end);MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,start);MouseAt(h,WM_LBUTTONUP,0,start);
  Require(!InteriorShowsBackdrop(h)&&moved==0,"round-trip drag did not restore contents without translating");Require(!toolbar||IsWindowVisible(toolbar),"round-trip drag failed to restore toolbar");
  MouseAt(h,WM_LBUTTONDOWN,MK_LBUTTON,start);MouseAt(h,WM_MOUSEMOVE,MK_LBUTTON,end);MouseAt(h,WM_LBUTTONUP,0,end);
  Require(moved==1&&InteriorShowsBackdrop(h),"release flashed the old contents before retranslation");overlay.Close();
 }
}
sat::Image InstallerFixture(){
 sat::ComPtr<IWICImagingFactory> factory;sat::CheckHR(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"test");
 sat::ComPtr<IWICBitmapDecoder> decoder;sat::CheckHR(factory->CreateDecoderFromFilename(L"test-fixtures/installer-en.png",nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder),"test");
 sat::ComPtr<IWICBitmapFrameDecode> frame;sat::CheckHR(decoder->GetFrame(0,&frame),"test");sat::ComPtr<IWICFormatConverter> converter;sat::CheckHR(factory->CreateFormatConverter(&converter),"test");sat::CheckHR(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"test");
 UINT w{},h{};converter->GetSize(&w,&h);sat::Image image{int(w),int(h),std::vector<unsigned char>(size_t(w)*h*4)};sat::CheckHR(converter->CopyPixels(nullptr,w*4,UINT(image.bgra.size()),image.bgra.data()),"test");return image;
}
void TestCrowdedLayouts(){
 auto image=InstallerFixture();sat::Settings settings;sat::Monitor monitor{L"fixture",{0,0,1600,1000},96};RECT roi{100,100,100+image.width,100+image.height};
 sat::TranslationResult result{"en",{
  {"Select Additional Tasks","选择附加任务",{40,88,350,6}},
  {"Which additional tasks should be performed?","需要执行哪些附加任务？",{65,131,500,10}},
  {"Select the additional tasks you would like Setup to perform while installing Screen AI Translator, then click Next.","选择安装屏幕翻译时需要执行的附加任务，然后点击“下一步”。",{70,226,850,18}},
  {"Background operation:","后台运行选项：",{70,321,360,8}},
  {"Start quietly when I sign in to Windows","登录 Windows 后在后台启动",{100,366,430,10}},
  {"Create a desktop shortcut","创建桌面快捷方式",{100,463,450,10}},
  {"Back / Next / Cancel","上一步 / 下一步 / 取消",{550,920,420,30}}
 }};
 auto checkFit=[&](const sat::OverlayBlock& block){
  for(size_t i=0;i<block.positioned.size();++i){const auto& item=block.positioned[i];DWRITE_TEXT_METRICS m{};item.layout->GetMetrics(&m);
   Require(item.overflow||(m.height+2*item.padding<=sat::Height(item.rect)+.5f&&m.widthIncludingTrailingWhitespace+2*item.padding<=sat::Width(item.rect)+.5f),"clipped text lacks overflow indication");
   Require(std::abs(item.rect.top-item.sourceRect.top)<=4+int(std::ceil((item.detectedLines.empty()?0:sat::Height(item.detectedLines.front()))*.18f))&&std::abs(item.rect.left-item.sourceRect.left)<=4,"fallback moved a passage to another location");
   Require(item.eraseHeight==sat::Height(item.sourceRect),"translation height expanded source removal");
   Require(item.rect.left>=0&&item.rect.top>=0&&item.rect.right<=image.width&&item.rect.bottom<=image.height,"expanded cover escaped capture");
   for(size_t j=0;j<i;++j){RECT overlap{};Require(item.suppressed||block.positioned[j].suppressed||!IntersectRect(&overlap,&item.rect,&block.positioned[j].rect),"visible translation covers overlap");}
  }
 };
 auto blocks=sat::BuildOverlay(result,image,roi,monitor,settings);checkFit(blocks[0]);
 sat::Overlay overlay;overlay.Show(blocks,settings,96,[]{});Pump();auto h=Windows(L"SAT.Overlay").front();auto bar=Windows(L"SAT.TranslationToolbar").front();
 RECT actual{};GetWindowRect(h,&actual);Require(EqualRect(&actual,&roi)!=FALSE,"tiny boxes automatically changed view");
 auto before=Snapshot(h,L"installer-height-expanded.png");Snapshot(bar,L"installer-height-toolbar.png");
 SendMessageW(bar,WM_COMMAND,1,0);auto original=Snapshot(h,L"installer-original.png");Require(before!=original,"spatial translation missing");SendMessageW(bar,WM_COMMAND,0,0);auto restored=Snapshot(h,L"installer-height-restored.png");Require(before==restored,"spatial view was not restored");
 SendMessageW(bar,WM_COMMAND,2,0);Snapshot(h,L"installer-manual-reading.png");SendMessageW(bar,WM_COMMAND,0,0);GetWindowRect(h,&actual);Require(EqualRect(&actual,&roi)!=FALSE,"manual reading could not return to spatial view");overlay.Close();
 monitor.dpi=192;auto highDpi=sat::BuildOverlay(result,image,roi,monitor,settings);checkFit(highDpi[0]);monitor.dpi=96;
 auto crowded=result;for(size_t i=0;i<crowded.segments.size();++i){crowded.segments[i].box.y=60+int(i)*35;crowded.segments[i].box.height=6;}checkFit(sat::BuildOverlay(crowded,image,roi,monitor,settings)[0]);

 result.segments={ {"a","第一段完整译文",{100,100,300,100}}, {"b","第二段完整译文",{120,120,300,100}} };checkFit(sat::BuildOverlay(result,image,roi,monitor,settings)[0]);
 settings.autoFont=false;settings.fontSize=70;result.segments={{"button","无法在小按钮中容纳的完整译文",{900,950,90,40}}};auto large=sat::BuildOverlay(result,image,roi,monitor,settings);checkFit(large[0]);Require(large[0].positioned[0].fontSize==70,"fixed font unexpectedly shrank");
 settings.fontSize=18;for(int i=0;i<100;++i)result.segments[0].translated+="很长的内容仍在这个区域内阅读。";
 auto longBlock=sat::BuildOverlay(result,image,roi,monitor,settings);Require(longBlock[0].positioned[0].overflow,"long local block lacks scroll extent");overlay.Show(longBlock,settings,96,[]{});Pump();h=Windows(L"SAT.Overlay").front();auto top=Snapshot(h,L"spatial-local-scroll-top.png");
 auto r=longBlock[0].positioned[0].rect;SendMessageW(h,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-12000)),MAKELPARAM(roi.left+(r.left+r.right)/2,roi.top+(r.top+r.bottom)/2));auto bottom=Snapshot(h,L"spatial-local-scroll-bottom.png");Require(top!=bottom,"local overflow did not scroll");GetWindowRect(h,&actual);Require(EqualRect(&actual,&roi)!=FALSE,"local scroll changed mode");overlay.Close();
 Require(sat::ReadableTextColor(RGB(250,250,250),RGB(245,245,245))==RGB(0,0,0),"low contrast white-on-light text retained");
 Require(sat::ReadableTextColor(RGB(25,25,25),RGB(20,20,20))==RGB(255,255,255),"low contrast dark-on-dark text retained");

}
void TestProgress(){
 auto monitor=sat::MonitorAtCursor();DragBackdrop backdrop(monitor.rect);sat::Overlay progress;
 RECT roi{monitor.rect.left+80,monitor.rect.top+80,monitor.rect.left+500,monitor.rect.top+300};
 int dismissed{};
 for(UINT dpi:{96u,144u,192u}){
  monitor.dpi=dpi;backdrop.Color(RGB(31,97,149));progress.Progress(roi,monitor,[&]{++dismissed;});
  auto frame=Windows(L"SAT.ProgressFrame").front();auto notice=Windows(L"SAT.Overlay").front();
  Require(IsWindowVisible(frame)&&IsWindowVisible(notice),"processing UI is not immediately visible");RequireBounds(frame,roi,"processing border changed selection geometry");
  Require(InteriorShowsBackdrop(frame),"processing frame obscures source pixels");backdrop.Color(RGB(91,143,67));Require(InteriorShowsBackdrop(frame),"processing frame freezes the desktop");
  auto noticeBounds=Bounds(notice);RECT overlap{};Require(!IntersectRect(&overlap,&noticeBounds,&roi),"processing notice covers source despite available space");
  if(dpi==96){Snapshot(frame,L"processing-frame.png");Snapshot(notice,L"processing-notice.png");}
  // Verify the status window's close control remains usable at each scale.
  auto x=sat::Width(noticeBounds)-5;SendMessageW(notice,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,5));SendMessageW(notice,WM_LBUTTONUP,0,MAKELPARAM(x,5));
  progress.Close();Require(Windows(L"SAT.ProgressFrame").empty()&&Windows(L"SAT.Overlay").empty(),"processing UI survived close");
 }
 Require(dismissed==3,"processing cancel control failed");
}
void TestSelection(){
 sat::Monitor monitor{L"test",{40,60,640,460},96};auto image=std::make_shared<sat::Image>(sat::Image{600,400,std::vector<unsigned char>(600*400*4,150)});sat::Selection selection;std::optional<RECT> chosen;int callbacks{};
 selection.Show(monitor,image,[&](auto r){Require(IsWindowVisible(selection.Window()),"selection disappeared before replacement callback");chosen=r;++callbacks;});Pump();HWND h=selection.Window();SendMessageW(h,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(120,130));SendMessageW(h,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(420,290));SendMessageW(h,WM_LBUTTONUP,0,MAKELPARAM(420,290));Require(callbacks==1&&chosen.has_value(),"release failed to select");Require(chosen->left==160&&chosen->top==190&&chosen->right==460&&chosen->bottom==350,"selection physical offset mismatch");Require(!selection.Window(),"selection not destroyed after handoff");
 selection.Show(monitor,image,[&](auto r){chosen=r;++callbacks;});SendMessageW(selection.Window(),WM_KEYDOWN,VK_ESCAPE,0);Require(callbacks==2&&!chosen,"Esc did not cancel");
}

}
int main(){try{SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);sat::ComScope com;INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_WIN95_CLASSES|ICC_HOTKEY_CLASS};InitCommonControlsEx(&cc);TestLayout();TestOverlay();TestSpatialOverlay();TestOverlayDragging();TestDragTransparency();TestCrowdedLayouts();TestProgress();TestSelection();std::cout<<"NativeTests: PASS ("<<checks<<" checks)\n";return 0;}catch(const std::exception& e){std::cerr<<"NativeTests: FAIL: "<<e.what()<<"\n";return 1;}}
