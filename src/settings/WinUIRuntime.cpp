#include "WinUIRuntime.h"
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <Microsoft.UI.Dispatching.Interop.h>


extern "C" HRESULT __stdcall WindowsAppRuntime_EnsureIsLoaded();
namespace sat {
namespace {
namespace xaml = winrt::Microsoft::UI::Xaml;
struct IslandApplication : xaml::ApplicationT<IslandApplication, xaml::Markup::IXamlMetadataProvider> {
 xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider provider;
 xaml::Markup::IXamlType GetXamlType(winrt::hstring const& name) { return provider.GetXamlType(name); }
 xaml::Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type) { return provider.GetXamlType(type); }
 winrt::com_array<xaml::Markup::XmlnsDefinition> GetXmlnsDefinitions() { return provider.GetXmlnsDefinitions(); }
 void OnLaunched(xaml::LaunchActivatedEventArgs const&) {
  Resources().MergedDictionaries().Append(xaml::Controls::XamlControlsResources());
 }
};
}
struct WinUIRuntime::Impl {
 winrt::Microsoft::UI::Dispatching::DispatcherQueueController queue{nullptr};
 xaml::Application application{nullptr};
 xaml::Hosting::WindowsXamlManager manager{nullptr};
 Impl() {
  winrt::check_hresult(WindowsAppRuntime_EnsureIsLoaded());
  queue=winrt::Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnCurrentThread();
  application=winrt::make<IslandApplication>();
  manager=xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
  if(application.Resources().MergedDictionaries().Size()==0)
   application.Resources().MergedDictionaries().Append(xaml::Controls::XamlControlsResources());
  // WinUI 的线程局部清理回调需要模块保持加载，直至进程退出。
  HMODULE xamlModule{};
  winrt::check_bool(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN,L"Microsoft.UI.Xaml.dll",&xamlModule));
 }
 ~Impl() {
  manager.Close();
  queue.ShutdownQueue();
  manager=nullptr;
  application=nullptr;
  queue=nullptr;
  winrt::clear_factory_cache();
 }
};
WinUIRuntime::WinUIRuntime() {
 // 与 WinUI 应用入口一致，UI apartment 的生命周期覆盖整个进程。
 // XAML、队列和控件在析构时关闭，COM 在线程退出时完成最终清理。
 winrt::init_apartment(winrt::apartment_type::single_threaded);
 impl_=std::make_unique<Impl>();
}
WinUIRuntime::~WinUIRuntime()=default;
bool WinUIRuntime::ProcessMessage(const MSG& message) { return ContentPreTranslateMessage(&message)!=FALSE; }
}
