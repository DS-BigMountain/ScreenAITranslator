#include "SettingsWindow.h"
#include "Store.h"
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <algorithm>
#include <cmath>

namespace sat {
namespace xaml=winrt::Microsoft::UI::Xaml;
using namespace xaml::Controls;
namespace {
std::wstring Color(COLORREF c) { wchar_t b[8];swprintf_s(b,L"%02X%02X%02X",GetRValue(c),GetGValue(c),GetBValue(c));return b; }
COLORREF ParseColor(std::wstring t) {
 if(!t.empty()&&t[0]==L'#')t.erase(0,1);
 if(t.size()!=6||t.find_first_not_of(L"0123456789abcdefABCDEF")!=t.npos)throw AppError("settings","颜色须为六位十六进制数值。");
 auto v=std::stoul(t,nullptr,16);return RGB((v>>16)&255,(v>>8)&255,v&255);
}
std::wstring HotkeyLabel(Hotkey key) {
 if(!key.key)return {};
 std::wstring label;
 if(key.modifiers&MOD_CONTROL)label+=L"Ctrl + ";
 if(key.modifiers&MOD_ALT)label+=L"Alt + ";
 if(key.modifiers&MOD_SHIFT)label+=L"Shift + ";
 if(key.modifiers&MOD_WIN)label+=L"Win + ";
 wchar_t name[64]{};auto scan=MapVirtualKeyW(key.key,MAPVK_VK_TO_VSC);
 if(key.key>=VK_PRIOR&&key.key<=VK_DELETE)scan|=0x100;
 GetKeyNameTextW(static_cast<LONG>(scan<<16),name,64);
 return label+(name[0]?std::wstring(name):std::to_wstring(key.key));
}
struct Secret {
 std::string value;
 ~Secret(){if(!value.empty())SecureZeroMemory(value.data(),value.size());}
};
}
struct SettingsWindow::Impl {
 xaml::Hosting::DesktopWindowXamlSource island{nullptr};
 xaml::FrameworkElement root{nullptr};
 Hotkey hotkeys[4]{};
 bool updating{};
 bool focusPending{};
 template<class T> T Get(const wchar_t* name) const {return root.FindName(name).as<T>();}
 std::wstring Text(const wchar_t* name) const {return std::wstring(Get<TextBox>(name).Text());}
 bool On(const wchar_t* name) const {return Get<ToggleSwitch>(name).IsOn();}
 double Number(const wchar_t* name) const {
  auto control=Get<NumberBox>(name);auto value=control.Value();
  if(!std::isfinite(value))throw AppError("settings","请填写有效数值。");return value;
 }
 int Integer(const wchar_t* name) const {
  auto value=Number(name);if(std::floor(value)!=value)throw AppError("settings","秒数与上下文数量须为整数。");return static_cast<int>(value);
 }
};
SettingsWindow::SettingsWindow()=default;
SettingsWindow::~SettingsWindow(){Close();}
xaml::FrameworkElement SettingsWindow::Content() const {return ui_?ui_->root:nullptr;}
void SettingsWindow::Show(const Settings& s,const std::string& key,const PersistentState& state,SettingsCallbacks cb) {
 if(hwnd_){ShowWindow(hwnd_,SW_RESTORE);SetForegroundWindow(hwnd_);return;}
 settings_=s;callbacks_=std::move(cb);page_=std::clamp(state.tab,0,4);apiBusy_=false;
 WNDCLASSEXW wc{sizeof(wc)};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SAT.Settings";wc.lpfnWndProc=Proc;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(wc.hInstance,MAKEINTRESOURCEW(101));wc.hIconSm=wc.hIcon;RegisterClassExW(&wc);
 POINT pt{};GetCursorPos(&pt);auto mon=MonitorFromPoint(pt,MONITOR_DEFAULTTOPRIMARY);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(mon,&mi);
 auto dpi=GetDpiForSystem();int width=std::min(MulDiv(1040,dpi,96),Width(mi.rcWork)),height=std::min(MulDiv(820,dpi,96),Height(mi.rcWork));
 int x=mi.rcWork.left+(Width(mi.rcWork)-width)/2,y=mi.rcWork.top+(Height(mi.rcWork)-height)/2;
 if(state.window){auto r=*state.window;auto savedMon=MonitorFromRect(&r,MONITOR_DEFAULTTONULL);if(savedMon){mi.cbSize=sizeof(mi);GetMonitorInfoW(savedMon,&mi);width=std::min(std::max(width,Width(r)),Width(mi.rcWork));height=std::min(std::max(height,Height(r)),Height(mi.rcWork));x=std::clamp(r.left,mi.rcWork.left,mi.rcWork.right-width);y=std::clamp(r.top,mi.rcWork.top,mi.rcWork.bottom-height);}}
 hwnd_=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"屏幕翻译",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,x,y,width,height,nullptr,nullptr,wc.hInstance,this);
 if(!hwnd_)throw AppError("settings","无法创建设置窗口");
 try{Build(key);Layout();ShowWindow(hwnd_,SW_SHOW);SetForegroundWindow(hwnd_);}catch(...){Close();throw;}
}
void SettingsWindow::Close() {
 if(ui_){ui_->updating=true;if(ui_->root)ui_->Get<PasswordBox>(L"Key").Password(L"");if(ui_->island){ui_->island.Content(nullptr);ui_->island.Close();}ui_.reset();}
 if(hwnd_)DestroyWindow(hwnd_);hwnd_=nullptr;
}
void SettingsWindow::Layout() {
 if(!ui_||!ui_->island)return;RECT r{};GetClientRect(hwnd_,&r);
 ui_->island.SiteBridge().MoveAndResize({0,0,Width(r),Height(r)});
 ui_->island.SiteBridge().Show();
}
void SettingsWindow::Build(const std::string& key) {
 ui_=std::make_unique<Impl>();ui_->updating=true;
 auto resource=FindResourceW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(201),RT_RCDATA);
 if(!resource)throw AppError("settings","界面资源缺失");
 auto loaded=LoadResource(GetModuleHandleW(nullptr),resource);auto bytes=static_cast<const char*>(LockResource(loaded));auto size=SizeofResource(GetModuleHandleW(nullptr),resource);
 ui_->root=xaml::Markup::XamlReader::Load(Wide(std::string(bytes,size))).as<xaml::FrameworkElement>();
 ui_->island=xaml::Hosting::DesktopWindowXamlSource();ui_->island.Initialize(winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd_));ui_->island.Content(ui_->root);
 ui_->island.TakeFocusRequested([this](auto const&,auto const& args){
  auto reason=args.Request().Reason();
  if(!ui_->focusPending&&(reason==xaml::Hosting::XamlSourceFocusNavigationReason::First||reason==xaml::Hosting::XamlSourceFocusNavigationReason::Last)){
   ui_->focusPending=true;PostMessageW(hwnd_,WM_APP+43,static_cast<WPARAM>(reason),0);
  }
 });
 auto text=[&](const wchar_t* name,const std::wstring& value){ui_->Get<TextBox>(name).Text(value);};
 auto toggle=[&](const wchar_t* name,bool value){ui_->Get<ToggleSwitch>(name).IsOn(value);};
 auto number=[&](const wchar_t* name,double value){ui_->Get<NumberBox>(name).Value(value);};
 toggle(L"Startup",settings_.startup);toggle(L"Tray",settings_.tray);toggle(L"AutoHide",settings_.autoHide);number(L"HideSeconds",settings_.autoHideSeconds);
 ui_->Get<ComboBox>(L"Provider").SelectedIndex(settings_.provider=="deepseek"?0:1);text(L"BaseUrl",settings_.baseUrl);ui_->Get<PasswordBox>(L"Key").Password(Wide(key));
 ui_->Get<ComboBox>(L"Model").Items().Append(winrt::box_value(Wide(settings_.model)));ui_->Get<ComboBox>(L"Model").SelectedIndex(0);number(L"Timeout",settings_.timeoutSeconds);ui_->Get<ComboBox>(L"ThinkingMode").SelectedIndex(settings_.thinkingHigh?1:0);
 ui_->Get<ComboBox>(L"Quality").SelectedIndex(settings_.quality);ui_->Get<ComboBox>(L"OcrMode").SelectedIndex(settings_.ocrMode);toggle(L"ContextEnabled",settings_.contextEnabled);number(L"ContextSize",settings_.contextSize);text(L"Prompt",Wide(settings_.prompt));
 toggle(L"SpatialOverlay",settings_.spatialOverlay);auto fonts=ui_->Get<ComboBox>(L"Font");int selectedFont=-1;
 for(auto name:{L"Microsoft YaHei UI",L"Microsoft YaHei",L"SimSun",L"SimHei",L"KaiTi",L"FangSong",L"Segoe UI",L"Arial",L"Times New Roman",L"Consolas",L"Yu Gothic UI",L"Meiryo"}){if(settings_.font==name)selectedFont=int(fonts.Items().Size());fonts.Items().Append(winrt::box_value(name));}
 if(selectedFont<0){selectedFont=int(fonts.Items().Size());fonts.Items().Append(winrt::box_value(settings_.font));}fonts.SelectedIndex(selectedFont);fonts.Text(settings_.font);toggle(L"MatchTextColor",settings_.matchTextColor);toggle(L"AutoFont",settings_.autoFont);number(L"FontSize",settings_.fontSize);text(L"TextColor",Color(settings_.textColor));text(L"OutlineColor",Color(settings_.outlineColor));number(L"OutlineWidth",settings_.outlineWidth);number(L"Opacity",settings_.backgroundOpacity);
 for(int i=0;i<4;++i){ui_->hotkeys[i]=settings_.hotkeys[i];auto name=L"Hotkey"+std::to_wstring(i);auto control=ui_->Get<TextBox>(name.c_str());control.Text(HotkeyLabel(ui_->hotkeys[i]));
  control.PreviewKeyDown([this,i](auto const& sender,xaml::Input::KeyRoutedEventArgs const& e){
   auto key=static_cast<UINT>(e.Key());if(key==VK_TAB)return;e.Handled(true);
   if(key==VK_CONTROL||key==VK_MENU||key==VK_SHIFT||key==VK_LWIN||key==VK_RWIN)return;
   Hotkey next{};if(key!=VK_BACK&&key!=VK_DELETE){next.key=key;if(GetKeyState(VK_CONTROL)<0)next.modifiers|=MOD_CONTROL;if(GetKeyState(VK_MENU)<0)next.modifiers|=MOD_ALT;if(GetKeyState(VK_SHIFT)<0)next.modifiers|=MOD_SHIFT;if(GetKeyState(VK_LWIN)<0||GetKeyState(VK_RWIN)<0)next.modifiers|=MOD_WIN;}
   ui_->hotkeys[i]=next;sender.template as<TextBox>().Text(HotkeyLabel(next));
  });
 }
 auto nav=ui_->Get<NavigationView>(L"Navigation");
 auto select=[this](int page){page_=page;for(int i=0;i<5;++i){auto name=L"Page"+std::to_wstring(i);ui_->Get<StackPanel>(name.c_str()).Visibility(i==page?xaml::Visibility::Visible:xaml::Visibility::Collapsed);}const wchar_t* titles[]{L"翻译与偏好",L"快捷键",L"模型连接",L"翻译参数",L"显示效果"};ui_->Get<TextBlock>(L"PageTitle").Text(titles[page]);ui_->Get<ScrollViewer>(L"PageScroll").ChangeView(nullptr,winrt::box_value(0.0).as<winrt::Windows::Foundation::IReference<double>>(),nullptr);};
 nav.SelectionChanged([select](auto const&,NavigationViewSelectionChangedEventArgs const& args){if(auto item=args.SelectedItem().try_as<NavigationViewItem>())select(std::stoi(std::wstring(winrt::unbox_value<winrt::hstring>(item.Tag()))));});
 nav.SelectedItem(nav.MenuItems().GetAt(page_));select(page_);
 auto button=[&](const wchar_t* name,std::function<void()> action){auto control=ui_->Get<Button>(name);xaml::Automation::AutomationProperties::SetAutomationId(control,name);control.Click([this,action](auto const&,auto const&){try{action();}catch(const AppError& e){Status(Wide(e.what()));}catch(const winrt::hresult_error&){Status(L"界面操作未完成。");}catch(...){Status(L"操作未完成，请检查设置。");}});};
 button(L"Apply",[this]{Save();});button(L"Close",[this]{PostMessageW(hwnd_,WM_CLOSE,0,0);});
 button(L"FetchModels",[this]{StartApi(true);});button(L"TestApi",[this]{StartApi(false);});
 for(auto [name,action]:std::initializer_list<std::pair<const wchar_t*,SettingsAction>>{{L"Translate",SettingsAction::Translate},{L"TranslateFixed",SettingsAction::FixedTranslate},{L"Reselect",SettingsAction::ReselectRegion},{L"ClearRegion",SettingsAction::ClearRegion},{L"ClearContext",SettingsAction::ClearContext}}){button(name,[this,action]{if(callbacks_.action)callbacks_.action(action);if(action==SettingsAction::ClearRegion)Status(L"上次区域已清除。");if(action==SettingsAction::ClearContext)Status(L"上下文已清空。");});}
 ui_->Get<TextBox>(L"BaseUrl").RegisterPropertyChangedCallback(TextBox::TextProperty(),[this](auto const&,auto const&){InvalidateModels();});
 ui_->Get<PasswordBox>(L"Key").RegisterPropertyChangedCallback(PasswordBox::PasswordProperty(),[this](auto const&,auto const&){InvalidateModels();});
 ui_->Get<ComboBox>(L"Provider").RegisterPropertyChangedCallback(xaml::Controls::Primitives::Selector::SelectedIndexProperty(),[this](auto const&,auto const&){if(ui_->updating)return;InvalidateModels();auto provider=ui_->Get<ComboBox>(L"Provider").SelectedIndex();auto address=ui_->Text(L"BaseUrl");if(provider==1&&address==L"https://api.deepseek.com")ui_->Get<TextBox>(L"BaseUrl").Text(L"https://api.openai.com/v1");else if(provider==0&&address==L"https://api.openai.com/v1")ui_->Get<TextBox>(L"BaseUrl").Text(L"https://api.deepseek.com");});
 auto dependent=[this]{ui_->Get<NumberBox>(L"HideSeconds").IsEnabled(ui_->On(L"AutoHide"));ui_->Get<NumberBox>(L"ContextSize").IsEnabled(ui_->On(L"ContextEnabled"));ui_->Get<NumberBox>(L"FontSize").IsEnabled(!ui_->On(L"AutoFont"));ui_->Get<TextBox>(L"TextColor").IsEnabled(!ui_->On(L"MatchTextColor"));};
 for(auto name:{L"AutoHide",L"ContextEnabled",L"AutoFont",L"MatchTextColor"})ui_->Get<ToggleSwitch>(name).Toggled([dependent](auto const&,auto const&){dependent();});
 dependent();ui_->updating=false;
}
void SettingsWindow::Status(const std::wstring& text){if(ui_&&ui_->root)ui_->Get<TextBlock>(L"Status").Text(text);}
void SettingsWindow::ApiBusy(bool busy) {
 apiBusy_=busy;if(!ui_)return;
 for(auto name:{L"Provider",L"BaseUrl",L"Key",L"Model",L"Timeout",L"ThinkingMode",L"FetchModels",L"TestApi",L"Apply"})ui_->Get<Control>(name).IsEnabled(!busy);
 auto progress=ui_->Get<ProgressRing>(L"ApiProgress");progress.IsActive(busy);progress.Visibility(busy?xaml::Visibility::Visible:xaml::Visibility::Collapsed);
}
void SettingsWindow::InvalidateModels() {
 if(!ui_||ui_->updating||apiBusy_)return;
 auto model=ui_->Get<ComboBox>(L"Model");model.SelectedItem(nullptr);model.SelectedIndex(-1);model.Items().Clear();model.Text(L"");ui_->Get<TextBlock>(L"ApiResult").Text(L"连接配置已改变，请输入模型名称或重新获取列表。");
}
Settings SettingsWindow::ReadApi(bool requireModel) const {
 auto value=settings_;value.provider=ui_->Get<ComboBox>(L"Provider").SelectedIndex()==0?"deepseek":"openai-compatible";value.baseUrl=ui_->Text(L"BaseUrl");value.timeoutSeconds=ui_->Integer(L"Timeout");value.thinkingHigh=ui_->Get<ComboBox>(L"ThinkingMode").SelectedIndex()==1;
 auto model=ui_->Get<ComboBox>(L"Model");auto name=std::wstring(model.Text());
 if(name.empty()&&model.SelectedItem())name=std::wstring(winrt::unbox_value<winrt::hstring>(model.SelectedItem()));
 if(name.empty()&&requireModel)throw AppError("settings","请选择或输入支持图片的模型名称。");
 if(!name.empty())value.model=Utf8(name);return value;
}
void SettingsWindow::StartApi(bool fetch) {
 if(apiBusy_||!callbacks_.api)return;
 auto value=ReadApi(!fetch);Secret secret{Utf8(std::wstring(ui_->Get<PasswordBox>(L"Key").Password()))};if(secret.value.empty())throw AppError("settings","请先填写 API 密钥。");
 ApiBusy(true);ui_->Get<TextBlock>(L"ApiResult").Text(fetch?L"正在获取模型列表…":L"正在测试图片识别与翻译…");
 try{callbacks_.api(fetch,value,secret.value);}catch(...){ApiFinished({},L"请求未完成，请检查连接配置。",false);throw;}
}
void SettingsWindow::ApiFinished(const std::vector<std::string>& models,const std::wstring& message,bool fetched) {
 if(!ui_)return;ApiBusy(false);
 if(fetched){auto model=ui_->Get<ComboBox>(L"Model");auto previous=model.Text();model.Items().Clear();int selected=-1;for(size_t i=0;i<models.size();++i){auto name=Wide(models[i]);model.Items().Append(winrt::box_value(name));if(name==previous)selected=static_cast<int>(i);}if(selected>=0)model.SelectedIndex(selected);else if(models.size()==1)model.SelectedIndex(0);else model.Text(previous);}
 ui_->Get<TextBlock>(L"ApiResult").Text(message);
}
void SettingsWindow::Save() {
 if(apiBusy_)return;
 try{
  auto next=ReadApi(true);next.startup=ui_->On(L"Startup");next.tray=ui_->On(L"Tray");next.autoHide=ui_->On(L"AutoHide");next.autoHideSeconds=ui_->Integer(L"HideSeconds");std::copy(std::begin(ui_->hotkeys),std::end(ui_->hotkeys),std::begin(next.hotkeys));
  next.quality=ui_->Get<ComboBox>(L"Quality").SelectedIndex();next.ocrMode=ui_->Get<ComboBox>(L"OcrMode").SelectedIndex();next.contextEnabled=ui_->On(L"ContextEnabled");next.contextSize=ui_->Integer(L"ContextSize");next.prompt=Utf8(ui_->Text(L"Prompt"));
  next.spatialOverlay=ui_->On(L"SpatialOverlay");next.font=std::wstring(ui_->Get<ComboBox>(L"Font").Text());next.matchTextColor=ui_->On(L"MatchTextColor");next.autoFont=ui_->On(L"AutoFont");next.fontSize=static_cast<float>(ui_->Number(L"FontSize"));next.textColor=ParseColor(ui_->Text(L"TextColor"));next.outlineColor=ParseColor(ui_->Text(L"OutlineColor"));next.outlineWidth=static_cast<float>(ui_->Number(L"OutlineWidth"));next.backgroundOpacity=static_cast<float>(ui_->Number(L"Opacity"));
  ValidateSettings(next);Secret secret{Utf8(std::wstring(ui_->Get<PasswordBox>(L"Key").Password()))};auto error=callbacks_.apply?callbacks_.apply(next,secret.value):L"保存服务不可用。";
  if(!error.empty()){Status(error);return;}settings_=next;Status(L"设置已保存。关闭窗口后程序继续在后台运行。");
 }catch(const AppError& e){Status(Wide(e.what()));}catch(...){Status(L"请检查数值范围和六位十六进制颜色。");}
}
LRESULT CALLBACK SettingsWindow::Proc(HWND h,UINT m,WPARAM w,LPARAM l) {
 auto self=reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(h,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);self->hwnd_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(h,m,w,l);
 try{switch(m){
 case WM_SIZE:self->Layout();return 0;
 case WM_APP+43:if(self->ui_){auto& ui=*self->ui_;if(ui.root.IsLoaded())ui.island.NavigateFocus(xaml::Hosting::XamlSourceFocusNavigationRequest(static_cast<xaml::Hosting::XamlSourceFocusNavigationReason>(w)));ui.focusPending=false;}return 0;
 case WM_GETMINMAXINFO:{auto info=reinterpret_cast<MINMAXINFO*>(l);auto dpi=GetDpiForWindow(h);info->ptMinTrackSize={MulDiv(860,dpi,96),MulDiv(580,dpi,96)};return 0;}
 case WM_SETFOCUS:if(self->ui_&&self->ui_->root.IsLoaded())self->ui_->island.NavigateFocus(xaml::Hosting::XamlSourceFocusNavigationRequest(xaml::Hosting::XamlSourceFocusNavigationReason::Restore));return 0;
 case WM_DPICHANGED:{auto r=reinterpret_cast<RECT*>(l);SetWindowPos(h,nullptr,r->left,r->top,Width(*r),Height(*r),SWP_NOZORDER|SWP_NOACTIVATE);self->Layout();return 0;}
 case WM_CLOSE:{RECT r{};GetWindowRect(h,&r);if(self->callbacks_.closed)self->callbacks_.closed(r,self->page_);self->Close();return 0;}
 case WM_NCDESTROY:self->hwnd_=nullptr;SetWindowLongPtrW(h,GWLP_USERDATA,0);break;
 }}catch(...){self->Status(L"窗口操作未完成。");}return DefWindowProcW(h,m,w,l);
}
}
