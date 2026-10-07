// In-process integration harness exercises the production controller and message loop.
// Only the cloud provider and data directory are injected; desktop capture is real DXGI.
#include "../src/app/App.cpp"
#include <iostream>
#include <fstream>
#include <psapi.h>
using namespace std::chrono_literals;
namespace {
std::atomic<int> translations{},cancelled{};
std::atomic<bool> slow{};
class FakeProvider final:public sat::ITranslationProvider {
public:
 sat::TranslationResult Translate(const std::string& image,const std::vector<sat::ContextItem>& context,const sat::Settings&,const std::string& key,std::stop_token stop)override{
  if(image.empty()||key!="test-only-key")throw std::runtime_error("invalid pipeline input");
  auto call=++translations;if(call>1&&context.empty())throw std::runtime_error("context was not supplied");
  if(slow){for(int i=0;i<300;++i){if(stop.stop_requested()){++cancelled;throw sat::Cancelled{};}std::this_thread::sleep_for(5ms);}}
  return {"ja",{{"ここから先は危険だ。","从这里开始很危险。",{100,100,800,300}},{"戻る","返回",{100,650,200,200}}}};
 }
};
HWND Find(const wchar_t* name){HWND h{};while((h=FindWindowExW(nullptr,h,name,nullptr))){DWORD pid{};GetWindowThreadProcessId(h,&pid);if(pid==GetCurrentProcessId())return h;}return nullptr;}
template<class F> void Wait(F pred,const char* error,int timeout=6000){auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);while(!pred()){if(std::chrono::steady_clock::now()>end)throw std::runtime_error(error);std::this_thread::sleep_for(10ms);}}
RECT Choose(int offset=0){Wait([]{return Find(L"SAT.Selection")!=nullptr;},"frozen selection missing");auto selection=Find(L"SAT.Selection");RECT screen{};GetWindowRect(selection,&screen);SendMessageW(selection,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(100+offset,120));SendMessageW(selection,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(600+offset,310));SendMessageW(selection,WM_LBUTTONUP,0,MAKELPARAM(600+offset,310));return {screen.left+100+offset,screen.top+120,screen.left+600+offset,screen.top+310};}
void MouseAt(HWND window,UINT message,WPARAM buttons,POINT screen){ScreenToClient(window,&screen);SendMessageW(window,message,buttons,MAKELPARAM(screen.x,screen.y));}
void CheckRegion(RECT expected,const char* error){RECT actual{};if(!GetWindowRect(Find(L"SAT.Overlay"),&actual)||!EqualRect(&actual,&expected)||Find(L"SAT.Selection"))throw std::runtime_error(error);}
void WaitTranslation(int calls){Wait([&]{return translations.load()==calls&&Find(L"SAT.TranslationToolbar");},"translation did not finish exactly once");}
void RepeatFixed(HWND controller,RECT expected){auto calls=translations.load();PostMessageW(controller,WM_HOTKEY,2,0);WaitTranslation(calls+1);CheckRegion(expected,"fixed translation did not follow the latest region");}
RECT DragResult(HWND controller,RECT region,int dx,int dy,bool cancel=false){
 auto overlay=Find(L"SAT.Overlay");RECT view{};GetWindowRect(overlay,&view);auto calls=translations.load();POINT start{view.left+1,view.top+60},end{start.x+dx,start.y+dy};
 MouseAt(overlay,WM_LBUTTONDOWN,MK_LBUTTON,start);MouseAt(overlay,WM_MOUSEMOVE,MK_LBUTTON,end);auto preview=view;OffsetRect(&preview,dx,dy);CheckRegion(preview,"drag preview did not track pointer");
 SendMessageW(controller,WM_TIMER,1,0);if(!IsWindow(overlay)||translations.load()!=calls)throw std::runtime_error("drag was auto-hidden or translated before release");
 if(cancel){SendMessageW(overlay,WM_CANCELMODE,0,0);CheckRegion(view,"cancelled drag did not restore the visible frame");return region;}
 MouseAt(overlay,WM_LBUTTONUP,0,end);OffsetRect(&region,dx,dy);
 WaitTranslation(calls+1);CheckRegion(region,"drag retranslation used wrong region or reading dimensions");return region;
}
unsigned long long CpuTime(){FILETIME created{},exited{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;return k.QuadPart+u.QuadPart;}
void HotkeyTransaction(){
 HWND h=CreateWindowExW(0,L"STATIC",L"SAT hotkey test",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);if(!h)throw std::runtime_error("hotkey test window failed");UINT modifiers=MOD_CONTROL|MOD_ALT|MOD_SHIFT;std::vector<UINT> keys;
 for(UINT k=VK_F13;k<=VK_F24&&keys.size()<2;++k)if(RegisterHotKey(h,300+static_cast<int>(keys.size()),modifiers|MOD_NOREPEAT,k))keys.push_back(k);
 if(keys.size()!=2){for(size_t i=0;i<keys.size();++i)UnregisterHotKey(h,300+static_cast<int>(i));DestroyWindow(h);throw std::runtime_error("no free test hotkeys");}
 UnregisterHotKey(h,300);bool passed{};{sat::Hotkeys manager(h);sat::Settings s;s.hotkeys[0]={modifiers,keys[0]};if(manager.Apply(s).empty()){s.hotkeys[0].key=keys[1];auto conflict=manager.Apply(s);bool grabbed=RegisterHotKey(h,302,modifiers|MOD_NOREPEAT,keys[0])!=FALSE;if(grabbed)UnregisterHotKey(h,302);passed=!conflict.empty()&&!grabbed;}}
 UnregisterHotKey(h,301);DestroyWindow(h);if(!passed)throw std::runtime_error("hotkey conflict did not roll back");
}
}
int main(){
 std::filesystem::path root=std::filesystem::temp_directory_path()/(L"SAT-integration-"+std::to_wstring(GetCurrentProcessId()));
 try{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);sat::WinUIRuntime ui;INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_WIN95_CLASSES|ICC_HOTKEY_CLASS};InitCommonControlsEx(&cc);
  HotkeyTransaction();sat::Store store(root);sat::Settings options;options.startup=false;options.autoHideSeconds=1;options.encryptedKey=sat::ProtectSecret("test-only-key");store.SaveSettings(options);
  {auto monitor=sat::MonitorAtCursor();nlohmann::json legacy={{"version",1},{"tab",0},{"fixed",{{"monitorId",sat::Utf8(monitor.id)},{"x",.1},{"y",.1},{"width",.2},{"height",.2},{"monitorWidth",sat::Width(monitor.rect)},{"monitorHeight",sat::Height(monitor.rect)}}}};std::ofstream file(root/L"state.json");file<<legacy.dump();}
  std::exception_ptr driverError;std::jthread driver([&]{HWND controller{};try{
    Wait([&]{controller=Find(L"SAT.Controller");return controller!=nullptr;},"controller missing");
    std::this_thread::sleep_for(100ms);auto cpu=CpuTime();std::this_thread::sleep_for(1500ms);auto delta=CpuTime()-cpu;PROCESS_MEMORY_COUNTERS counters{sizeof(counters)};GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters));std::cout<<"Idle sample: "<<delta/10000.0<<" ms CPU / 1500 ms; working set "<<counters.WorkingSetSize/(1024.0*1024.0)<<" MiB\n";if(Find(L"SAT.Settings")||Find(L"SAT.Selection")||Find(L"SAT.Overlay"))throw std::runtime_error("background mode displayed UI");
    // Even a legacy saved region must not skip the first manual selection.
    PostMessageW(controller,WM_HOTKEY,2,0);Wait([]{return Find(L"SAT.Selection")!=nullptr;},"first fixed action must select manually");SendMessageW(Find(L"SAT.Selection"),WM_KEYDOWN,VK_ESCAPE,0);Wait([]{return !Find(L"SAT.Selection");},"initial selection cancel failed");
    PostMessageW(controller,WM_HOTKEY,1,0);auto chosen=Choose();Wait([]{return Find(L"SAT.TranslationToolbar")!=nullptr;},"translated overlay missing");
    auto overlay=Find(L"SAT.Overlay");RECT bounds{};GetWindowRect(overlay,&bounds);if(!EqualRect(&bounds,&chosen))throw std::runtime_error("overlay bounds differ from selected region");
    SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(15,60));SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(15,60));std::this_thread::sleep_for(1300ms);if(!IsWindow(overlay))throw std::runtime_error("body click or disabled auto-hide dismissed translation");
    auto toolbar=Find(L"SAT.TranslationToolbar");if(!toolbar)throw std::runtime_error("translation toolbar missing");auto callsBeforeViews=translations.load();SendMessageW(toolbar,WM_COMMAND,1,0);SendMessageW(toolbar,WM_COMMAND,2,0);SendMessageW(toolbar,WM_COMMAND,0,0);if(translations.load()!=callsBeforeViews)throw std::runtime_error("local view switching called cloud provider");SendMessageW(toolbar,WM_COMMAND,5,0);Wait([]{return !Find(L"SAT.Overlay");},"close button did not dismiss overlay");
    PostMessageW(controller,WM_HOTKEY,2,0);Wait([]{return Find(L"SAT.TranslationToolbar")!=nullptr;},"fixed action did not reuse ordinary selection");
    GetWindowRect(Find(L"SAT.Overlay"),&bounds);if(!EqualRect(&bounds,&chosen)||Find(L"SAT.Selection"))throw std::runtime_error("fixed region differs from ordinary selection");
    PostMessageW(controller,WM_HOTKEY,2,0);Wait([]{return translations.load()>=3;},"fixed repeat did not translate");Wait([]{return Find(L"SAT.TranslationToolbar")!=nullptr;},"repeated fixed overlay missing");if(Find(L"SAT.Selection"))throw std::runtime_error("fixed repeat unnecessarily selected");
    PostMessageW(controller,WM_HOTKEY,1,0);chosen=Choose(60);Wait([]{return Find(L"SAT.TranslationToolbar")!=nullptr;},"updated ordinary result missing");
    PostMessageW(controller,WM_HOTKEY,2,0);Wait([]{return translations.load()>=5;},"updated fixed repeat missing");Wait([]{return Find(L"SAT.TranslationToolbar")!=nullptr;},"updated fixed result missing");GetWindowRect(Find(L"SAT.Overlay"),&bounds);if(!EqualRect(&bounds,&chosen))throw std::runtime_error("latest ordinary selection did not replace region");
    // Moving results commits the same physical ROI used by the next fixed translation.
    chosen=DragResult(controller,chosen,35,25);RepeatFixed(controller,chosen);
    chosen=DragResult(controller,chosen,-20,15);RepeatFixed(controller,chosen);
    DragResult(controller,chosen,30,20,true);RepeatFixed(controller,chosen);
    SendMessageW(Find(L"SAT.TranslationToolbar"),WM_COMMAND,1,0);chosen=DragResult(controller,chosen,10,10);
    SendMessageW(Find(L"SAT.TranslationToolbar"),WM_COMMAND,2,0);chosen=DragResult(controller,chosen,-10,-10);RepeatFixed(controller,chosen);
    // Cancelling an in-flight moved translation must keep its last committed ROI.
    slow=true;auto calls=translations.load();overlay=Find(L"SAT.Overlay");POINT dragStart{chosen.left+1,chosen.top+60},dragEnd{chosen.left+26,chosen.top+75};
    MouseAt(overlay,WM_LBUTTONDOWN,MK_LBUTTON,dragStart);MouseAt(overlay,WM_LBUTTONUP,0,dragEnd);OffsetRect(&chosen,25,15);
    Wait([&]{return translations.load()==calls+1;},"slow moved request did not start");PostMessageW(controller,WM_HOTKEY,4,0);Wait([]{return cancelled.load()==1;},"active request was not cancelled");if(Find(L"SAT.Overlay"))throw std::runtime_error("stale overlay survived cancel");slow=false;RepeatFixed(controller,chosen);
    PostMessageW(controller,WM_HOTKEY,1,0);Wait([]{return Find(L"SAT.Selection")!=nullptr;},"second ordinary selection missing");SendMessageW(Find(L"SAT.Selection"),WM_KEYDOWN,VK_ESCAPE,0);Wait([]{return !Find(L"SAT.Selection");},"Esc did not close selection");
    RepeatFixed(controller,chosen);
   }catch(...){driverError=std::current_exception();}if(controller)PostMessageW(controller,WM_CLOSE,0,0);
  });
  sat::App app(true,root,[](const std::string&){return std::make_unique<FakeProvider>();});int code=app.Run();driver.join();if(driverError)std::rethrow_exception(driverError);if(code!=0)throw std::runtime_error("controller exit failed");if(std::filesystem::exists(root/"history")||std::filesystem::exists(root/"logs"))throw std::runtime_error("removed history/log module wrote files");
  {std::ifstream input(root/L"state.json");auto saved=nlohmann::json::parse(input);if(saved.contains("fixed"))throw std::runtime_error("session region leaked to disk");}
  translations=0;slow=false;driverError=nullptr;
  std::jthread restartDriver([&]{HWND controller{};try{
   Wait([&]{controller=Find(L"SAT.Controller");return controller!=nullptr;},"restart controller missing");PostMessageW(controller,WM_HOTKEY,2,0);auto chosen=Choose(100);Wait([]{return Find(L"SAT.TranslationToolbar")!=nullptr;},"fixed-first translation after restart failed");
   PostMessageW(controller,WM_HOTKEY,2,0);Wait([]{return translations.load()>=2;},"fixed-first selection not reused");Wait([]{return Find(L"SAT.TranslationToolbar")!=nullptr;},"fixed-first repeat result missing");RECT bounds{};GetWindowRect(Find(L"SAT.Overlay"),&bounds);if(!EqualRect(&bounds,&chosen)||Find(L"SAT.Selection"))throw std::runtime_error("fixed-first repeat did not reuse region");
  }catch(...){driverError=std::current_exception();}if(controller)PostMessageW(controller,WM_CLOSE,0,0);});
  sat::App restarted(true,root,[](const std::string&){return std::make_unique<FakeProvider>();});int restartCode=restarted.Run();restartDriver.join();if(driverError)std::rethrow_exception(driverError);if(restartCode)throw std::runtime_error("restart failed");
  std::filesystem::remove_all(root);std::cout<<"AppIntegrationTests: PASS (real desktop capture -> frozen selection -> ROI/JPEG -> fake provider -> DirectWrite overlay, border drag/retranslation, latest fixed region, reading geometry, session-only region reuse/restart, dismissal, Esc, cancellation, context)\n";return 0;
 }catch(const std::exception& e){std::cerr<<"AppIntegrationTests: FAIL: "<<e.what()<<"; isolated artifacts retained in "<<sat::Utf8(root.wstring())<<"\n";return 1;}
}
