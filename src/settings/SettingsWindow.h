#pragma once
#include "common/Types.h"
#include <map>
namespace sat {
enum class SettingsAction { ClearContext, ReselectRegion, ClearRegion, Translate, FixedTranslate };
struct SettingsCallbacks {
 std::function<std::wstring(const Settings&,const std::string&)> apply;
 std::function<void(SettingsAction)> action;
 std::function<void(bool,const Settings&,const std::string&)> api;
 std::function<void(RECT,int)> closed;
};
class SettingsWindow {
 HWND hwnd_{},tabs_{},status_{};HFONT font_{},headingFont_{};
 Settings settings_;std::string key_;SettingsCallbacks callbacks_;
 std::map<int,HWND> controls_;
 struct Item{HWND handle;int page,x,y,w,h;};std::vector<Item> items_;
 int page_{};UINT dpi_{96};bool keyVisible_{};
 bool apiBusy_{};std::vector<std::string> models_;
 static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
 HWND Add(int page,int id,const wchar_t* kind,const std::wstring& text,DWORD style,int x,int y,int w,int h);
 void Label(int page,const std::wstring& text,int x,int y,int w=170);
 void Edit(int page,int id,const std::wstring& value,int y,int width=420,DWORD style=0);
 void Check(int page,int id,const std::wstring& label,bool checked,int y);
 void Build();void Layout();void SelectPage();void Save();void Command(int id);
 std::wstring Text(int id)const;bool Checked(int id)const;
 Settings ReadApi(bool requireModel)const;void StartApi(bool fetch);void InvalidateModels();
public:
 ~SettingsWindow();
 void Show(const Settings&,const std::string&,const PersistentState&,SettingsCallbacks);
 void Close();HWND Window()const{return hwnd_;}
 void Status(const std::wstring& text);
 void ApiBusy(bool busy);
 void ApiFinished(const std::vector<std::string>& models,const std::wstring& message,bool fetched);
};
}

