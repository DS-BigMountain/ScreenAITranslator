#include "common/Platform.h"
#include "overlay/Overlay.h"
#include "capture/Capture.h"
#include "overlay/SpatialLayout.h"
#include "selection/Selection.h"
#include "settings/SettingsWindow.h"
#include "settings/WinUIRuntime.h"
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <commctrl.h>
#include <iostream>
#include <algorithm>
#include <dwrite.h>
#include <dwmapi.h>
#include <wincodec.h>
namespace {
int checks{};
void Require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void Pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){if(sat::WinUIRuntime::ProcessMessage(m))continue;TranslateMessage(&m);DispatchMessageW(&m);}}
unsigned long long Snapshot(HWND window,const std::wstring& name){
 RECT r{};GetWindowRect(window,&r);HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,sat::Width(r),sat::Height(r));ReleaseDC(nullptr,screen);auto old=SelectObject(dc,bitmap);SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);UpdateWindow(window);Sleep(100);bool layered=true;HDC ownWindow=layered?GetDC(nullptr):GetWindowDC(window);Require(BitBlt(dc,0,0,sat::Width(r),sat::Height(r),ownWindow,layered?r.left:0,layered?r.top:0,SRCCOPY|CAPTUREBLT)!=FALSE,"cannot inspect native settings rendering");GdiFlush();ReleaseDC(layered?nullptr:window,ownWindow);SetWindowPos(window,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
 sat::ComPtr<IWICImagingFactory> factory;sat::CheckHR(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"test");sat::ComPtr<IWICBitmap> image;sat::CheckHR(factory->CreateBitmapFromHBITMAP(bitmap,nullptr,WICBitmapIgnoreAlpha,&image),"test");SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
 std::filesystem::create_directories("test-output");auto path=std::filesystem::path("test-output")/name;sat::ComPtr<IWICStream> stream;sat::CheckHR(factory->CreateStream(&stream),"test");sat::CheckHR(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"test");sat::ComPtr<IWICBitmapEncoder> encoder;sat::CheckHR(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"test");sat::CheckHR(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"test");sat::ComPtr<IWICBitmapFrameEncode> frame;sat::CheckHR(encoder->CreateNewFrame(&frame,nullptr),"test");sat::CheckHR(frame->Initialize(nullptr),"test");sat::CheckHR(frame->WriteSource(image.Get(),nullptr),"test");sat::CheckHR(frame->Commit(),"test");sat::CheckHR(encoder->Commit(),"test");
 std::vector<unsigned char> pixels(static_cast<size_t>(sat::Width(r))*sat::Height(r)*4);sat::CheckHR(image->CopyPixels(nullptr,sat::Width(r)*4,static_cast<UINT>(pixels.size()),pixels.data()),"test");unsigned long long hash=1469598103934665603ULL;for(auto pixel:pixels){hash^=pixel;hash*=1099511628211ULL;}return hash;
}
void TestSettings(){
 namespace xaml=winrt::Microsoft::UI::Xaml;
 using namespace xaml::Controls;
 sat::SettingsWindow window;sat::Settings options;options.startup=false;sat::PersistentState state;sat::SettingsCallbacks cb;
 int saved{},fetched{},tested{},actions{},closed{};bool reject{};
 cb.apply=[&](auto& value,auto& key){Require(value.model=="test-vision"&&key=="test-only-key"&&value.thinkingHigh&&value.ocrMode==2&&value.font==L"SimSun"&&!value.matchTextColor,"saved API values differ from controls");++saved;return reject?std::wstring(L"保存被测试拒绝"):std::wstring{};};
 cb.action=[&](auto){++actions;};cb.closed=[&](RECT r,int page){Require(sat::Valid(r)&&page==2,"window position or navigation not preserved");++closed;};
 auto pump=[](){for(int i=0;i<20;++i){Pump();Sleep(25);}};
 auto click=[&](const wchar_t* name){auto button=window.Content().FindName(name).as<Button>();xaml::Automation::Peers::ButtonAutomationPeer peer(button);peer.GetPattern(xaml::Automation::Peers::PatternInterface::Invoke).as<xaml::Automation::Provider::IInvokeProvider>().Invoke();pump();};
 cb.api=[&](bool fetch,const sat::Settings& value,const std::string& key){Require(key=="test-only-key","API button uses wrong key");if(fetch){++fetched;window.ApiFinished({"test-vision"},L"已获取模型",true);}else{++tested;Require(value.model=="test-vision"&&value.thinkingHigh,"API test ignores model or thinking preference");window.ApiFinished({},L"测试通过",false);}};
 window.Show(options,"test-only-key",state,cb);pump();auto h=window.Window();Require(h!=nullptr,"settings missing");auto root=window.Content();
 auto nav=root.FindName(L"Navigation").as<NavigationView>();Require(nav.ActualWidth()>800&&nav.ActualHeight()>500,"WinUI content has no visible layout");Require(root.FindName(L"Translate").as<Button>().ActualWidth()>0,"translation button template is not visible");Require(nav.MenuItems().Size()==5,"settings navigation pages missing");
 auto password=root.FindName(L"Key").as<PasswordBox>();Require(password.PasswordRevealMode()==PasswordRevealMode::Peek,"API key is not masked by default");
 for(int page=0;page<5;++page){nav.SelectedItem(nav.MenuItems().GetAt(page));pump();auto panel=root.FindName(L"Page"+std::to_wstring(page)).as<StackPanel>();Require(panel.Visibility()==xaml::Visibility::Visible,"navigation failed to display selected page");Snapshot(h,L"settings-"+std::to_wstring(page)+L".png");}
 nav.SelectedItem(nav.MenuItems().GetAt(2));pump();
 auto model=root.FindName(L"Model").as<ComboBox>();Require(model.IsEditable(),"manual model entry disabled");Require(model.Text()==sat::Wide(options.model),"saved model is not displayed on first load");
 auto ocr=root.FindName(L"OcrMode").as<ComboBox>();Require(ocr.Items().Size()==4&&ocr.SelectedIndex()==0,"OCR modes or default incorrect");ocr.SelectedIndex(2);
 auto font=root.FindName(L"Font").as<ComboBox>();Require(font.IsEditable()&&font.Items().Size()>=10&&font.SelectedIndex()>=0,"common font dropdown unavailable");Require(font.Text()==options.font,"saved font was lost");font.SelectedIndex(2);
 auto match=root.FindName(L"MatchTextColor").as<ToggleSwitch>();Require(match.IsOn(),"source color matching not enabled by default");match.IsOn(false);
 Require(winrt::unbox_value<winrt::hstring>(root.FindName(L"TranslateFixed").as<Button>().Content())==L"翻译上次区域","last-region label not updated");
 auto thinking=root.FindName(L"ThinkingMode").as<ComboBox>();Require(thinking.Items().Size()==2,"thinking choices changed");thinking.SelectedIndex(1);
 click(L"FetchModels");Require(fetched==1&&model.Items().Size()==1,"model discovery is not connected");click(L"TestApi");Require(tested==1,"API test is not connected");
 click(L"Apply");Require(saved==1,"settings save failed");
 window.ApiBusy(true);Require(!root.FindName(L"FetchModels").as<Button>().IsEnabled()&&!root.FindName(L"Apply").as<Button>().IsEnabled(),"busy API permits duplicate requests or save");window.ApiFinished({},L"测试完成",false);Require(root.FindName(L"Apply").as<Button>().IsEnabled(),"completion leaves save disabled");
 password.Password(L"changed-key");pump();Require(model.Items().Size()==0&&model.Text().empty(),"credential changes retain stale model choices");password.Password(L"test-only-key");pump();model.Text(L"test-vision");click(L"Apply");Require(saved==2,"manual model cannot be saved");
 auto textColor=root.FindName(L"TextColor").as<TextBox>();textColor.Text(L"wrong");click(L"Apply");Require(saved==2,"invalid color reached persistence callback");textColor.Text(L"FFFFFF");
 reject=true;click(L"Apply");Require(saved==3&&root.FindName(L"Status").as<TextBlock>().Text()==L"保存被测试拒绝","save failure was not retained in the interface");reject=false;
 nav.SelectedItem(nav.MenuItems().GetAt(0));click(L"Translate");click(L"TranslateFixed");click(L"Reselect");click(L"ClearRegion");Require(actions==4,"translation actions disconnected");
 nav.SelectedItem(nav.MenuItems().GetAt(3));click(L"ClearContext");Require(actions==5,"clear context disconnected");
 nav.SelectedItem(nav.MenuItems().GetAt(2));window.Show(options,"different-key",state,cb);Require(password.Password()==L"test-only-key","reopening visible window discarded unsaved input");
 // 关闭后释放控件引用，验证再次创建窗口的资源生命周期。
 font=nullptr;match=nullptr;ocr=nullptr;model=nullptr;password=nullptr;thinking=nullptr;textColor=nullptr;nav=nullptr;root=nullptr;
 click(L"Close");Require(!window.Window()&&closed==1,"close did not persist window state");
 for(int i=0;i<3;++i){window.Show(options,"test-only-key",state,cb);pump();Require(window.Content()!=nullptr,"window reopen failed");window.Close();pump();}
}
}
int main(){try{SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);sat::WinUIRuntime ui;TestSettings();std::cout<<"WinUISettingsTests: PASS ("<<checks<<" checks)\n";return 0;}catch(const winrt::hresult_error& e){std::wcerr<<L"WinUI failure: "<<e.code()<<L" "<<e.message().c_str()<<L"\n";return 1;}catch(const std::exception& e){std::cerr<<"WinUISettingsTests: FAIL: "<<e.what()<<"\n";return 1;}}
