#include "settings/SettingsWindow.h"
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
using namespace sat;
namespace xaml=winrt::Microsoft::UI::Xaml;
using namespace xaml::Controls;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Pump(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(WinUIRuntime::ProcessMessage(message))continue;TranslateMessage(&message);DispatchMessageW(&message);}}
SIZE_T ReportMemory(const char* stage){PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);Check(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)),"memory query failed");std::cout<<stage<<": private="<<memory.PrivateUsage/1048576.0<<" MiB, working_set="<<memory.WorkingSetSize/1048576.0<<" MiB\n";return memory.PrivateUsage;}
int main(){try{
 WinUIRuntime runtime(true);Check(!WinUIRuntime::Initialized(),"background startup eagerly initialized XAML");MSG empty{};Check(!WinUIRuntime::ProcessMessage(empty),"inactive XAML intercepted messages");ReportMemory("Deferred UI startup");{
 SettingsWindow window;Settings saved;std::string savedKey;int saves=0;
 SettingsCallbacks callbacks;callbacks.apply=[&](const Settings& value,const std::string& key){saved=value;savedKey=key;++saves;return std::wstring{};};
 window.Show(Settings{},"original-key",{},callbacks,false);Pump();Check(WinUIRuntime::Initialized(),"settings did not initialize XAML on demand");Check(!IsWindowVisible(window.Window()),"configuration test must remain hidden");
 auto root=window.Content();auto provider=root.FindName(L"Provider").as<ComboBox>();Check(provider.Items().Size()==ProviderPresets().size(),"provider menu incomplete");
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
 std::cout<<"SettingsConfigurationTests: PASS (hidden window, lazy startup, reopen lifecycle)\n";return 0;
}catch(const winrt::hresult_error& error){std::wcerr<<error.message().c_str()<<'\n';return 1;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
