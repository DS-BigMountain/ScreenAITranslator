#include "Worker.h"
#include "common/Platform.h"
#include "capture/Capture.h"
#include "providers/Provider.h"
#include "settings/Store.h"
#include "settings/SettingsWindow.h"
#include "settings/WinUIRuntime.h"
#include "selection/Selection.h"
#include "overlay/Overlay.h"
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <deque>
#include <atomic>
#include <algorithm>
namespace sat {
namespace {
constexpr UINT UiMessage=WM_APP+1, TrayMessage=WM_APP+2;
constexpr UINT_PTR HideTimer=1, ContextTimer=2;
enum class Stage{Idle,Capture,Selecting,Preprocessing,Requesting,Layout,Displaying};
const wchar_t* StageName(Stage s){switch(s){case Stage::Capture:return L"正在截图";case Stage::Selecting:return L"正在框选";case Stage::Preprocessing:return L"正在处理图片";case Stage::Requesting:return L"正在翻译";case Stage::Layout:return L"正在排版";case Stage::Displaying:return L"翻译已显示";default:return L"就绪";}}
std::wstring Executable(){std::wstring p(32768,0);auto n=GetModuleFileNameW(nullptr,p.data(),static_cast<DWORD>(p.size()));p.resize(n);return p;}
class Hotkeys {
 HWND window_{};std::vector<int> registered_;Hotkey values_[4]{};
 void Release(){for(int id:registered_)UnregisterHotKey(window_,id);registered_.clear();}
public:
 explicit Hotkeys(HWND w):window_(w){}
 ~Hotkeys(){Release();}
 std::wstring Apply(const Settings& s){
  // Re-register as one transaction so changing an existing action can reuse its key.
  Hotkey previous[4];std::copy(std::begin(values_),std::end(values_),previous);Release();
  for(int i=0;i<4;++i){auto k=s.hotkeys[i];if(k.key&&!RegisterHotKey(window_,i+1,k.modifiers|MOD_NOREPEAT,k.key)){
    Release();for(int j=0;j<4;++j)if(previous[j].key&&RegisterHotKey(window_,j+1,previous[j].modifiers|MOD_NOREPEAT,previous[j].key))registered_.push_back(j+1);
    const wchar_t* names[]{L"普通截图翻译",L"翻译上次区域",L"打开设置",L"关闭翻译层"};return std::wstring(names[i])+L"快捷键已被占用，设置未保存。";
   }if(k.key)registered_.push_back(i+1);
  }std::copy(std::begin(s.hotkeys),std::end(s.hotkeys),values_);return {};
 }
};
struct Secret {std::string value;explicit Secret(std::string s):value(std::move(s)){}~Secret(){if(!value.empty())SecureZeroMemory(value.data(),value.size());}};
}
class App {
 HWND window_{};Store store_;Settings settings_;PersistentState state_;std::optional<FixedRegion> sessionRegion_;std::string key_;std::wstring recovery_;
 std::function<std::unique_ptr<ITranslationProvider>(const std::string&)> providerFactory_;
 std::unique_ptr<Hotkeys> hotkeys_;SettingsWindow settingsWindow_;Selection selection_;Overlay overlay_,progress_;TranslationContext context_;Worker worker_;
 std::mutex queueMutex_;std::deque<std::function<void()>> queue_;bool shuttingDown_{};unsigned long long generation_{};
 Stage stage_{Stage::Idle};Monitor currentMonitor_;RECT anchor_{};bool trayAdded_{};UINT taskbarCreated_{};bool background_{};bool apiActive_{};
 static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){
  auto self=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);self->window_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(h,m,w,l);
  try{return self->Message(h,m,w,l);}catch(const AppError& e){self->Error(e);}catch(const std::exception&){self->Error(AppError("app","操作未完成"));}return 0;
 }
 void Post(std::function<void()> f){std::lock_guard lock(queueMutex_);if(shuttingDown_)return;queue_.push_back(std::move(f));PostMessageW(window_,UiMessage,0,0);}
 void Dispatch(){std::deque<std::function<void()>> items;{std::lock_guard lock(queueMutex_);items.swap(queue_);}for(auto& f:items)f();}
 void SetStage(Stage stage){stage_=stage;UpdateTray();}
 void UpdateTray(){
  NOTIFYICONDATAW ni{sizeof(ni)};ni.hWnd=window_;ni.uID=1;
  if(!settings_.tray){if(trayAdded_)Shell_NotifyIconW(NIM_DELETE,&ni);trayAdded_=false;return;}
  ni.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;ni.uCallbackMessage=TrayMessage;ni.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101));if(!ni.hIcon)ni.hIcon=LoadIconW(nullptr,IDI_APPLICATION);std::wstring tip=std::wstring(L"屏幕翻译 · ")+StageName(stage_);wcsncpy_s(ni.szTip,tip.c_str(),_TRUNCATE);
  if(Shell_NotifyIconW(trayAdded_?NIM_MODIFY:NIM_ADD,&ni))trayAdded_=true;
 }
 void ContextChanged(){KillTimer(window_,ContextTimer);context_.Clear();}
 void SaveState(){try{store_.SaveState(state_);}catch(const AppError& e){settingsWindow_.Status(Wide(Sanitize(e.what(),key_)));}}
 void Error(const AppError& e){std::wstring message=Wide(Sanitize(e.what(),key_));settingsWindow_.Status(message);Notice(message);}
 void Notice(const std::wstring& text){
  try{if(currentMonitor_.id.empty())currentMonitor_=MonitorAtCursor();if(!Valid(anchor_)){anchor_=currentMonitor_.rect;anchor_.left+=40;anchor_.top+=40;anchor_.right=anchor_.left+420;anchor_.bottom=anchor_.top+70;}
   overlay_.Notice(text,anchor_,currentMonitor_,[this]{Post([this]{CloseOverlay();});});SetTimer(window_,HideTimer,4000,nullptr);
  }catch(...){/* The settings window retains the current error if rendering fails. */}progress_.Close();SetStage(Stage::Idle);
 }
 void StartProgress(unsigned long long id){
  if(id!=generation_)return;
  if(!progress_.Count())progress_.Progress(anchor_,currentMonitor_,[this,id]{Post([this,id]{if(id==generation_)Cancel();});});
  SetStage(Stage::Preprocessing);
 }
 void CloseOverlay(){KillTimer(window_,HideTimer);overlay_.Close();progress_.Close();SetStage(Stage::Idle);}
 void Cancel(){++generation_;worker_.Cancel();if(apiActive_){apiActive_=false;settingsWindow_.ApiFinished({},L"API 操作已取消。",false);}selection_.Close();CloseOverlay();}
 void Begin(bool fixed,bool reselect=false){
  Cancel();auto id=generation_;if(reselect){ContextChanged();sessionRegion_.reset();SaveState();}
  if(key_.empty()){ShowSettings();Notice(L"请先在 API 页面填写密钥并保存设置");return;}
  auto monitors=EnumerateMonitors();std::optional<RECT> region;
  if(fixed&&sessionRegion_){region=RestoreRegion(*sessionRegion_,monitors);if(region){auto it=std::find_if(monitors.begin(),monitors.end(),[&](auto& m){return m.id==sessionRegion_->monitorId;});if(it!=monitors.end())currentMonitor_=*it;}else{sessionRegion_.reset();SaveState();}}
  if(!region)currentMonitor_=MonitorAtCursor();auto monitor=currentMonitor_;anchor_=region.value_or(monitor.rect);
  // Hide settings before the compositor is flushed so it cannot enter the frozen screenshot.
  if(settingsWindow_.Window())ShowWindow(settingsWindow_.Window(),SW_HIDE);SetStage(Stage::Capture);
  auto options=settings_;auto history=context_.Get(options.contextEnabled,options.contextSize);auto secret=std::make_shared<Secret>(key_);
  worker_.Submit([this,id,monitor,region,options,history,secret](std::stop_token stop){
   try{ComScope com;CheckStop(stop);DwmFlush();auto image=std::make_shared<Image>(CaptureMonitor(monitor,stop));CheckStop(stop);
    if(region){Translate(id,monitor,image,*region,options,history,secret->value,stop);return;}
    Post([this,id,monitor,image]{if(id!=generation_)return;SetStage(Stage::Selecting);selection_.Show(monitor,image,[this,id,monitor,image](std::optional<RECT> chosen){
     if(id!=generation_)return;
     if(chosen){anchor_=*chosen;try{StartProgress(id);}catch(const AppError& e){Error(e);return;}catch(...){Error(AppError("overlay","无法显示翻译进度"));return;}}
     Post([this,id,monitor,image,chosen]{
      if(id!=generation_)return;if(!chosen){SetStage(Stage::Idle);return;}anchor_=*chosen;sessionRegion_=NormalizeRegion(monitor,*chosen);
      auto options=settings_;auto history=context_.Get(options.contextEnabled,options.contextSize);auto secret=std::make_shared<Secret>(key_);
      worker_.Submit([this,id,monitor,image,region=*chosen,options,history,secret](std::stop_token nextStop){try{ComScope com;Translate(id,monitor,image,region,options,history,secret->value,nextStop);}catch(const Cancelled&){}catch(const AppError& e){Post([this,id,e]{if(id==generation_)Error(e);});}catch(...){Post([this,id]{if(id==generation_)Error(AppError("translation","翻译处理失败"));});}});
     });});});
   }catch(const Cancelled&){}catch(const AppError& e){Post([this,id,e]{if(id==generation_)Error(e);});}catch(...){Post([this,id]{if(id==generation_)Error(AppError("capture","屏幕截图失败"));});}
  });
 }
 void Translate(unsigned long long id,const Monitor& monitor,std::shared_ptr<Image> screenshot,RECT region,const Settings& options,const std::vector<ContextItem>& history,const std::string& key,std::stop_token stop){
  auto start=std::chrono::steady_clock::now();Post([this,id]{StartProgress(id);});RECT local=region;OffsetRect(&local,-monitor.rect.left,-monitor.rect.top);auto cropped=Crop(*screenshot,local);screenshot.reset();CheckStop(stop);
  auto jpeg=EncodeJpeg(cropped,options.quality,stop);auto data=Base64(jpeg);jpeg.clear();jpeg.shrink_to_fit();CheckStop(stop);
  auto detected=options.spatialOverlay?DetectText(cropped,stop,options.ocrMode):TextDetection{};
  if(options.spatialOverlay&&detected.regions.empty())detected=DetectTextGeometry(cropped,stop);
  Post([this,id]{if(id==generation_)SetStage(Stage::Requesting);});auto provider=providerFactory_(options.provider);auto apiStart=std::chrono::steady_clock::now();auto result=provider->TranslateLocated(data,history,options,key,stop,detected.regions);auto apiMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-apiStart).count();data.clear();data.shrink_to_fit();CheckStop(stop);
  if(result.segments.empty()){Post([this,id]{if(id==generation_)Notice(L"未识别到文字");});return;}
  Post([this,id]{if(id==generation_)SetStage(Stage::Layout);});auto blocks=BuildOverlay(result,cropped,region,monitor,options);CheckStop(stop);
  auto total=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  Post([this,id,blocks=std::move(blocks),result=std::move(result),options,monitor,apiMs,total]{
   if(id!=generation_)return;
   overlay_.Show(blocks,options,monitor.dpi,
    [this,id]{Post([this,id]{if(id==generation_)CloseOverlay();});},
    [this,id]{Post([this,id]{if(id==generation_&&stage_==Stage::Displaying)Begin(false);});},
    [this,id,monitor](RECT region){Post([this,id,monitor,region]{
     if(id!=generation_||stage_!=Stage::Displaying)return;
     // The fixed path closes the old overlay before capturing fresh screen pixels.
     sessionRegion_=NormalizeRegion(monitor,region);Begin(true);
    });});
   progress_.Close();SetStage(Stage::Displaying);
   if(options.contextEnabled){context_.Add(result,options.contextSize);SetTimer(window_,ContextTimer,30*60*1000,nullptr);}
   if(options.autoHide)SetTimer(window_,HideTimer,options.autoHideSeconds*1000,nullptr);
   settingsWindow_.Status(L"翻译完成 · API "+std::to_wstring(apiMs)+L" ms · 本次处理 "+std::to_wstring(total)+L" ms");
  });
 }
 std::wstring Apply(const Settings& next,const std::string& key){
  ValidateSettings(next);if(!next.tray&&!next.hotkeys[2].key)return L"关闭托盘前，请设置“打开设置”快捷键。";
  Settings updated=next;updated.encryptedKey=ProtectSecret(key);auto hotkeyError=hotkeys_->Apply(updated);if(!hotkeyError.empty())return hotkeyError;
  try{SetStartup(updated.startup,Executable());store_.SaveSettings(updated);}catch(...){hotkeys_->Apply(settings_);try{SetStartup(settings_.startup,Executable());}catch(...){}throw;}
  Cancel();ContextChanged();settings_=std::move(updated);if(!key_.empty())SecureZeroMemory(key_.data(),key_.size());key_=key;recovery_.clear();SaveState();UpdateTray();return {};
 }
 void ApiOperation(bool fetch,const Settings& input,const std::string& secretText){
  Settings options=input;options.encryptedKey.clear();ValidateSettings(options);
  Cancel();apiActive_=true;settingsWindow_.ApiBusy(true);auto id=generation_;auto secret=std::make_shared<Secret>(secretText);
  worker_.Submit([this,id,fetch,options,secret](std::stop_token stop){
   try{ComScope com;auto started=std::chrono::steady_clock::now();std::vector<std::string> models;std::wstring message;
    if(fetch){models=FetchModels(options,secret->value,stop);message=L"已获取 "+std::to_wstring(models.size())+L" 个模型。请选择模型，再点击测试 API；模型列表不代表图片识别能力。";}
    else{auto result=TestVisionApi(options,secret->value,stop);auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();message=L"API 测试通过：测试图识别、翻译和返回格式正常（"+std::to_wstring(ms)+L" ms）。";for(const auto& segment:result.segments){message+=L"\r\n"+Wide(segment.original)+L" → "+Wide(segment.translated);if(message.size()>1200)break;}}
    CheckStop(stop);Post([this,id,fetch,models=std::move(models),message]{if(id!=generation_)return;apiActive_=false;settingsWindow_.ApiFinished(models,message,fetch);});
   }catch(const Cancelled&){}catch(const AppError& e){auto message=Wide(Sanitize(e.what(),secret->value));Post([this,id,fetch,message]{if(id!=generation_)return;apiActive_=false;settingsWindow_.ApiFinished({},L"操作失败："+message,fetch);});}
    catch(...){Post([this,id,fetch]{if(id!=generation_)return;apiActive_=false;settingsWindow_.ApiFinished({},L"操作失败，请检查 API 地址、密钥和所选模型。",fetch);});}
  });
 }
 void ShowSettings(){
  SettingsCallbacks cb;cb.apply=[this](const Settings& s,const std::string& k){return Apply(s,k);};cb.closed=[this](RECT r,int tab){if(apiActive_)Cancel();state_.window=r;state_.tab=tab;SaveState();};
  cb.api=[this](bool fetch,const Settings& s,const std::string& secret){ApiOperation(fetch,s,secret);};
  cb.action=[this](SettingsAction action){switch(action){case SettingsAction::ClearContext:ContextChanged();break;case SettingsAction::ClearRegion:Cancel();sessionRegion_.reset();ContextChanged();SaveState();break;case SettingsAction::ReselectRegion:Begin(true,true);break;case SettingsAction::Translate:Begin(false);break;case SettingsAction::FixedTranslate:Begin(true);break;}};
  settingsWindow_.Show(settings_,key_,state_,std::move(cb));if(!recovery_.empty())settingsWindow_.Status(recovery_);
 }
 void TrayMenu(){
  HMENU menu=CreatePopupMenu();const wchar_t* labels[]{L"打开设置",L"普通截图翻译",L"翻译上次区域",L"重新选择区域",L"关闭当前翻译",L"退出"};for(int i=0;i<6;++i)AppendMenuW(menu,MF_STRING,100+i,labels[i]);POINT pt{};GetCursorPos(&pt);SetForegroundWindow(window_);int action=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,pt.x,pt.y,0,window_,nullptr);DestroyMenu(menu);PostMessageW(window_,WM_NULL,0,0);Action(action);
 }
 void Action(int action){switch(action){case 100:ShowSettings();break;case 101:Begin(false);break;case 102:Begin(true);break;case 103:Begin(true,true);break;case 104:Cancel();break;case 105:DestroyWindow(window_);break;}}
 LRESULT Message(HWND h,UINT m,WPARAM w,LPARAM l){
  if(m==taskbarCreated_){trayAdded_=false;UpdateTray();return 0;}
  switch(m){case UiMessage:Dispatch();return 0;case WM_HOTKEY:switch(w){case 1:Begin(false);break;case 2:Begin(true);break;case 3:ShowSettings();break;case 4:Cancel();break;}return 0;
  case TrayMessage:if(l==WM_LBUTTONDBLCLK)ShowSettings();else if(l==WM_RBUTTONUP||l==WM_CONTEXTMENU)TrayMenu();return 0;
  case WM_COPYDATA:ShowSettings();return TRUE;
  case WM_TIMER:if(w==HideTimer){if(!overlay_.Dragging())CloseOverlay();}else if(w==ContextTimer)ContextChanged();return 0;
  case WM_DISPLAYCHANGE:Cancel();currentMonitor_={};if(sessionRegion_&&!RestoreRegion(*sessionRegion_,EnumerateMonitors())){sessionRegion_.reset();SaveState();}return 0;
  case WM_QUERYENDSESSION:return TRUE;
  case WM_ENDSESSION:if(w)DestroyWindow(h);return 0;
  case WM_CLOSE:DestroyWindow(h);return 0;
  case WM_DESTROY:{Cancel();{std::lock_guard lock(queueMutex_);shuttingDown_=true;}worker_.Stop();settingsWindow_.Close();SaveState();if(trayAdded_){NOTIFYICONDATAW ni{sizeof(ni)};ni.hWnd=h;ni.uID=1;Shell_NotifyIconW(NIM_DELETE,&ni);}hotkeys_.reset();PostQuitMessage(0);return 0;}
  }return DefWindowProcW(h,m,w,l);
 }
public:
 explicit App(bool background,std::filesystem::path dataRoot={},std::function<std::unique_ptr<ITranslationProvider>(const std::string&)> factory=CreateProvider):store_(std::move(dataRoot)),providerFactory_(std::move(factory)),background_(background){}
 ~App(){worker_.Stop();if(!key_.empty())SecureZeroMemory(key_.data(),key_.size());}
 int Run(){
  try{settings_=store_.LoadSettings();}catch(const AppError&){recovery_=L"配置读取失败；请检查 %LOCALAPPDATA%\\ScreenAITranslator\\config.json，原文件已保留。";}
  try{state_=store_.LoadState();}catch(const AppError& e){recovery_=Wide(Sanitize(e.what(),key_));}
  try{key_=UnprotectSecret(settings_.encryptedKey);}catch(const AppError&){recovery_=L"无法解密 API Key，请重新填写并保存。";}
  taskbarCreated_=RegisterWindowMessageW(L"TaskbarCreated");WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SAT.Controller";wc.lpfnWndProc=Proc;RegisterClassW(&wc);
  window_=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"ScreenAITranslator.Controller",WS_POPUP,0,0,0,0,nullptr,nullptr,wc.hInstance,this);if(!window_)return 1;
  hotkeys_=std::make_unique<Hotkeys>(window_);auto error=hotkeys_->Apply(settings_);if(!error.empty()){recovery_=error;settings_.tray=true;}
  if(!std::filesystem::exists(store_.Root()/L"config.json")){DWORD bytes{};settings_.startup=RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",L"ScreenAITranslator",RRF_RT_REG_SZ,nullptr,nullptr,&bytes)==ERROR_SUCCESS;}
  if(!settings_.tray&&!settings_.hotkeys[2].key)settings_.tray=true;UpdateTray();
  // Installer supplies startup registration; explicit Save updates it for portable runs.
  if(!background_)ShowSettings();MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){if(WinUIRuntime::ProcessMessage(msg))continue;TranslateMessage(&msg);DispatchMessageW(&msg);}return static_cast<int>(msg.wParam);
 }
};
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR command,int){
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES|ICC_HOTKEY_CLASS};InitCommonControlsEx(&controls);
 const bool background=wcsstr(command,L"--background")!=nullptr;
 HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\ScreenAITranslator.SingleInstance");if(!mutex)return 1;if(GetLastError()==ERROR_ALREADY_EXISTS){if(!background){if(auto existing=FindWindowW(L"SAT.Controller",L"ScreenAITranslator.Controller")){COPYDATASTRUCT cd{};DWORD_PTR result{};SendMessageTimeoutW(existing,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&cd),SMTO_ABORTIFHUNG,1000,&result);}}CloseHandle(mutex);return 0;}
 int exitCode=0;try{sat::WinUIRuntime ui;sat::App app(background);exitCode=app.Run();}catch(...){exitCode=1;}CloseHandle(mutex);return exitCode;
}
