#include "common/Platform.h"
#include "overlay/Overlay.h"
#include "overlay/SpatialLayout.h"
#include "selection/Selection.h"
#include "settings/SettingsWindow.h"
#include <commctrl.h>
#include <iostream>
#include <algorithm>
#include <dwrite.h>
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
 auto& items=blocks[0].positioned;Require(items[0].rect.left==30&&items[0].rect.top==30&&items[1].rect.left==450,"normalized locations not mapped to physical pixels");Require(!items[2].overflow&&sat::Height(items[2].rect)>sat::Height(items[2].sourceRect),"long spatial text should expand into free space");Require(items[2].fontSize>=12,"auto layout shrank text below legibility floor");
 sat::Overlay overlay;int dismissed{},reselected{};overlay.Show(blocks,settings,96,[&]{++dismissed;},[&]{++reselected;});Pump();auto h=Windows(L"SAT.Overlay").front();auto bars=Windows(L"SAT.TranslationToolbar");Require(bars.size()==1,"spatial toolbar missing");auto bar=bars.front();
 auto translated=Snapshot(h,L"spatial-translated.png");Snapshot(bar,L"spatial-toolbar.png");SendMessageW(bar,WM_COMMAND,1,0);auto original=Snapshot(h,L"spatial-original.png");Require(original!=translated,"source toggle did not change image");
 SendMessageW(bar,WM_COMMAND,2,0);RECT reading{};GetWindowRect(h,&reading);Require(sat::Height(reading)>=320,"reading view did not expand small capture");Snapshot(h,L"spatial-reading.png");
 SendMessageW(bar,WM_COMMAND,0,0);RECT restored{};GetWindowRect(h,&restored);Require(EqualRect(&restored,&roi)!=FALSE,"spatial view did not restore exact selection coordinates");
 SendMessageW(bar,WM_COMMAND,4,0);Require(reselected==1,"reselect action not connected");SendMessageW(bar,WM_COMMAND,5,0);Require(dismissed==1,"toolbar close action not connected");overlay.Close();Require(Windows(L"SAT.TranslationToolbar").empty(),"toolbar leaked on close");
 sat::Monitor negative{L"negative",{-1920,-1080,0,0},144};RECT tinyRegion{-1900,-100,-1850,-50};auto edge=sat::BuildOverlay(result,image,tinyRegion,negative,settings);Require(EqualRect(&edge[0].screen,&tinyRegion)!=FALSE,"negative screen position changed");
 result.segments={{"bad","边界",{-500,-500,5000,5000}}};auto clamped=sat::BuildOverlay(result,image,roi,monitor,settings);auto rect=clamped[0].positioned[0].rect;Require(rect.left==0&&rect.top==0&&rect.right==600&&rect.bottom==300,"out-of-range model box was not clamped");
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
   Require(m.height+2*item.padding<=sat::Height(item.rect)+.5f,"measured text height exceeds expanded cover");
   Require(m.widthIncludingTrailingWhitespace+2*item.padding<=sat::Width(item.rect)+.5f,"measured text width exceeds expanded cover");
   Require(item.rect.left>=0&&item.rect.top>=0&&item.rect.right<=image.width&&item.rect.bottom<=image.height,"expanded cover escaped capture");
   for(size_t j=0;j<i;++j){RECT overlap{};Require(!IntersectRect(&overlap,&item.rect,&block.positioned[j].rect),"expanded translation covers overlap");}
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
void TestSelection(){
 sat::Monitor monitor{L"test",{40,60,640,460},96};auto image=std::make_shared<sat::Image>(sat::Image{600,400,std::vector<unsigned char>(600*400*4,150)});sat::Selection selection;std::optional<RECT> chosen;int callbacks{};
 selection.Show(monitor,image,[&](auto r){chosen=r;++callbacks;});Pump();HWND h=selection.Window();SendMessageW(h,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(120,130));SendMessageW(h,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(420,290));SendMessageW(h,WM_LBUTTONUP,0,MAKELPARAM(420,290));Require(callbacks==1&&chosen.has_value(),"release failed to select");Require(chosen->left==160&&chosen->top==190&&chosen->right==460&&chosen->bottom==350,"selection physical offset mismatch");Require(!selection.Window(),"selection not destroyed before completion");
 selection.Show(monitor,image,[&](auto r){chosen=r;++callbacks;});SendMessageW(selection.Window(),WM_KEYDOWN,VK_ESCAPE,0);Require(callbacks==2&&!chosen,"Esc did not cancel");
}
void TestSettings(){
 sat::SettingsWindow window;sat::Settings options;sat::PersistentState state;sat::SettingsCallbacks cb;bool saved{};cb.apply=[&](auto& value,auto& key){saved=value.model=="test-vision"&&key=="test-only-key"&&value.thinkingHigh;return std::wstring{};};cb.action=[](auto){};window.Show(options,"test-only-key",state,cb);Pump();auto h=window.Window();Require(h!=nullptr,"settings missing");Require(GetWindowLongPtrW(GetDlgItem(h,142),GWL_STYLE)&ES_PASSWORD,"key not masked");
 auto tabs=FindWindowExW(h,nullptr,WC_TABCONTROLW,nullptr);for(int page=0;page<5;++page){TabCtrl_SetCurSel(tabs,page);NMHDR notification{tabs,0,TCN_SELCHANGE};SendMessageW(h,WM_NOTIFY,0,reinterpret_cast<LPARAM>(&notification));RedrawWindow(h,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);Pump();Sleep(200);Pump();Snapshot(h,L"settings-"+std::to_wstring(page)+L".png");}
 Require(TabCtrl_GetItemCount(tabs)==5,"history tab remains");
 Require((GetWindowLongPtrW(GetDlgItem(h,144),GWL_STYLE)&3)==CBS_DROPDOWN,"model name cannot be entered manually");
 Require(GetDlgItem(h,103)==nullptr,"region persistence checkbox remains");Require(SendMessageW(GetDlgItem(h,149),CB_GETCOUNT,0,0)==2,"thinking mode must have exactly two choices");SendMessageW(GetDlgItem(h,149),CB_SETCURSEL,1,0);
 window.ApiFinished({"test-vision"},L"已获取测试模型",true);SendMessageW(h,WM_COMMAND,MAKEWPARAM(220,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(h,220)));Require(saved,"settings save did not read native controls");SendMessageW(h,WM_COMMAND,MAKEWPARAM(143,BN_CLICKED),0);Require(SendMessageW(GetDlgItem(h,142),EM_GETPASSWORDCHAR,0,0)==0,"key reveal failed");
 window.ApiBusy(true);Require(!IsWindowEnabled(GetDlgItem(h,146))&&!IsWindowEnabled(GetDlgItem(h,220)),"busy API permits duplicate requests or save");window.ApiFinished({},L"测试完成",false);Require(IsWindowEnabled(GetDlgItem(h,146)),"API completion leaves controls disabled");
 SetWindowTextW(GetDlgItem(h,142),L"changed-key");Require(SendMessageW(GetDlgItem(h,144),CB_GETCOUNT,0,0)==0,"credential change retains stale model list");
 SetWindowTextW(GetDlgItem(h,144),L"test-vision");SetWindowTextW(GetDlgItem(h,142),L"test-only-key");SetWindowTextW(GetDlgItem(h,144),L"test-vision");saved=false;
 SendMessageW(h,WM_COMMAND,MAKEWPARAM(220,BN_CLICKED),0);Require(saved,"manual model cannot be saved without model discovery");
 window.Close();Require(!window.Window(),"settings close failed");
 int fetched{},tested{};cb.api=[&](bool fetch,const sat::Settings& value,const std::string& key){Require(key=="test-only-key","API button uses wrong key");if(fetch){++fetched;window.ApiFinished({"second","test-vision"},L"已获取",true);}else{++tested;Require(value.model=="second"&&value.thinkingHigh,"API test ignores model or thinking preference");window.ApiFinished({},L"测试通过",false);}};
 window.Show(options,"test-only-key",state,cb);h=window.Window();SendMessageW(GetDlgItem(h,149),CB_SETCURSEL,1,0);SendMessageW(h,WM_COMMAND,MAKEWPARAM(146,BN_CLICKED),0);Require(fetched==1&&SendMessageW(GetDlgItem(h,144),CB_GETCOUNT,0,0)==2,"fetch button fails to populate list");SendMessageW(GetDlgItem(h,144),CB_SETCURSEL,0,0);SendMessageW(h,WM_COMMAND,MAKEWPARAM(147,BN_CLICKED),0);Require(tested==1,"test API button not connected");window.Close();
}
}
int main(){try{SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);sat::ComScope com;INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_WIN95_CLASSES|ICC_HOTKEY_CLASS};InitCommonControlsEx(&cc);TestLayout();TestOverlay();TestSpatialOverlay();TestCrowdedLayouts();TestSelection();TestSettings();std::cout<<"NativeTests: PASS ("<<checks<<" checks)\n";return 0;}catch(const std::exception& e){std::cerr<<"NativeTests: FAIL: "<<e.what()<<"\n";return 1;}}






