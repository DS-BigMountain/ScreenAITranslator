#include "SettingsWindow.h"
#include <commctrl.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>
namespace sat {
namespace {
enum { Startup=101,Tray,AutoHide=104,HideSeconds, NormalKey=120,FixedKey,OpenKey,CloseKey,WinNormal=130,WinFixed,WinOpen,WinClose,
 Provider=140,BaseUrl,Key,ShowKey,Model,Timeout,FetchModelsButton,TestApiButton,ApiResult,ThinkingMode, Quality=160,ContextEnabled,ContextSize,Prompt,ClearContext,Reselect,ClearRegion,
 Font=180,AutoFont,FontSize,TextColor,OutlineColor,OutlineWidth,Opacity,SpatialOverlay,
 Apply=220,CloseButton,Translate,TranslateFixed };
std::wstring Number(float v){std::wostringstream s;s<<v;return s.str();}
std::wstring Color(COLORREF c){wchar_t b[8];swprintf_s(b,L"%02X%02X%02X",GetRValue(c),GetGValue(c),GetBValue(c));return b;}
float ParseFloat(const std::wstring& t){size_t p{};auto n=std::stof(t,&p);if(p!=t.size()||!std::isfinite(n))throw std::runtime_error("number");return n;}
int ParseInt(const std::wstring& t){size_t p{};auto n=std::stoi(t,&p);if(p!=t.size())throw std::runtime_error("number");return n;}
COLORREF ParseColor(std::wstring t){if(!t.empty()&&t[0]==L'#')t.erase(0,1);if(t.size()!=6||t.find_first_not_of(L"0123456789abcdefABCDEF")!=t.npos)throw std::runtime_error("color");auto v=std::stoul(t,nullptr,16);return RGB((v>>16)&255,(v>>8)&255,v&255);}
}
SettingsWindow::~SettingsWindow(){Close();if(font_)DeleteObject(font_);if(headingFont_)DeleteObject(headingFont_);}
void SettingsWindow::Close(){if(hwnd_)DestroyWindow(hwnd_);hwnd_=nullptr;controls_.clear();items_.clear();if(!key_.empty())SecureZeroMemory(key_.data(),key_.size());key_.clear();}
void SettingsWindow::Show(const Settings& s,const std::string& key,const PersistentState& state,SettingsCallbacks cb){
 if(hwnd_){ShowWindow(hwnd_,SW_RESTORE);SetForegroundWindow(hwnd_);return;}
 settings_=s;key_=key;callbacks_=std::move(cb);page_=std::clamp(state.tab,0,4);keyVisible_=false;apiBusy_=false;models_.clear();
 WNDCLASSEXW wc{sizeof(wc)};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SAT.Settings";wc.lpfnWndProc=Proc;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);wc.hIcon=LoadIconW(wc.hInstance,MAKEINTRESOURCEW(101));wc.hIconSm=wc.hIcon;RegisterClassExW(&wc);
 POINT pt{};GetCursorPos(&pt);auto mon=MonitorFromPoint(pt,MONITOR_DEFAULTTOPRIMARY);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(mon,&mi);
 int x=mi.rcWork.left+50,y=mi.rcWork.top+50;if(state.window){auto r=*state.window;if(MonitorFromRect(&r,MONITOR_DEFAULTTONULL)){x=r.left;y=r.top;}}
 hwnd_=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"屏幕翻译 · Screen AI Translator",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,x,y,760,690,nullptr,nullptr,wc.hInstance,this);
 if(!hwnd_)throw AppError("settings","无法创建设置窗口");dpi_=GetDpiForWindow(hwnd_);Build();Layout();SelectPage();ShowWindow(hwnd_,SW_SHOW);SetForegroundWindow(hwnd_);
}
HWND SettingsWindow::Add(int page,int id,const wchar_t* kind,const std::wstring& text,DWORD style,int x,int y,int w,int h){
 HWND child=CreateWindowExW(wcscmp(kind,L"EDIT")==0?WS_EX_CLIENTEDGE:0,kind,text.c_str(),WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|style,0,0,0,0,hwnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
 if(!child)throw AppError("settings","无法创建设置控件");
 if(id)controls_[id]=child;items_.push_back({child,page,x,y,w,h});return child;
}
void SettingsWindow::Label(int page,const std::wstring& text,int x,int y,int w){Add(page,0,L"STATIC",text,SS_LEFT,x,y,w,25);}
void SettingsWindow::Edit(int page,int id,const std::wstring& value,int y,int width,DWORD style){auto h=Add(page,id,L"EDIT",value,WS_TABSTOP|ES_AUTOHSCROLL|style,218,y,width,28);SendMessageW(h,EM_SETLIMITTEXT,id==Key?4096:8192,0);}
void SettingsWindow::Check(int page,int id,const std::wstring& label,bool checked,int y){auto h=Add(page,id,L"BUTTON",label,WS_TABSTOP|BS_AUTOCHECKBOX,38,y,550,28);SendMessageW(h,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);}
void SettingsWindow::Build(){
 // Keep the tab strip separate from sibling content; a full-page tab window
 // can paint over native edit/static controls during themed redraw/WM_PRINT.
 tabs_=Add(-1,0,WC_TABCONTROLW,L"",WS_TABSTOP,18,18,704,34);
 const wchar_t* pages[]{L"使用偏好",L"快捷键",L"模型连接",L"翻译质量",L"显示效果"};for(int i=0;i<5;++i){TCITEMW t{};t.mask=TCIF_TEXT;t.pszText=const_cast<wchar_t*>(pages[i]);TabCtrl_InsertItem(tabs_,i,&t);}TabCtrl_SetCurSel(tabs_,page_);
 Check(0,Startup,L"Windows 登录后静默启动",settings_.startup,218);Check(0,Tray,L"显示系统托盘图标",settings_.tray,258);Label(0,L"固定区域跟随最后一次框选或拖动位置，退出程序后清空。",38,298,654);Check(0,AutoHide,L"自动隐藏翻译结果（默认关闭）",settings_.autoHide,338);
 Label(0,L"自动隐藏时间（秒）",38,384);Edit(0,HideSeconds,std::to_wstring(settings_.autoHideSeconds),380,150,ES_NUMBER);
 Label(0,L"关闭设置后在后台运行。截图只在主动触发时进行。",38,438,640);
 Label(0,L"关闭托盘前，请先配置“打开设置”快捷键。",38,474,640);
 Add(0,Translate,L"BUTTON",L"开始屏幕翻译",WS_TABSTOP|BS_OWNERDRAW,38,80,250,46);Add(0,TranslateFixed,L"BUTTON",L"固定区域翻译",WS_TABSTOP,306,80,250,46);
 Label(0,L"01 框选屏幕   →   02 等待翻译   →   03 原位阅读 / 对照原图",38,149,660);
 Label(0,key_.empty()?L"首次使用：在“模型连接”中填写 API，再保存设置。":L"模型已连接配置 · 截图只发送给你选择的服务。",38,181,660);
 const wchar_t* actions[]{L"普通截图翻译",L"固定区域翻译",L"打开设置",L"关闭翻译层"};
 for(int i=0;i<4;++i){int y=86+i*65;Label(1,actions[i],38,y+4);auto h=Add(1,NormalKey+i,HOTKEY_CLASSW,L"",WS_TABSTOP,218,y,286,30);
  WORD f=0;auto k=settings_.hotkeys[i];if(k.modifiers&MOD_CONTROL)f|=HOTKEYF_CONTROL;if(k.modifiers&MOD_ALT)f|=HOTKEYF_ALT;if(k.modifiers&MOD_SHIFT)f|=HOTKEYF_SHIFT;if(k.key>=VK_PRIOR&&k.key<=VK_DELETE)f|=HOTKEYF_EXT;
  SendMessageW(h,HKM_SETHOTKEY,MAKEWORD(k.key,f),0);auto win=Add(1,WinNormal+i,L"BUTTON",L"Win 键",WS_TABSTOP|BS_AUTOCHECKBOX,526,y,120,30);SendMessageW(win,BM_SETCHECK,(k.modifiers&MOD_WIN)?BST_CHECKED:BST_UNCHECKED,0);
 }
 Label(1,L"在输入框中直接按下组合键；退格键可清除。",38,370,640);Label(1,L"不预设快捷键。重复或被占用的组合键无法保存。",38,410,640);
 Label(2,L"Provider",38,82);auto provider=Add(2,Provider,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,218,78,420,180);SendMessageW(provider,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"DeepSeek"));SendMessageW(provider,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"OpenAI Compatible"));SendMessageW(provider,CB_SETCURSEL,settings_.provider=="deepseek"?0:1,0);
 Label(2,L"API Base URL",38,132);Edit(2,BaseUrl,settings_.baseUrl,128);
 Label(2,L"API Key",38,182);Edit(2,Key,Wide(key_),178,322,ES_PASSWORD);Add(2,ShowKey,L"BUTTON",L"显示",WS_TABSTOP,550,178,88,28);
 Label(2,L"Model",38,232);Add(2,Model,WC_COMBOBOXW,Wide(settings_.model),WS_TABSTOP|CBS_DROPDOWN|WS_VSCROLL,218,228,420,220);
 SetWindowTextW(controls_.at(Model),Wide(settings_.model).c_str());
 Label(2,L"Timeout（秒）",38,282);Edit(2,Timeout,std::to_wstring(settings_.timeoutSeconds),278,150,ES_NUMBER);
 Label(2,L"思考模式",402,282,100);auto thinking=Add(2,ThinkingMode,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,512,278,126,100);
 SendMessageW(thinking,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"关"));SendMessageW(thinking,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"high"));SendMessageW(thinking,CB_SETCURSEL,settings_.thinkingHigh?1:0,0);
 Add(2,FetchModelsButton,L"BUTTON",L"获取模型",WS_TABSTOP,218,328,194,34);
 Add(2,TestApiButton,L"BUTTON",L"测试 API",WS_TABSTOP,428,328,210,34);
 Label(2,L"选择或输入模型名称；须支持图片输入。建议先测试 API。",38,378,654);
 Label(2,L"测试不上传桌面截图，会产生一次小图片 API 请求。",38,412,654);
 Add(2,ApiResult,L"EDIT",settings_.encryptedKey.empty()?L"填写地址、密钥与模型名称；也可获取模型列表。":L"已保存模型："+Wide(settings_.model)+L"。获取列表后可重新选择。",ES_READONLY|ES_MULTILINE|WS_VSCROLL,38,448,654,102);
 Label(3,L"图片质量",38,82);auto quality=Add(3,Quality,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,218,78,420,180);for(auto t:{L"速度 · 1280 px",L"均衡 · 1600 px",L"质量 · 2200 px"})SendMessageW(quality,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(t));SendMessageW(quality,CB_SETCURSEL,settings_.quality,0);
 Check(3,ContextEnabled,L"使用最近翻译上下文（仅保存在内存）",settings_.contextEnabled,121);Label(3,L"上下文数量（5–10）",38,166);Edit(3,ContextSize,std::to_wstring(settings_.contextSize),162,150,ES_NUMBER);
 Label(3,L"自定义翻译要求",38,212,640);auto prompt=Add(3,Prompt,L"EDIT",Wide(settings_.prompt),WS_TABSTOP|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN,38,244,622,166);SendMessageW(prompt,EM_SETLIMITTEXT,8192,0);
 Add(3,ClearContext,L"BUTTON",L"清空上下文",WS_TABSTOP,38,435,170,34);Add(3,Reselect,L"BUTTON",L"重新选择固定区域",WS_TABSTOP,224,435,222,34);Add(3,ClearRegion,L"BUTTON",L"清除固定区域",WS_TABSTOP,462,435,194,34);
 Label(4,L"字体",38,82);Edit(4,Font,settings_.font,78);
 Check(4,AutoFont,L"自动估算字号",settings_.autoFont,120);Label(4,L"固定字号（DIP）",38,164);Edit(4,FontSize,Number(settings_.fontSize),160,150);
 Label(4,L"文字颜色（RRGGBB）",38,211);Edit(4,TextColor,Color(settings_.textColor),207,150);
 Label(4,L"描边颜色（RRGGBB）",38,258);Edit(4,OutlineColor,Color(settings_.outlineColor),254,150);
 Label(4,L"描边宽度（DIP）",38,305);Edit(4,OutlineWidth,Number(settings_.outlineWidth),301,150);
 Label(4,L"复杂背景不透明度",38,352);Edit(4,Opacity,Number(settings_.backgroundOpacity),348,150);Label(4,L"0.1–1.0",388,352,140);
 Check(4,SpatialOverlay,L"原位覆盖译文，并显示原图 / 全文阅读工具条",settings_.spatialOverlay,405);
 Label(4,L"较长内容用全文阅读查看；关闭此项则使用传统译文框。",38,446,640);
 status_=Add(-1,0,L"STATIC",L"设置修改后点击保存。",SS_LEFT,26,570,500,48);
 Add(-1,Apply,L"BUTTON",L"保存设置",WS_TABSTOP|BS_OWNERDRAW,530,579,112,36);Add(-1,CloseButton,L"BUTTON",L"关闭",WS_TABSTOP,651,579,71,36);
 for(auto& item:items_)item.y+=68;
 Add(-1,240,L"STATIC",L"屏幕翻译",SS_LEFT,28,15,300,32);
 Add(-1,241,L"STATIC",L"框选即译，让理解留在原处。",SS_LEFT,29,48,470,23);
 Add(-1,242,L"STATIC",L"SCREEN / AI   1.1",SS_RIGHT,510,24,200,24);
 SetWindowPos(tabs_,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
void SettingsWindow::Layout(){
 dpi_=GetDpiForWindow(hwnd_);if(!dpi_)dpi_=96;auto s=[&](int n){return MulDiv(n,dpi_,96);};
 HFONT next=CreateFontW(-s(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
 for(auto item:items_){MoveWindow(item.handle,s(item.x),s(item.y),s(item.w),s(item.h),TRUE);SendMessageW(item.handle,WM_SETFONT,reinterpret_cast<WPARAM>(next),TRUE);}
  if(font_)DeleteObject(font_);font_=next;
 if(headingFont_)DeleteObject(headingFont_);headingFont_=CreateFontW(-s(24),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
 SendMessageW(controls_.at(240),WM_SETFONT,reinterpret_cast<WPARAM>(headingFont_),TRUE);
 TabCtrl_SetPadding(tabs_,s(16),s(6));RECT r{0,0,s(742),s(702)};AdjustWindowRectExForDpi(&r,static_cast<DWORD>(GetWindowLongPtrW(hwnd_,GWL_STYLE)),FALSE,0,dpi_);
 SetWindowPos(hwnd_,nullptr,0,0,Width(r),Height(r),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
}
void SettingsWindow::SelectPage(){page_=TabCtrl_GetCurSel(tabs_);for(auto item:items_)if(item.page>=0)ShowWindow(item.handle,item.page==page_?SW_SHOW:SW_HIDE);}
std::wstring SettingsWindow::Text(int id)const{auto h=controls_.at(id);int n=GetWindowTextLengthW(h);std::wstring t(n+1,0);GetWindowTextW(h,t.data(),n+1);t.resize(n);return t;}
bool SettingsWindow::Checked(int id)const{return SendMessageW(controls_.at(id),BM_GETCHECK,0,0)==BST_CHECKED;}
void SettingsWindow::Status(const std::wstring& t){if(status_)SetWindowTextW(status_,t.c_str());}
void SettingsWindow::ApiBusy(bool busy){
 apiBusy_=busy;if(!hwnd_)return;
 for(int id:{Provider,BaseUrl,Key,Model,Timeout,ThinkingMode,FetchModelsButton,TestApiButton,Apply})EnableWindow(controls_.at(id),!busy);
}
void SettingsWindow::InvalidateModels(){
 models_.clear();SendMessageW(controls_.at(Model),CB_RESETCONTENT,0,0);SetWindowTextW(controls_.at(Model),L"");
 SetWindowTextW(controls_.at(ApiResult),L"连接配置已改变，请输入模型名称或重新获取列表。");
}
Settings SettingsWindow::ReadApi(bool requireModel)const{
 Settings value=settings_;value.provider=SendMessageW(controls_.at(Provider),CB_GETCURSEL,0,0)==0?"deepseek":"openai-compatible";
 value.thinkingHigh=SendMessageW(controls_.at(ThinkingMode),CB_GETCURSEL,0,0)==1;value.baseUrl=Text(BaseUrl);value.timeoutSeconds=ParseInt(Text(Timeout));
 auto selected=SendMessageW(controls_.at(Model),CB_GETCURSEL,0,0);
 if(selected>=0&&static_cast<size_t>(selected)<models_.size())value.model=models_[static_cast<size_t>(selected)];
 else {auto name=Text(Model);if(!name.empty())value.model=Utf8(name);else if(requireModel)throw AppError("settings","请选择或输入支持图片的模型名称");}
 return value;
}
void SettingsWindow::StartApi(bool fetch){
 if(apiBusy_||!callbacks_.api)return;
 auto options=ReadApi(!fetch);auto secret=Utf8(Text(Key));
 if(secret.empty())throw AppError("settings","请先填写 API Key");
 ApiBusy(true);SetWindowTextW(controls_.at(ApiResult),fetch?L"正在获取模型列表…":L"正在测试图片识别与翻译…");
 try{callbacks_.api(fetch,options,secret);}
 catch(const AppError& e){SecureZeroMemory(secret.data(),secret.size());ApiFinished({},Wide(e.what()),false);throw;}
 catch(...){SecureZeroMemory(secret.data(),secret.size());ApiFinished({},L"操作未完成，请检查设置。",false);throw;}
 SecureZeroMemory(secret.data(),secret.size());
}
void SettingsWindow::ApiFinished(const std::vector<std::string>& models,const std::wstring& message,bool fetched){
 if(!hwnd_)return;ApiBusy(false);
 if(fetched){models_=models;SendMessageW(controls_.at(Model),CB_RESETCONTENT,0,0);int selected=-1;
  for(size_t i=0;i<models_.size();++i){auto name=Wide(models_[i]);SendMessageW(controls_.at(Model),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));if(models_[i]==settings_.model)selected=static_cast<int>(i);}
  if(selected>=0)SendMessageW(controls_.at(Model),CB_SETCURSEL,selected,0);
  else if(models_.size()==1)SendMessageW(controls_.at(Model),CB_SETCURSEL,0,0);
 }
 SetWindowTextW(controls_.at(ApiResult),message.c_str());
}
void SettingsWindow::Save(){
 try{
  Settings next=settings_;next.startup=Checked(Startup);next.tray=Checked(Tray);next.autoHide=Checked(AutoHide);next.autoHideSeconds=ParseInt(Text(HideSeconds));
  for(int i=0;i<4;++i){auto v=SendMessageW(controls_.at(NormalKey+i),HKM_GETHOTKEY,0,0);Hotkey k{};k.key=LOBYTE(v);auto m=HIBYTE(v);if(m&HOTKEYF_CONTROL)k.modifiers|=MOD_CONTROL;if(m&HOTKEYF_SHIFT)k.modifiers|=MOD_SHIFT;if(m&HOTKEYF_ALT)k.modifiers|=MOD_ALT;if(Checked(WinNormal+i))k.modifiers|=MOD_WIN;if(!k.key)k.modifiers=0;next.hotkeys[i]=k;}
  auto api=ReadApi(true);next.provider=api.provider;next.baseUrl=api.baseUrl;next.model=api.model;next.timeoutSeconds=api.timeoutSeconds;next.thinkingHigh=api.thinkingHigh;


  next.quality=static_cast<int>(SendMessageW(controls_.at(Quality),CB_GETCURSEL,0,0));next.contextEnabled=Checked(ContextEnabled);next.contextSize=ParseInt(Text(ContextSize));next.prompt=Utf8(Text(Prompt));
  next.spatialOverlay=Checked(SpatialOverlay);next.font=Text(Font);next.autoFont=Checked(AutoFont);next.fontSize=ParseFloat(Text(FontSize));next.textColor=ParseColor(Text(TextColor));next.outlineColor=ParseColor(Text(OutlineColor));next.outlineWidth=ParseFloat(Text(OutlineWidth));next.backgroundOpacity=ParseFloat(Text(Opacity));
  auto secret=Utf8(Text(Key));std::wstring failure;try{failure=callbacks_.apply(next,secret);}catch(...){SecureZeroMemory(secret.data(),secret.size());throw;}
  if(!failure.empty()){SecureZeroMemory(secret.data(),secret.size());Status(failure);return;}
  if(!key_.empty())SecureZeroMemory(key_.data(),key_.size());key_=std::move(secret);
  settings_=next;Status(L"已保存。关闭此窗口后继续在后台运行。");
 }catch(const AppError& e){Status(Wide(e.what()));}catch(...){Status(L"请检查数值格式、范围和六位十六进制颜色。");}
}
void SettingsWindow::Command(int id){
 if(id==FetchModelsButton||id==TestApiButton){StartApi(id==FetchModelsButton);return;}
 if(id==Apply){Save();return;}if(id==CloseButton||id==IDCANCEL){SendMessageW(hwnd_,WM_CLOSE,0,0);return;}
 if(id==ShowKey){keyVisible_=!keyVisible_;SendMessageW(controls_.at(Key),EM_SETPASSWORDCHAR,keyVisible_?0:L'●',0);SetWindowTextW(controls_.at(ShowKey),keyVisible_?L"隐藏":L"显示");InvalidateRect(controls_.at(Key),nullptr,TRUE);return;}
 if(!callbacks_.action)return;
 switch(id){case ClearContext:callbacks_.action(SettingsAction::ClearContext);Status(L"上下文已清空。");break;case Reselect:callbacks_.action(SettingsAction::ReselectRegion);break;case ClearRegion:callbacks_.action(SettingsAction::ClearRegion);Status(L"固定区域已清除。");break;case Translate:callbacks_.action(SettingsAction::Translate);break;case TranslateFixed:callbacks_.action(SettingsAction::FixedTranslate);break;}
}
LRESULT CALLBACK SettingsWindow::Proc(HWND h,UINT m,WPARAM w,LPARAM l){
 auto self=reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(h,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);self->hwnd_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(h,m,w,l);
 try{switch(m){
 case WM_DRAWITEM:{auto item=reinterpret_cast<DRAWITEMSTRUCT*>(l);if(item->CtlID!=Apply&&item->CtlID!=Translate)break;
 auto dc=item->hDC;bool disabled=(item->itemState&ODS_DISABLED)!=0;auto brush=CreateSolidBrush(disabled?RGB(155,164,190):(item->itemState&ODS_SELECTED)?RGB(46,67,167):RGB(66,91,207));
 FillRect(dc,&item->rcItem,brush);DeleteObject(brush);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));auto previous=SelectObject(dc,self->font_);
 auto label=self->Text(item->CtlID);DrawTextW(dc,label.c_str(),-1,&item->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);if(item->itemState&ODS_FOCUS){auto focus=item->rcItem;InflateRect(&focus,-4,-4);DrawFocusRect(dc,&focus);}SelectObject(dc,previous);return TRUE;}
 case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:SetBkColor(reinterpret_cast<HDC>(w),RGB(255,255,255));SetTextColor(reinterpret_cast<HDC>(w),RGB(20,20,20));return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
 case WM_COMMAND:{int id=LOWORD(w);int notification=HIWORD(w);
 if((id==BaseUrl||id==Key)&&notification==EN_CHANGE&&!self->apiBusy_&&self->controls_.contains(ApiResult))self->InvalidateModels();
 if(id==Provider&&notification==CBN_SELCHANGE&&!self->apiBusy_){self->InvalidateModels();auto index=SendMessageW(self->controls_.at(Provider),CB_GETCURSEL,0,0);auto address=self->Text(BaseUrl);if(index==1&&address==L"https://api.deepseek.com")SetWindowTextW(self->controls_.at(BaseUrl),L"https://api.openai.com/v1");else if(index==0&&address==L"https://api.openai.com/v1")SetWindowTextW(self->controls_.at(BaseUrl),L"https://api.deepseek.com");}
 if(notification==BN_CLICKED)self->Command(id);return 0;}case WM_NOTIFY:if(reinterpret_cast<NMHDR*>(l)->hwndFrom==self->tabs_&&reinterpret_cast<NMHDR*>(l)->code==TCN_SELCHANGE){self->SelectPage();return 0;}break;
 case WM_DPICHANGED:{auto r=reinterpret_cast<RECT*>(l);SetWindowPos(h,nullptr,r->left,r->top,Width(*r),Height(*r),SWP_NOZORDER|SWP_NOACTIVATE);self->Layout();return 0;}
 case WM_CLOSE:{RECT r{};GetWindowRect(h,&r);if(self->callbacks_.closed)self->callbacks_.closed(r,self->page_);self->Close();return 0;}
 case WM_NCDESTROY:self->hwnd_=nullptr;self->status_=nullptr;SetWindowLongPtrW(h,GWLP_USERDATA,0);break;
 }}catch(const AppError& e){self->Status(Wide(e.what()));}catch(...){self->Status(L"操作未完成，请检查设置。");}
 return DefWindowProcW(h,m,w,l);
}
}





