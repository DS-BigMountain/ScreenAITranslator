#pragma once
#include "common/Types.h"
namespace winrt::Microsoft::UI::Xaml { struct FrameworkElement; }
namespace sat {
enum class SettingsAction { ClearContext, ReselectRegion, ClearRegion, Translate, FixedTranslate };
struct SettingsCallbacks {
 std::function<std::wstring(const Settings&,const std::string&)> apply;
 std::function<void(SettingsAction)> action;
 std::function<void(bool,const Settings&,const std::string&)> api;
 std::function<void(RECT,int)> closed;
};
class SettingsWindow {
 struct Impl;
 std::unique_ptr<Impl> ui_;
 HWND hwnd_{};
 Settings settings_;
 SettingsCallbacks callbacks_;
 int page_{};
 bool apiBusy_{};
 static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
 void Build(const std::string& key);
 void Layout();
 void Save();
 void StartApi(bool fetch);
 Settings ReadApi(bool requireModel) const;
 void InvalidateModels();
public:
 SettingsWindow();
 ~SettingsWindow();
 void Show(const Settings&,const std::string&,const PersistentState&,SettingsCallbacks);
 void Close();
 HWND Window() const { return hwnd_; }
 winrt::Microsoft::UI::Xaml::FrameworkElement Content() const;
 void Status(const std::wstring& text);
 void ApiBusy(bool busy);
 void ApiFinished(const std::vector<std::string>& models,const std::wstring& message,bool fetched);
};
}
