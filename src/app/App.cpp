#include "Worker.h"
#include "IdleRestart.h"
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
constexpr UINT_PTR HideTimer=1, ContextTimer=2, IdleTimer=3;
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
 friend class AppIdleTest;
 HWND window_{};Store store_;Settings settings_;PersistentState state_;std::optional<FixedRegion> sessionRegion_;std::string key_;std::wstring recovery_;
 std::function<std::unique_ptr<ITranslationProvider>(const std::string&)> providerFactory_;
 std::unique_ptr<Hotkeys> hotkeys_;SettingsWindow settingsWindow_;Selection selection_;Overlay overlay_,progress_;TranslationContext context_;Worker worker_;
 std::mutex queueMutex_;std::deque<std::function<void()>> queue_;bool shuttingDown_{};unsigned long long generation_{};
 Stage stage_{Stage::Idle};Monitor currentMonitor_;RECT anchor_{};bool trayAdded_{};UINT taskbarCreated_{};bool background_{};bool apiActive_{};
 bool usedResources_{};
 std::function<bool(const std::string&)> idleRestart_;
 std::string resume_;
 void ArmIdle(){if(idleRestart_&&usedResources_&&!shuttingDown_)SetTimer(window_,IdleTimer,5000,nullptr);}
 std::string IdleSnapshot(){
  nlohmann::json value={{"version",1},{"context",nlohmann::json::array()},{"expires",context_.ExpiresAt()},{"recovery",Utf8(recovery_)}};
  for(const auto& item:context_.Get(settings_.contextEnabled,settings_.contextSize))value["context"].push_back({item.original,item.translated});
  if(sessionRegion_){const auto& r=*sessionRegion_;value["region"]={Utf8(r.monitorId),r.x,r.y,r.width,r.height,r.monitorWidth,r.monitorHeight};}
  return value.dump();
 }
 void RestoreIdle(){
  if(resume_.empty())return;
  auto value=nlohmann::json::parse(resume_);resume_.clear();resume_.shrink_to_fit();
  if(value.at("version")!=1)throw AppError("idle","Unsupported resume state");
  recovery_=Wide(value.at("recovery").get<std::string>());
  if(value.contains("region")){const auto& r=value.at("region");sessionRegion_=FixedRegion{Wide(r.at(0).get<std::string>()),r.at(1).get<double>(),r.at(2).get<double>(),r.at(3).get<double>(),r.at(4).get<double>(),r.at(5).get<int>(),r.at(6).get<int>()};}
  std::vector<ContextItem> items;for(const auto& item:value.at("context"))items.push_back({item.at(0).get<std::string>(),item.at(1).get<std::string>()});
  if(settings_.contextEnabled)context_.Restore(std::move(items),value.at("expires").get<ULONGLONG>());
 }
 void TryIdle(){
  KillTimer(window_,IdleTimer);
  // Hidden settings still hold unsaved drafts. Only explicitly closed windows
  // may be discarded; active requests, overlays and queued UI work also block handoff.
  if(!idleRestart_||!usedResources_||shuttingDown_||stage_!=Stage::Idle||apiActive_||settingsWindow_.Window()||selection_.Window()||overlay_.Count()||progress_.Count())return;
  if(!worker_.Idle()){SetTimer(window_,IdleTimer,1000,nullptr);return;}
  {std::lock_guard lock(queueMutex_);if(!queue_.empty()){SetTimer(window_,IdleTimer,1000,nullptr);return;}}
  try{if(idleRestart_(IdleSnapshot()))DestroyWindow(window_);}catch(...){/* Keep the existing controller and hotkeys if handoff fails. */}
 }
 static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){
  auto self=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);self->window_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(h,m,w,l);
  try{return self->Message(h,m,w,l);}catch(const AppError& e){self->Error(e);}catch(const std::exception&){self->Error(AppError("app","操作未完成"));}return 0;
 }
 void Post(std::function<void()> f){std::lock_guard lock(queueMutex_);if(shuttingDown_)return;queue_.push_back(std::move(f));PostMessageW(window_,UiMessage,0,0);}
 void Dispatch(){std::deque<std::function<void()>> items;{std::lock_guard lock(queueMutex_);items.swap(queue_);}for(auto& f:items)f();}
 void SetStage(Stage stage){stage_=stage;UpdateTray();if(stage==Stage::Idle)ArmIdle();else KillTimer(window_,IdleTimer);}
 void UpdateTray(){
  NOTIFYICONDATAW ni{sizeof(ni)};ni.hWnd=window_;ni.uID=1;
  if(!settings_.tray){if(trayAdded_)Shell_NotifyIconW(NIM_DELETE,&ni);trayAdded_=false;return;}
  ni.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;ni.uCallbackMessage=TrayMessage;
  ni.hIcon=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(102),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
  if(!ni.hIcon)ni.hIcon=LoadIconW(nullptr,IDI_APPLICATION);std::wstring tip=std::wstring(L"屏幕翻译 · ")+StageName(stage_);wcsncpy_s(ni.szTip,tip.c_str(),_TRUNCATE);
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
  usedResources_=true;
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
    if(region){Translate(id,monitor,std::move(image),*region,options,history,secret->value,stop);return;}
    Post([this,id,monitor,image]{if(id!=generation_)return;SetStage(Stage::Selecting);selection_.Show(monitor,image,[this,id,monitor,image](std::optional<RECT> chosen){
     if(id!=generation_)return;
     if(chosen){anchor_=*chosen;try{StartProgress(id);}catch(const AppError& e){Error(e);return;}catch(...){Error(AppError("overlay","无法显示翻译进度"));return;}}
     Post([this,id,monitor,image,chosen]{
      if(id!=generation_)return;if(!chosen){SetStage(Stage::Idle);return;}anchor_=*chosen;sessionRegion_=NormalizeRegion(monitor,*chosen);
      auto options=settings_;auto history=context_.Get(options.contextEnabled,options.contextSize);auto secret=std::make_shared<Secret>(key_);
      worker_.Submit([this,id,monitor,image,region=*chosen,options,history,secret](std::stop_token nextStop)mutable{try{ComScope com;Translate(id,monitor,std::move(image),region,options,history,secret->value,nextStop);}catch(const Cancelled&){}catch(const AppError& e){Post([this,id,e]{if(id==generation_)Error(e);});}catch(...){Post([this,id]{if(id==generation_)Error(AppError("translation","翻译处理失败"));});}});
     });});});
   }catch(const Cancelled&){}catch(const AppError& e){Post([this,id,e]{if(id==generation_)Error(e);});}catch(...){Post([this,id]{if(id==generation_)Error(AppError("capture","屏幕截图失败"));});}
  });
 }
 void Translate(unsigned long long id,const Monitor& monitor,std::shared_ptr<Image> screenshot,RECT region,const Settings& options,const std::vector<ContextItem>& history,const std::string& key,std::stop_token stop){
  auto start=std::chrono::steady_clock::now();Post([this,id]{StartProgress(id);});RECT local=region;OffsetRect(&local,-monitor.rect.left,-monitor.rect.top);auto cropped=Crop(*screenshot,local);screenshot.reset();CheckStop(stop);
  auto ocrStart=std::chrono::steady_clock::now();
  auto detected=DetectText(cropped,stop,options.ocrMode);
  if(detected.regions.empty())detected=DetectTextGeometry(cropped,stop);
  auto ocrMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-ocrStart).count();
  const bool textOnly=CanTranslateText(detected.regions);std::string data;
  auto connection=SelectTranslationConnection(options,detected.regions);
  Secret selectedKey(!textOnly&&options.fallbackEnabled?UnprotectSecret(options.encryptedFallbackKey):key);
  if(!textOnly){auto jpeg=EncodeJpeg(cropped,options.quality,stop);data=Base64(jpeg);}CheckStop(stop);
  Post([this,id]{if(id==generation_)SetStage(Stage::Requesting);});auto provider=providerFactory_(connection.provider);auto apiStart=std::chrono::steady_clock::now();auto result=provider->TranslateLocated(data,history,connection,selectedKey.value,stop,detected.regions);auto apiMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-apiStart).count();data.clear();data.shrink_to_fit();CheckStop(stop);
  if(result.segments.empty()){Post([this,id]{if(id==generation_)Notice(L"未识别到文字");});return;}
  Post([this,id]{if(id==generation_)SetStage(Stage::Layout);});auto blocks=BuildOverlay(result,std::move(cropped),region,monitor,options);CheckStop(stop);
  auto total=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  Post([this,id,blocks=std::move(blocks),result=std::move(result),options,monitor,apiMs,total,ocrMs,textOnly]{
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
   settingsWindow_.Status(std::wstring(L"翻译完成 · ")+(textOnly?L"纯文本":L"视觉翻译")+L" · OCR "+std::to_wstring(ocrMs)+L" ms · API "+std::to_wstring(apiMs)+L" ms · 请求 "+std::to_wstring(result.requestCount)+L" 次 · 本次处理 "+std::to_wstring(total)+L" ms");
  });
 }
 std::wstring Apply(const Settings& next,const std::string& key){
  ValidateSettings(next);if(!next.tray&&!next.hotkeys[2].key)return L"关闭托盘前，请设置“打开设置”快捷键。";
  Settings updated=next;updated.encryptedKey=ProtectSecret(key);auto hotkeyError=hotkeys_->Apply(updated);if(!hotkeyError.empty())return hotkeyError;
  try{SetStartup(updated.startup,Executable());store_.SaveSettings(updated);}catch(...){hotkeys_->Apply(settings_);try{SetStartup(settings_.startup,Executable());}catch(...){}throw;}
  Cancel();ContextChanged();settings_=std::move(updated);if(!key_.empty())SecureZeroMemory(key_.data(),key_.size());key_=key;recovery_.clear();SaveState();UpdateTray();return {};
 }
 void ApiOperation(bool fetch,const Settings& input,const std::string& secretText){
  usedResources_=true;
  Settings options=input;options.encryptedKey.clear();ValidateSettings(options);
  Cancel();apiActive_=true;settingsWindow_.ApiBusy(true);auto id=generation_;auto secret=std::make_shared<Secret>(secretText);
  worker_.Submit([this,id,fetch,options,secret](std::stop_token stop){
   try{ComScope com;auto started=std::chrono::steady_clock::now();std::vector<std::string> models;std::wstring message;
    if(fetch){models=FetchModels(options,secret->value,stop);message=L"已获取 "+std::to_wstring(models.size())+L" 个模型。请选择模型，再点击测试 API；模型列表不代表图片识别能力。";}
    else{auto result=TestTextApi(options,secret->value,stop);message=L"主连接文字翻译自检通过。";
     if(options.fallbackEnabled){auto fallback=SelectTranslationConnection(options,{});Secret fallbackKey(UnprotectSecret(options.encryptedFallbackKey));TestVisionApi(fallback,fallbackKey.value,stop);message+=L"视觉兜底自检通过。";}
     auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();message+=L"耗时 "+std::to_wstring(ms)+L" ms。主连接图片能力需选择支持视觉的模型。";
     for(const auto& segment:result.segments){message+=L"\r\n"+Wide(segment.original)+L" → "+Wide(segment.translated);if(message.size()>1200)break;}}
    CheckStop(stop);Post([this,id,fetch,models=std::move(models),message]{if(id!=generation_)return;apiActive_=false;settingsWindow_.ApiFinished(models,message,fetch);});
   }catch(const Cancelled&){}catch(const AppError& e){auto message=Wide(Sanitize(e.what(),secret->value));Post([this,id,fetch,message]{if(id!=generation_)return;apiActive_=false;settingsWindow_.ApiFinished({},L"操作失败："+message,fetch);});}
    catch(...){Post([this,id,fetch]{if(id!=generation_)return;apiActive_=false;settingsWindow_.ApiFinished({},L"操作失败，请检查 API 地址、密钥和所选模型。",fetch);});}
  });
 }
 void ShowSettings(bool visible=true){
  usedResources_=true;KillTimer(window_,IdleTimer);
  SettingsCallbacks cb;cb.apply=[this](const Settings& s,const std::string& k){return Apply(s,k);};cb.closed=[this](RECT r,int tab){if(apiActive_)Cancel();state_.window=r;state_.tab=tab;SaveState();ArmIdle();};
  cb.api=[this](bool fetch,const Settings& s,const std::string& secret){ApiOperation(fetch,s,secret);};
  cb.action=[this](SettingsAction action){switch(action){case SettingsAction::ClearContext:ContextChanged();break;case SettingsAction::Translate:Begin(false);break;case SettingsAction::FixedTranslate:Begin(true);break;}};
  settingsWindow_.Show(settings_,key_,state_,std::move(cb),visible);if(!recovery_.empty())settingsWindow_.Status(recovery_);
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
  case WM_TIMER:if(w==HideTimer){if(!overlay_.Dragging())CloseOverlay();}else if(w==ContextTimer)ContextChanged();else if(w==IdleTimer)TryIdle();return 0;
  case WM_DISPLAYCHANGE:Cancel();currentMonitor_={};if(sessionRegion_&&!RestoreRegion(*sessionRegion_,EnumerateMonitors())){sessionRegion_.reset();SaveState();}return 0;
  case WM_QUERYENDSESSION:return TRUE;
  case WM_ENDSESSION:if(w)DestroyWindow(h);return 0;
  case WM_CLOSE:DestroyWindow(h);return 0;
  case WM_DESTROY:{Cancel();{std::lock_guard lock(queueMutex_);shuttingDown_=true;}worker_.Stop();settingsWindow_.Close();SaveState();if(trayAdded_){NOTIFYICONDATAW ni{sizeof(ni)};ni.hWnd=h;ni.uID=1;Shell_NotifyIconW(NIM_DELETE,&ni);}hotkeys_.reset();PostQuitMessage(0);return 0;}
  }return DefWindowProcW(h,m,w,l);
 }
public:
 explicit App(bool background,std::filesystem::path dataRoot={},std::function<std::unique_ptr<ITranslationProvider>(const std::string&)> factory=CreateProvider,std::function<bool(const std::string&)> idleRestart={},std::string resume={}):store_(std::move(dataRoot)),providerFactory_(std::move(factory)),background_(background),idleRestart_(std::move(idleRestart)),resume_(std::move(resume)){}
 ~App(){worker_.Stop();if(!key_.empty())SecureZeroMemory(key_.data(),key_.size());}
 int Run(){
  try{settings_=store_.LoadSettings();}catch(const AppError&){recovery_=L"配置读取失败；请检查 %LOCALAPPDATA%\\ScreenAITranslator\\config.json，原文件已保留。";}
  try{state_=store_.LoadState();}catch(const AppError& e){recovery_=Wide(Sanitize(e.what(),key_));}
  try{key_=UnprotectSecret(settings_.encryptedKey);}catch(const AppError&){recovery_=L"无法解密 API Key，请重新填写并保存。";}
  RestoreIdle();
  taskbarCreated_=RegisterWindowMessageW(L"TaskbarCreated");WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SAT.Controller";wc.lpfnWndProc=Proc;RegisterClassW(&wc);
  window_=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"ScreenAITranslator.Controller",WS_POPUP,0,0,0,0,nullptr,nullptr,wc.hInstance,this);if(!window_)return 1;
  hotkeys_=std::make_unique<Hotkeys>(window_);auto error=hotkeys_->Apply(settings_);if(!error.empty()){recovery_=error;settings_.tray=true;}
  if(!std::filesystem::exists(store_.Root()/L"config.json")){DWORD bytes{};settings_.startup=RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",L"ScreenAITranslator",RRF_RT_REG_SZ,nullptr,nullptr,&bytes)==ERROR_SUCCESS;}
  if(!settings_.tray&&!settings_.hotkeys[2].key)settings_.tray=true;UpdateTray();
  const auto now=GetTickCount64();if(context_.ExpiresAt()>now)SetTimer(window_,ContextTimer,static_cast<UINT>(context_.ExpiresAt()-now),nullptr);
  // Installer supplies startup registration; explicit Save updates it for portable runs.
  if(!background_)ShowSettings();MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){if(WinUIRuntime::ProcessMessage(msg))continue;TranslateMessage(&msg);DispatchMessageW(&msg);}return static_cast<int>(msg.wParam);
 }
};
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR command,int){
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES|ICC_HOTKEY_CLASS};InitCommonControlsEx(&controls);
 const bool background=wcsstr(command,L"--background")!=nullptr;
 HANDLE mutex{};int exitCode=0;
 try{
  auto resume=sat::ReceiveIdleReplacement(command);mutex=resume.instance;
  if(!mutex){mutex=CreateMutexW(nullptr,FALSE,L"Local\\ScreenAITranslator.SingleInstance");if(!mutex)return 1;if(GetLastError()==ERROR_ALREADY_EXISTS){if(!background){if(auto existing=FindWindowW(L"SAT.Controller",L"ScreenAITranslator.Controller")){COPYDATASTRUCT cd{};DWORD_PTR result{};SendMessageTimeoutW(existing,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&cd),SMTO_ABORTIFHUNG,1000,&result);}}CloseHandle(mutex);return 0;}}
  sat::WinUIRuntime ui(true);sat::App app(background,{},sat::CreateProvider,[mutex](const std::string& payload){return sat::StartIdleReplacement(sat::Executable(),mutex,payload);},std::move(resume.payload));exitCode=app.Run();
 }catch(...){exitCode=1;}
 if(mutex)CloseHandle(mutex);return exitCode;
}
