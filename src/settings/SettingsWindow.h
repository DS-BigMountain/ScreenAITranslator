#pragma once
#include "common/Types.h"
#include "TextTranslationPage.h"
namespace winrt::Microsoft::UI::Xaml {
struct FrameworkElement;
}
namespace sat {
enum class SettingsAction { ClearContext, Translate, FixedTranslate };
struct SettingsCallbacks {
    std::function<std::wstring(const Settings&, const std::string&)> apply;
    std::function<void(SettingsAction)> action;
    std::function<void(bool, const Settings&, const std::string&)> api;
    std::function<void(RECT, int, int)> closed;
    std::function<void(bool)> hotkeyEditing;
    std::function<void(const std::string&)> targetLanguage;
};
class SettingsWindow {
    friend class TextTranslationUiTest;
    struct Impl;
    std::unique_ptr<Impl> ui_;
    TextTranslationPage textPage_;
    HWND hwnd_{};
    Settings settings_;
    SettingsCallbacks callbacks_;
    int page_{};
    int settingsTab_{};
    bool apiBusy_{};
    bool hotkeyEditing_{};
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    void Build(const std::string& key);
    void UpdateIcons();
    void UpdateCaptionTheme();
    void Layout();
    void Save();
    void StartApi(bool fetch);
    Settings ReadApi(bool requireModel) const;
    void InvalidateModels();
    void UpdateTranslationMode();
    void SetHotkeyEditing(bool editing);
    void SelectPage(int page);
    void SelectSettingsTab(int tab);

public:
    SettingsWindow();
    ~SettingsWindow();
    void Show(const Settings&, const std::string&, const PersistentState&, SettingsCallbacks,
              bool visible = true);
    void Close();
    HWND Window() const {
        return hwnd_;
    }
    winrt::Microsoft::UI::Xaml::FrameworkElement Content() const;
    void Status(const std::wstring& text);
    void ApiBusy(bool busy);
    void ApiFinished(const std::vector<std::string>& models, const std::wstring& message, bool fetched);
    bool BackgroundIdle() {
        return textPage_.BackgroundIdle();
    }
    std::string TextSession() {
        return textPage_.Snapshot();
    }
    void RestoreTextSession(const std::string& value) {
        textPage_.Restore(value);
    }
};
} // namespace sat
