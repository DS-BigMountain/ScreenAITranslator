#include "settings/SettingsWindow.h"
#include "common/Platform.h"
#include "settings/WinUIRuntime.h"
#include "settings/Store.h"
#include "providers/Catalog.h"
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <iostream>
#include <psapi.h>
#include <shellapi.h>
#include <propsys.h>
#include <propkey.h>
using namespace sat;
namespace xaml=winrt::Microsoft::UI::Xaml;
using namespace xaml::Controls;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Pump(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(WinUIRuntime::ProcessMessage(message))continue;TranslateMessage(&message);DispatchMessageW(&message);}}
void CheckWindowIcons(HWND window){
 auto dpi=GetDpiForWindow(window);
 for(bool compact:{false,true}){
  auto width=GetSystemMetricsForDpi(compact?SM_CXSMICON:SM_CXICON,dpi),height=GetSystemMetricsForDpi(compact?SM_CYSMICON:SM_CYICON,dpi);
  auto expected=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101),IMAGE_ICON,width,height,LR_SHARED));
  auto actual=reinterpret_cast<HICON>(SendMessageW(window,WM_GETICON,compact?ICON_SMALL:ICON_BIG,0));
  Check(expected&&actual==expected,compact?"title bar icon does not use the current app resource":"taskbar icon does not use the current app resource");
  ICONINFO info{};Check(GetIconInfo(actual,&info)!=FALSE,"window icon cannot be inspected");BITMAP bitmap{};auto bytes=GetObjectW(info.hbmColor,sizeof(bitmap),&bitmap);if(info.hbmColor)DeleteObject(info.hbmColor);if(info.hbmMask)DeleteObject(info.hbmMask);
  Check(bytes==sizeof(bitmap)&&bitmap.bmWidth==width&&bitmap.bmHeight==height,"window icon dimensions do not match DPI");
 }
}
std::vector<unsigned char> IconPixels(HICON icon,int size){
 HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=size;info.bmiHeader.biHeight=-size;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* bits{};
 auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);Check(bitmap&&bits,"cannot create offscreen icon surface");auto old=SelectObject(dc,bitmap);memset(bits,255,static_cast<size_t>(size)*size*4);
 auto drawn=DrawIconEx(dc,0,0,icon,size,size,0,nullptr,DI_NORMAL);GdiFlush();auto data=static_cast<unsigned char*>(bits);std::vector<unsigned char> pixels(data,data+static_cast<size_t>(size)*size*4);
 SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);Check(drawn!=FALSE,"cannot render icon offscreen");return pixels;
}
void CheckTaskbarIdentity(HWND window){
 ComPtr<IPropertyStore> properties;Check(SUCCEEDED(SHGetPropertyStoreForWindow(window,IID_PPV_ARGS(&properties))),"cannot read taskbar property store");
 auto text=[&](const PROPERTYKEY& key){PROPVARIANT value{};Check(SUCCEEDED(properties->GetValue(key,&value)),"cannot read taskbar property");bool valid=value.vt==VT_LPWSTR&&value.pwszVal;std::wstring result=valid?value.pwszVal:L"";PropVariantClear(&value);Check(valid,"taskbar property is missing");return result;};
 Check(text(PKEY_AppUserModel_ID)==L"ScreenAITranslator.Desktop","taskbar group has no explicit app identity");
 auto resource=text(PKEY_AppUserModel_RelaunchIconResource);Check(resource.ends_with(L",0"),"taskbar icon is not an explicit ICO file");std::filesystem::path path(resource.substr(0,resource.size()-2));
 Check(path.filename().wstring().starts_with(L"ScreenAITranslator-icon-")&&std::filesystem::is_regular_file(path),"taskbar icon asset is missing or has no content identity");
 std::wstring executable(32768,0);executable.resize(GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size())));
 Check(path.parent_path()==std::filesystem::path(executable).parent_path(),"taskbar icon does not belong to this application");
 Check(text(PKEY_AppUserModel_RelaunchCommand)==L"\""+executable+L"\""&&text(PKEY_AppUserModel_RelaunchDisplayNameResource)==L"@"+executable+L",-301","taskbar shortcut relaunch metadata is incorrect");
 wchar_t name[64]{};Check(LoadStringW(GetModuleHandleW(nullptr),301,name,64)>0&&std::wstring(name)==L"屏幕翻译","taskbar display name resource is missing");
 SHFILEINFOW shell{};Check(SHGetFileInfoW(path.c_str(),0,&shell,sizeof(shell),SHGFI_ICON|SHGFI_SMALLICON)!=0&&shell.hIcon,"Shell cannot load the taskbar icon");
 int size=GetSystemMetrics(SM_CXSMICON);auto expected=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101),IMAGE_ICON,size,size,LR_SHARED));Check(expected!=nullptr,"expected icon resource is missing");
 auto actual=IconPixels(shell.hIcon,size);DestroyIcon(shell.hIcon);auto reference=IconPixels(expected,size);unsigned long long colorError{};for(size_t i=0;i<actual.size();++i)if(i%4!=3)colorError+=std::abs(int(actual[i])-int(reference[i]));
 std::cout<<"Shell icon mean RGB difference: "<<double(colorError)/(size*size*3)<<" / 255\n";
 // Shell extraction can resample a different ICO frame; allow bounded RGB rasterization error.
 Check(double(colorError)/(size*size*3)<8.0,"Shell taskbar icon differs from the current embedded icon");
 auto tray=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(102),IMAGE_ICON,size,size,LR_SHARED));Check(tray!=nullptr&&IconPixels(tray,size)!=reference,"dedicated enlarged tray icon is missing");
}
SIZE_T ReportMemory(const char* stage){PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);Check(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)),"memory query failed");std::cout<<stage<<": private="<<memory.PrivateUsage/1048576.0<<" MiB, working_set="<<memory.WorkingSetSize/1048576.0<<" MiB\n";return memory.PrivateUsage;}
int main(){try{
 WinUIRuntime runtime(true);Check(!WinUIRuntime::Initialized(),"background startup eagerly initialized XAML");MSG empty{};Check(!WinUIRuntime::ProcessMessage(empty),"inactive XAML intercepted messages");ReportMemory("Deferred UI startup");{
 SettingsWindow window;Settings saved;std::string savedKey;int saves=0;std::vector<SettingsAction> actions;
 SettingsCallbacks callbacks;callbacks.apply=[&](const Settings& value,const std::string& key){saved=value;savedKey=key;++saves;return std::wstring{};};
 callbacks.action=[&](SettingsAction action){actions.push_back(action);};
 window.Show(Settings{},"original-key",{},callbacks,false);Pump();Check(WinUIRuntime::Initialized(),"settings did not initialize XAML on demand");Check(!IsWindowVisible(window.Window()),"configuration test must remain hidden");
 CheckWindowIcons(window.Window());
 CheckTaskbarIdentity(window.Window());
 auto root=window.Content();auto provider=root.FindName(L"Provider").as<ComboBox>();Check(provider.Items().Size()==ProviderPresets().size(),"provider menu incomplete");
 Check(!root.FindName(L"Reselect")&&!root.FindName(L"ClearRegion"),"removed region controls still exist");
 auto home=root.FindName(L"Page0").as<StackPanel>();Check(home.Children().Size()==2,"home must contain only translation and runtime preferences cards");
 for(const wchar_t* name:{L"Translate",L"TranslateFixed",L"ClearContext"}){xaml::Automation::Peers::ButtonAutomationPeer actionPeer(root.FindName(name).as<Button>());actionPeer.GetPattern(xaml::Automation::Peers::PatternInterface::Invoke).as<xaml::Automation::Provider::IInvokeProvider>().Invoke();Pump();}
 Check(actions==std::vector<SettingsAction>{SettingsAction::Translate,SettingsAction::FixedTranslate,SettingsAction::ClearContext},"remaining settings actions are disconnected or misrouted");
 auto language=root.FindName(L"OcrMode").as<ComboBox>();Check(language.Items().Size()==4&&language.SelectedIndex()==0,"language menu or automatic default incorrect");language.SelectedIndex(3);
 Check(winrt::unbox_value<winrt::hstring>(language.SelectedItem().as<ComboBoxItem>().Content())==L"韩语","fourth language choice is not Korean");
 auto primaryKey=root.FindName(L"Key").as<PasswordBox>();auto model=root.FindName(L"Model").as<ComboBox>();
 model.Text(L"another-deepseek-model");Pump();Check(primaryKey.Password()==L"original-key","model selection cleared key");
 root.FindName(L"BaseUrl").as<TextBox>().Text(L"https://custom.example/v1");model.Text(L"another-deepseek-model");
 provider.SelectedIndex(10);Pump();Check(root.FindName(L"Key").as<PasswordBox>().Password().empty(),"provider switch retained another provider key");Check(root.FindName(L"BaseUrl").as<TextBox>().Text()==L"https://api-free.deepl.com/v2","preset endpoint not applied");
 root.FindName(L"Key").as<PasswordBox>().Password(L"primary-test-key");
 provider.SelectedIndex(0);Pump();Check(primaryKey.Password()==L"original-key"&&model.Text()==L"another-deepseek-model"&&root.FindName(L"BaseUrl").as<TextBox>().Text()==L"https://custom.example/v1","switch back lost original connection draft");
 provider.SelectedIndex(10);Pump();Check(primaryKey.Password()==L"primary-test-key"&&saves==0,"unsaved provider key lost or switch committed settings");
 root.FindName(L"FallbackProvider").as<ComboBox>().SelectedIndex(8);root.FindName(L"FallbackEnabled").as<ToggleSwitch>().IsOn(true);root.FindName(L"FallbackModel").as<TextBox>().Text(L"gemini-test");root.FindName(L"FallbackKey").as<PasswordBox>().Password(L"fallback-test-key");
 auto fallbackProvider=root.FindName(L"FallbackProvider").as<ComboBox>();fallbackProvider.SelectedIndex(0);Pump();Check(root.FindName(L"FallbackKey").as<PasswordBox>().Password().empty(),"fallback reused primary service credentials");fallbackProvider.SelectedIndex(8);Pump();Check(root.FindName(L"FallbackKey").as<PasswordBox>().Password()==L"fallback-test-key"&&root.FindName(L"FallbackModel").as<TextBox>().Text()==L"gemini-test","fallback draft lost during switching");
 root.FindName(L"ProxyMode").as<ComboBox>().SelectedIndex(2);root.FindName(L"ProxyAddress").as<TextBox>().Text(L"127.0.0.1:7890");
 xaml::Automation::Peers::ButtonAutomationPeer peer(root.FindName(L"Apply").as<Button>());peer.GetPattern(xaml::Automation::Peers::PatternInterface::Invoke).as<xaml::Automation::Provider::IInvokeProvider>().Invoke();Pump();
 Check(saves==1&&saved.provider=="deepl"&&saved.model=="translate"&&savedKey=="primary-test-key","primary service save failed");
 Check(saved.ocrMode==OcrKorean,"Korean selection saved legacy direct-vision ID");
 Check(saved.fallbackEnabled&&saved.fallbackProvider=="gemini"&&saved.fallbackModel=="gemini-test"&&UnprotectSecret(saved.encryptedFallbackKey)=="fallback-test-key","fallback configuration save failed");
 Check(saved.proxyMode==2&&saved.proxyAddress==L"127.0.0.1:7890"&&saved.translateAll,"network and coverage configuration save failed");
 Check(UnprotectSecret(saved.connectionProfiles.at("deepseek").encryptedKey)=="original-key"&&UnprotectSecret(saved.connectionProfiles.at("deepl").encryptedKey)=="primary-test-key","saving another service discarded previous key");
 window.ApiBusy(true);Check(!root.FindName(L"FallbackKey").as<PasswordBox>().IsEnabled()&&!root.FindName(L"ProxyMode").as<ComboBox>().IsEnabled(),"connection configuration editable during request");
 Check(!IsWindowVisible(window.Window()),"test unexpectedly displayed a window");window.ApiBusy(false);
 primaryKey.Password(L"unsaved-change");provider.SelectedIndex(0);Pump();window.Close();
 window.Show(saved,savedKey,{},callbacks,false);Pump();root=window.Content();provider=root.FindName(L"Provider").as<ComboBox>();primaryKey=root.FindName(L"Key").as<PasswordBox>();
 CheckWindowIcons(window.Window());
 CheckTaskbarIdentity(window.Window());
 Check(provider.SelectedIndex()==10&&primaryKey.Password()==L"primary-test-key"&&saves==1,"closing without save changed committed credentials");
 Check(root.FindName(L"OcrMode").as<ComboBox>().SelectedIndex()==3,"saved Korean language not restored");
 provider.SelectedIndex(0);Pump();Check(primaryKey.Password()==L"original-key"&&root.FindName(L"Model").as<ComboBox>().Text()==L"another-deepseek-model","reopening lost saved inactive provider profile");
 primaryKey.Password(L"");provider.SelectedIndex(10);provider.SelectedIndex(0);Pump();Check(primaryKey.Password().empty(),"explicitly cleared key resurrected from stale cache");
 window.Close();}
 Pump();ReportMemory("Settings closed");SIZE_T firstCycle{};
 for(int cycle=0;cycle<3;++cycle){
  {SettingsWindow window;Settings options;if(cycle==0)options.ocrMode=OcrDirectVision;window.Show(options,"test-key",{},{},false);Pump();Check(!IsWindowVisible(window.Window()),"lifecycle test displayed a window");Check(window.Content().FindName(L"OcrMode").as<ComboBox>().SelectedIndex()==0,"legacy vision mode displayed as Korean");window.Close();}
  Pump();auto memory=ReportMemory("Settings reopen/close");if(!cycle)firstCycle=memory;else Check(memory<=firstCycle+24*1024*1024,"reopening settings retains excessive memory");
 }
 std::cout<<"SettingsConfigurationTests: PASS (hidden window, Shell taskbar identity and icon pixels, title bar icons, lazy startup, reopen lifecycle)\n";return 0;
}catch(const winrt::hresult_error& error){std::wcerr<<error.message().c_str()<<'\n';return 1;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
