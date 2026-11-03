#include "SettingsWindow.h"
#include "common/AppIdentity.h"
#include "common/Secret.h"
#include "common/Platform.h"
#include "WinUIRuntime.h"
#include "Store.h"
#include "providers/Catalog.h"
#include "providers/RequestParameters.h"
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <shellapi.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <dwrite.h>

namespace sat {
namespace xaml = winrt::Microsoft::UI::Xaml;
using namespace xaml::Controls;
namespace {
std::wstring Color(COLORREF c) {
    wchar_t b[8];
    swprintf_s(b, L"%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return b;
}
COLORREF ParseColor(std::wstring t) {
    if (!t.empty() && t[0] == L'#')
        t.erase(0, 1);
    if (t.size() != 6 || t.find_first_not_of(L"0123456789abcdefABCDEF") != t.npos)
        throw AppError("settings", "颜色须为六位十六进制数值。");
    auto v = std::stoul(t, nullptr, 16);
    return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
}
std::wstring HotkeyLabel(Hotkey key) {
    if (!key.key)
        return {};
    std::wstring label;
    if (key.modifiers & MOD_CONTROL)
        label += L"Ctrl + ";
    if (key.modifiers & MOD_ALT)
        label += L"Alt + ";
    if (key.modifiers & MOD_SHIFT)
        label += L"Shift + ";
    if (key.modifiers & MOD_WIN)
        label += L"Win + ";
    wchar_t name[64]{};
    auto scan = MapVirtualKeyW(key.key, MAPVK_VK_TO_VSC);
    if (key.key >= VK_PRIOR && key.key <= VK_DELETE)
        scan |= 0x100;
    GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64);
    return label + (name[0] ? std::wstring(name) : std::to_wstring(key.key));
}
std::wstring FontLabel(IDWriteFontCollection* fonts, const std::wstring& name) {
    if (!fonts)
        return name;
    UINT32 index{};
    BOOL exists{};
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteLocalizedStrings> names;
    if (FAILED(fonts->FindFamilyName(name.c_str(), &index, &exists)) || !exists ||
        FAILED(fonts->GetFontFamily(index, &family)) || FAILED(family->GetFamilyNames(&names)))
        return name;
    for (auto locale : {L"zh-cn", L"zh-hans", L"zh-sg", L"zh-tw", L"zh-hk", L"zh"}) {
        if (FAILED(names->FindLocaleName(locale, &index, &exists)) || !exists)
            continue;
        UINT32 length{};
        if (FAILED(names->GetStringLength(index, &length)))
            continue;
        std::wstring label(length + 1, L'\0');
        if (SUCCEEDED(names->GetString(index, label.data(), length + 1))) {
            label.resize(length);
            return label;
        }
    }
    return name;
}
const PROPERTYKEY TaskbarProperties[]{PKEY_AppUserModel_ID, PKEY_AppUserModel_RelaunchIconResource,
                                      PKEY_AppUserModel_RelaunchCommand,
                                      PKEY_AppUserModel_RelaunchDisplayNameResource};
void ShowForUser(HWND window, int command) {
    ShowWindow(window, command);
    // A background launch's STARTF_USESHOWWINDOW/SW_HIDE can override the first ShowWindow.
    // An explicit settings request must still reveal the window on that same request.
    SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
    SetForegroundWindow(window);
}
} // namespace
struct SettingsWindow::Impl {
    xaml::Hosting::DesktopWindowXamlSource island{nullptr};
    xaml::FrameworkElement root{nullptr};
    xaml::Input::FocusManager::GotFocus_revoker focusGained;
    xaml::Input::FocusManager::LostFocus_revoker focusLost;
    xaml::FrameworkElement::ActualThemeChanged_revoker themeChanged;
    Hotkey hotkeys[3]{};
    bool updating{};
    bool focusPending{};
    bool hotkeyFocusPending{};
    bool customCaption{};
    std::map<std::string, ConnectionProfile> profiles;
    std::string activeProvider;
    std::vector<std::pair<std::wstring, std::wstring>> fontNames;
    template <class T> T Get(const wchar_t* name) const {
        return root.FindName(name).as<T>();
    }
    std::wstring Text(const wchar_t* name) const {
        return std::wstring(Get<TextBox>(name).Text());
    }
    bool On(const wchar_t* name) const {
        return Get<ToggleSwitch>(name).IsOn();
    }
    bool IsHotkeyEditor(xaml::DependencyObject element) const {
        for (; element; element = xaml::Media::VisualTreeHelper::GetParent(element)) {
            if (auto control = element.try_as<TextBox>()) {
                auto name = control.Name();
                if (name == L"Hotkey0" || name == L"Hotkey1" || name == L"Hotkey2")
                    return true;
            }
        }
        return false;
    }
    bool FocusedHotkey() const {
        return Get<StackPanel>(L"SettingsPage").Visibility() == xaml::Visibility::Visible &&
               Get<StackPanel>(L"SettingsGeneral").Visibility() == xaml::Visibility::Visible &&
               root.XamlRoot() &&
               IsHotkeyEditor(xaml::Input::FocusManager::GetFocusedElement(root.XamlRoot())
                                  .try_as<xaml::DependencyObject>());
    }
    std::wstring ReadFont() const {
        const auto name = std::wstring(Get<ComboBox>(L"Font").Text());
        for (const auto& [label, family] : fontNames)
            if (name == label)
                return family;
        return name;
    }
    ConnectionProfile ReadConnection() const {
        ConnectionProfile profile;
        profile.baseUrl = Text(L"BaseUrl");
        {
            auto control = Get<ComboBox>(L"Model");
            auto name = control.Text();
            if (name.empty() && control.SelectedItem())
                name = winrt::unbox_value<winrt::hstring>(control.SelectedItem());
            profile.model = Utf8(std::wstring(name));
            profile.serviceRegion = Utf8(Text(L"ServiceRegion"));
        }
        Secret secret{Utf8(std::wstring(Get<PasswordBox>(L"Key").Password()))};
        profile.encryptedKey = ProtectSecret(secret.value);
        profile.requestParameters = Utf8(Text(L"RequestParameters"));
        return profile;
    }
    double Number(const wchar_t* name) const {
        auto control = Get<NumberBox>(name);
        auto value = control.Value();
        if (!std::isfinite(value))
            throw AppError("settings", "请填写有效数值。");
        return value;
    }
    int Integer(const wchar_t* name) const {
        auto value = Number(name);
        if (std::floor(value) != value)
            throw AppError("settings", "秒数与上下文数量须为整数。");
        return static_cast<int>(value);
    }
};
SettingsWindow::SettingsWindow() = default;
SettingsWindow::~SettingsWindow() {
    Close();
}
xaml::FrameworkElement SettingsWindow::Content() const {
    return ui_ ? ui_->root : nullptr;
}
void SettingsWindow::Show(const Settings& s, const std::string& key, const PersistentState& state,
                          SettingsCallbacks cb, bool visible) {
    WinUIRuntime::EnsureInitialized();
    if (hwnd_) {
        if (visible) {
            ShowForUser(hwnd_, SW_RESTORE);
        }
        return;
    }
    settings_ = s;
    callbacks_ = std::move(cb);
    page_ = std::clamp(state.tab, 0, 2);
    settingsTab_ = std::clamp(state.settingsTab, 0, 3);
    apiBusy_ = false;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SAT.Settings";
    wc.lpfnWndProc = Proc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(101));
    wc.hIconSm = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                               LR_SHARED));
    RegisterClassExW(&wc);
    POINT pt{};
    GetCursorPos(&pt);
    auto mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    auto dpi = GetDpiForSystem();
    int width = std::min(MulDiv(1200, dpi, 96), Width(mi.rcWork)),
        height = std::min(MulDiv(1000, dpi, 96), Height(mi.rcWork));
    int x = mi.rcWork.left + (Width(mi.rcWork) - width) / 2,
        y = mi.rcWork.top + (Height(mi.rcWork) - height) / 2;
    if (state.window) {
        auto r = *state.window;
        auto savedMon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
        if (savedMon) {
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(savedMon, &mi);
            width = std::min(std::max(width, Width(r)), Width(mi.rcWork));
            height = std::min(std::max(height, Height(r)), Height(mi.rcWork));
            x = std::clamp(r.left, mi.rcWork.left, mi.rcWork.right - width);
            y = std::clamp(r.top, mi.rcWork.top, mi.rcWork.bottom - height);
        }
    }
    hwnd_ =
        CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, AppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                        x, y, width, height, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_)
        throw AppError("settings", "无法创建设置窗口");
    try {
        Build(key);
        UpdateIcons();
        Layout();
        if (visible) {
            ShowForUser(hwnd_, SW_SHOW);
        }
    } catch (...) {
        Close();
        throw;
    }
}
void SettingsWindow::UpdateIcons() {
    auto dpi = GetDpiForWindow(hwnd_);
    auto instance = GetModuleHandleW(nullptr);
    auto large = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                               GetSystemMetricsForDpi(SM_CXICON, dpi),
                                               GetSystemMetricsForDpi(SM_CYICON, dpi), LR_SHARED));
    auto smallIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                   GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                                   GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_SHARED));
    if (!large || !smallIcon)
        throw AppError("settings", "应用图标资源缺失。");
    std::wstring executable(32768, 0);
    auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size())
        throw AppError("settings", "无法定位程序路径。");
    executable.resize(length);
    auto iconPath = std::filesystem::path(executable).parent_path() / Wide(SAT_APP_ICON_NAME);
    if (!std::filesystem::is_regular_file(iconPath))
        throw AppError("settings", "任务栏图标文件缺失，请完整解压或重新安装程序。");
    ComPtr<IPropertyStore> properties;
    CheckHR(SHGetPropertyStoreForWindow(hwnd_, IID_PPV_ARGS(&properties)), "settings");
    auto property = [&](const PROPERTYKEY& key, const std::wstring& text) {
        PROPVARIANT value{};
        CheckHR(InitPropVariantFromString(text.c_str(), &value), "settings");
        auto result = properties->SetValue(key, value);
        PropVariantClear(&value);
        CheckHR(result, "settings");
    };
    property(PKEY_AppUserModel_RelaunchIconResource, iconPath.wstring() + L",0");
    property(PKEY_AppUserModel_RelaunchCommand, L"\"" + executable + L"\"");
    property(PKEY_AppUserModel_RelaunchDisplayNameResource, L"@" + executable + L",-301");
    property(PKEY_AppUserModel_ID, L"ScreenAITranslator.Desktop");
    auto appWindow = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(
        winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd_));
    appWindow.SetTaskbarIcon(iconPath.wstring());
    appWindow.SetTitleBarIcon(winrt::Microsoft::UI::GetIconIdFromIcon(smallIcon));
    if (ui_ && ui_->customCaption) {
        auto source = xaml::Media::Imaging::BitmapImage(
            winrt::Windows::Foundation::Uri(L"file:///" + iconPath.generic_wstring()));
        ui_->Get<xaml::Controls::Image>(L"CaptionIcon").Source(source);
        UpdateCaptionTheme();
    }
    // Set both per-window icon slots after XAML hosting has been initialized.
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
}
void SettingsWindow::UpdateCaptionTheme() {
    if (!ui_ || !ui_->customCaption)
        return;
    using UiColor = winrt::Windows::UI::Color;
    auto reference = [](UiColor color) {
        return winrt::box_value(color).as<winrt::Windows::Foundation::IReference<UiColor>>();
    };
    bool dark = ui_->root.ActualTheme() == xaml::ElementTheme::Dark;
    auto title = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(
                     winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd_))
                     .TitleBar();
    title.ButtonBackgroundColor(reference({0, 0, 0, 0}));
    title.ButtonInactiveBackgroundColor(reference({0, 0, 0, 0}));
    title.ButtonForegroundColor(reference(dark ? UiColor{255, 242, 242, 242} : UiColor{255, 27, 27, 27}));
    title.ButtonInactiveForegroundColor(
        reference(dark ? UiColor{255, 150, 150, 150} : UiColor{255, 110, 110, 110}));
    title.ButtonHoverBackgroundColor(
        reference(dark ? UiColor{255, 54, 54, 54} : UiColor{255, 234, 234, 234}));
}
void SettingsWindow::Close() {
    textPage_.Detach();
    SetHotkeyEditing(false);
    if (ui_) {
        ui_->focusGained.revoke();
        ui_->focusLost.revoke();
        ui_->themeChanged.revoke();
        ui_->updating = true;
        if (ui_->root)
            ui_->Get<PasswordBox>(L"Key").Password(L"");
        if (ui_->island) {
            ui_->island.Content(nullptr);
            ui_->island.Close();
        }
        ui_.reset();
    }
    if (hwnd_) {
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd_, IID_PPV_ARGS(&properties)))) {
            PROPVARIANT empty{};
            for (const auto& key : TaskbarProperties)
                properties->SetValue(key, empty);
        }
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
}
void SettingsWindow::Layout() {
    if (!ui_ || !ui_->island)
        return;
    RECT r{};
    GetClientRect(hwnd_, &r);
    auto dpi = GetDpiForWindow(hwnd_);
    const bool compact = MulDiv(Height(r), 96, dpi) < 740 || MulDiv(Width(r), 96, dpi) < 1000;
    ui_->Get<Grid>(L"ContentLayout")
        .Padding(compact ? xaml::Thickness{24, 22, 24, 20} : xaml::Thickness{36, 34, 36, 28});
    ui_->Get<Grid>(L"PageHeader").Margin({0, 0, 0, compact ? 16.0 : 33.0});
    ui_->Get<TextBlock>(L"PageTitle").FontSize(compact ? 28 : 34);
    ui_->Get<Grid>(L"TextPage").RowSpacing(compact ? 16 : 22);
    auto language = ui_->Get<Border>(L"LanguageCard");
    language.MinHeight(compact ? 64 : 80);
    language.Padding(compact ? xaml::Thickness{16, 10, 16, 10} : xaml::Thickness{20, 18, 20, 18});
    textPage_.AdaptLayout(compact);
    if (ui_->customCaption) {
        auto title = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(
                         winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd_))
                         .TitleBar();
        std::array<winrt::Windows::Graphics::RectInt32, 1> drag{
            {{0, 0, std::max(1, Width(r) - title.RightInset()), MulDiv(56, dpi, 96)}}};
        title.SetDragRectangles(drag);
    }
    ui_->island.SiteBridge().MoveAndResize({0, 0, Width(r), Height(r)});
    ui_->island.SiteBridge().Show();
}
void SettingsWindow::Build(const std::string& key) {
    ui_ = std::make_unique<Impl>();
    ui_->updating = true;
    ui_->profiles = settings_.connectionProfiles;
    ui_->activeProvider = settings_.provider;
    auto resource = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(201), RT_RCDATA);
    if (!resource)
        throw AppError("settings", "界面资源缺失");
    auto loaded = LoadResource(GetModuleHandleW(nullptr), resource);
    auto bytes = static_cast<const char*>(LockResource(loaded));
    auto size = SizeofResource(GetModuleHandleW(nullptr), resource);
    ui_->root = xaml::Markup::XamlReader::Load(Wide(std::string(bytes, size))).as<xaml::FrameworkElement>();
    if (winrt::Microsoft::UI::Windowing::AppWindowTitleBar::IsCustomizationSupported()) {
        auto title = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(
                         winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd_))
                         .TitleBar();
        title.ExtendsContentIntoTitleBar(true);
        title.PreferredHeightOption(winrt::Microsoft::UI::Windowing::TitleBarHeightOption::Tall);
        title.IconShowOptions(winrt::Microsoft::UI::Windowing::IconShowOptions::HideIconAndSystemMenu);
        ui_->customCaption = true;
        ui_->Get<Grid>(L"CaptionBar").Visibility(xaml::Visibility::Visible);
        ui_->themeChanged = ui_->root.ActualThemeChanged(
            winrt::auto_revoke, [this](auto const&, auto const&) { UpdateCaptionTheme(); });
    }
    ui_->island = xaml::Hosting::DesktopWindowXamlSource();
    ui_->island.Initialize(winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd_));
    ui_->island.Content(ui_->root);
    ui_->island.TakeFocusRequested([this](auto const&, auto const& args) {
        auto reason = args.Request().Reason();
        if (!ui_->focusPending && (reason == xaml::Hosting::XamlSourceFocusNavigationReason::First ||
                                   reason == xaml::Hosting::XamlSourceFocusNavigationReason::Last)) {
            ui_->focusPending = true;
            PostMessageW(hwnd_, WM_APP + 43, static_cast<WPARAM>(reason), 0);
        }
    });
    auto text = [&](const wchar_t* name, const std::wstring& value) { ui_->Get<TextBox>(name).Text(value); };
    auto toggle = [&](const wchar_t* name, bool value) { ui_->Get<ToggleSwitch>(name).IsOn(value); };
    auto number = [&](const wchar_t* name, double value) { ui_->Get<NumberBox>(name).Value(value); };
    toggle(L"Startup", settings_.startup);
    toggle(L"Tray", settings_.tray);
    toggle(L"TextCtrlEnter", settings_.textCtrlEnter);
    for (const auto& preset : ProviderPresets())
        ui_->Get<ComboBox>(L"Provider").Items().Append(winrt::box_value(preset.name));
    for (size_t i = 0; i < ProviderPresets().size(); ++i) {
        if (settings_.provider == ProviderPresets()[i].id)
            ui_->Get<ComboBox>(L"Provider").SelectedIndex(int(i));
    }
    text(L"BaseUrl", settings_.baseUrl);
    ui_->Get<PasswordBox>(L"Key").Password(Wide(key));
    text(L"ServiceRegion", Wide(settings_.serviceRegion));
    toggle(L"TranslateAll", settings_.translateAll);
    ui_->Get<ComboBox>(L"ProxyMode").SelectedIndex(settings_.proxyMode);
    text(L"ProxyAddress", settings_.proxyAddress);
    ui_->Get<ComboBox>(L"Model").Items().Append(winrt::box_value(Wide(settings_.model)));
    ui_->Get<ComboBox>(L"Model").SelectedIndex(0);
    number(L"Timeout", settings_.timeoutSeconds);
    text(L"RequestParameters", Wide(settings_.requestParameters));
    ui_->Get<ComboBox>(L"Quality").SelectedIndex(settings_.quality);
    ui_->Get<ComboBox>(L"TranslationMode").SelectedIndex(settings_.translationMode);
    ui_->Get<ComboBox>(L"OcrMode").SelectedIndex(OcrSelectionFromMode(settings_.ocrMode));
    toggle(L"ContextEnabled", settings_.contextEnabled);
    number(L"ContextSize", settings_.contextSize);
    text(L"Prompt", Wide(settings_.prompt));
    toggle(L"SpatialOverlay", settings_.spatialOverlay);
    auto fonts = ui_->Get<ComboBox>(L"Font");
    ComPtr<IDWriteFactory> fontFactory;
    ComPtr<IDWriteFontCollection> fontCollection;
    if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(fontFactory.GetAddressOf()))))
        fontFactory->GetSystemFontCollection(&fontCollection);
    int selectedFont = -1;
    for (auto name : {L"Microsoft YaHei UI", L"Microsoft YaHei", L"SimSun", L"SimHei", L"KaiTi", L"FangSong",
                      L"Segoe UI", L"Arial", L"Times New Roman", L"Consolas", L"Yu Gothic UI", L"Meiryo"}) {
        auto label = FontLabel(fontCollection.Get(), name);
        if (settings_.font == name || settings_.font == label)
            selectedFont = int(fonts.Items().Size());
        fonts.Items().Append(winrt::box_value(label));
        ui_->fontNames.emplace_back(label, name);
    }
    if (selectedFont < 0) {
        selectedFont = int(fonts.Items().Size());
        auto label = FontLabel(fontCollection.Get(), settings_.font);
        fonts.Items().Append(winrt::box_value(label));
        ui_->fontNames.emplace_back(label, settings_.font);
    }
    fonts.SelectedIndex(selectedFont);
    fonts.Text(ui_->fontNames[selectedFont].first);
    fonts.SelectionChanged([this](auto const& sender, auto const&) {
        auto control = sender.template as<ComboBox>();
        auto index = control.SelectedIndex();
        if (index >= 0)
            control.Text(ui_->fontNames.at(index).first);
    });
    toggle(L"MatchTextColor", settings_.matchTextColor);
    toggle(L"AutoFont", settings_.autoFont);
    number(L"FontSize", settings_.fontSize);
    text(L"TextColor", Color(settings_.textColor));
    text(L"OutlineColor", Color(settings_.outlineColor));
    number(L"OutlineWidth", settings_.outlineWidth);
    number(L"Opacity", settings_.backgroundOpacity);
    for (int i = 0; i < 3; ++i) {
        ui_->hotkeys[i] = settings_.hotkeys[i];
        auto name = L"Hotkey" + std::to_wstring(i);
        auto control = ui_->Get<TextBox>(name.c_str());
        control.Text(HotkeyLabel(ui_->hotkeys[i]));
        control.PreviewKeyDown([this, i](auto const& sender, xaml::Input::KeyRoutedEventArgs const& e) {
            auto key = static_cast<UINT>(e.Key());
            if (key == VK_TAB)
                return;
            e.Handled(true);
            if (key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT || key == VK_LWIN || key == VK_RWIN)
                return;
            Hotkey next{};
            if (key != VK_BACK && key != VK_DELETE) {
                next.key = key;
                if (GetKeyState(VK_CONTROL) < 0)
                    next.modifiers |= MOD_CONTROL;
                if (GetKeyState(VK_MENU) < 0)
                    next.modifiers |= MOD_ALT;
                if (GetKeyState(VK_SHIFT) < 0)
                    next.modifiers |= MOD_SHIFT;
                if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0)
                    next.modifiers |= MOD_WIN;
            }
            ui_->hotkeys[i] = next;
            sender.template as<TextBox>().Text(HotkeyLabel(next));
        });
    }
    auto nav = ui_->Get<NavigationView>(L"Navigation");
    auto focusChanged = [this](auto const&, auto const&) {
        if (ui_ && !ui_->hotkeyFocusPending) {
            // FocusManager notifications precede the committed focus state.
            ui_->hotkeyFocusPending = true;
            PostMessageW(hwnd_, WM_APP + 44, 0, 0);
        }
    };
    ui_->focusGained = xaml::Input::FocusManager::GotFocus(winrt::auto_revoke, focusChanged);
    ui_->focusLost = xaml::Input::FocusManager::LostFocus(winrt::auto_revoke, focusChanged);
    auto leaveEditor = xaml::Input::PointerEventHandler(
        [this](auto const&, xaml::Input::PointerRoutedEventArgs const& args) {
            if (hotkeyEditing_ &&
                !ui_->IsHotkeyEditor(args.OriginalSource().try_as<xaml::DependencyObject>())) {
                ui_->Get<Button>(L"Close").Focus(xaml::FocusState::Programmatic);
                SetHotkeyEditing(false);
            }
        });
    ui_->root.AddHandler(xaml::UIElement::PointerPressedEvent(), winrt::box_value(leaveEditor), true);
    nav.SelectionChanged([this](auto const&, NavigationViewSelectionChangedEventArgs const& args) {
        if (auto item = args.SelectedItem().try_as<NavigationViewItem>())
            SelectPage(std::stoi(std::wstring(winrt::unbox_value<winrt::hstring>(item.Tag()))));
    });
    ui_->Get<SelectorBar>(L"SettingsTabs").SelectionChanged([this](auto const& sender, auto const&) {
        auto item = sender.template as<SelectorBar>().SelectedItem();
        if (item)
            SelectSettingsTab(std::stoi(std::wstring(winrt::unbox_value<winrt::hstring>(item.Tag()))));
    });
    nav.SelectedItem(page_ < 2 ? nav.MenuItems().GetAt(page_) : nav.FooterMenuItems().GetAt(0));
    SelectPage(page_);
    SelectSettingsTab(settingsTab_);
    for (auto [name, tab] : std::initializer_list<std::pair<const wchar_t*, int>>{{L"TextConnection", 1},
                                                                                  {L"ScreenConnection", 1},
                                                                                  {L"OpenAppearance", 2},
                                                                                  {L"OpenShortcuts", 0}}) {
        ui_->Get<HyperlinkButton>(name).Click([this, tab](auto const&, auto const&) {
            auto navigation = ui_->Get<NavigationView>(L"Navigation");
            navigation.SelectedItem(navigation.FooterMenuItems().GetAt(0));
            SelectSettingsTab(tab);
        });
    }
    auto button = [&](const wchar_t* name, std::function<void()> action) {
        auto control = ui_->Get<Button>(name);
        xaml::Automation::AutomationProperties::SetAutomationId(control, name);
        control.Click([this, action](auto const&, auto const&) {
            try {
                action();
            } catch (const AppError& e) {
                Status(Wide(e.what()));
            } catch (const winrt::hresult_error&) {
                Status(L"界面操作未完成。");
            } catch (...) {
                Status(L"操作未完成，请检查设置。");
            }
        });
    };
    button(L"Apply", [this] { Save(); });
    button(L"Close", [this] { PostMessageW(hwnd_, WM_CLOSE, 0, 0); });
    button(L"FetchModels", [this] { StartApi(true); });
    button(L"TestApi", [this] { StartApi(false); });
    button(L"FormatRequestParameters", [this] {
        auto editor = ui_->Get<TextBox>(L"RequestParameters");
        auto formatted = ParseRequestParameters(Utf8(std::wstring(editor.Text()))).dump(2);
        editor.Text(Wide(formatted));
        Status(L"JSON 格式有效；字段支持范围以所选服务的官方定义为准。");
    });
    for (auto [name, action] : std::initializer_list<std::pair<const wchar_t*, SettingsAction>>{
             {L"Translate", SettingsAction::Translate},
             {L"TranslateFixed", SettingsAction::FixedTranslate},
             {L"ClearContext", SettingsAction::ClearContext}}) {
        button(name, [this, action] {
            if (callbacks_.action)
                callbacks_.action(action);
            if (action == SettingsAction::ClearContext)
                textPage_.ClearContext();
            if (action == SettingsAction::ClearContext)
                Status(L"上下文已清空。");
        });
    }
    ui_->Get<TextBox>(L"BaseUrl")
        .RegisterPropertyChangedCallback(TextBox::TextProperty(),
                                         [this](auto const&, auto const&) { InvalidateModels(); });
    ui_->Get<PasswordBox>(L"Key").RegisterPropertyChangedCallback(
        PasswordBox::PasswordProperty(), [this](auto const&, auto const&) { InvalidateModels(); });
    ui_->Get<ComboBox>(L"Provider")
        .RegisterPropertyChangedCallback(
            xaml::Controls::Primitives::Selector::SelectedIndexProperty(), [this](auto const&, auto const&) {
                if (ui_->updating)
                    return;
                auto index = ui_->Get<ComboBox>(L"Provider").SelectedIndex();
                if (index < 0)
                    return;
                const auto& preset = ProviderPresets().at(index);
                if (ui_->activeProvider == preset.id)
                    return;
                auto previous = ui_->ReadConnection();
                auto& profiles = ui_->profiles;
                auto found = profiles.find(preset.id);
                ConnectionProfile next = found == profiles.end()
                                             ? ConnectionProfile{preset.url, preset.model, {}, {}}
                                             : found->second;
                Secret secret{UnprotectSecret(next.encryptedKey)};
                profiles[ui_->activeProvider] = std::move(previous);
                ui_->updating = true;
                struct Reset {
                    bool& value;
                    ~Reset() {
                        value = false;
                    }
                } reset{ui_->updating};
                ui_->Get<PasswordBox>(L"Key").Password(Wide(secret.value));
                ui_->Get<TextBox>(L"BaseUrl").Text(next.baseUrl);
                ui_->Get<TextBox>(L"RequestParameters").Text(Wide(next.requestParameters));
                {
                    auto model = ui_->Get<ComboBox>(L"Model");
                    model.SelectedIndex(-1);
                    model.Items().Clear();
                    model.Text(Wide(next.model));
                    ui_->Get<TextBox>(L"ServiceRegion").Text(Wide(next.serviceRegion));
                }
                ui_->activeProvider = preset.id;
                UpdateTranslationMode();
                ui_->Get<TextBlock>(L"ApiResult")
                    .Text(L"已切换连接草稿。切回可恢复原配置；点击保存设置后写入磁盘。");
            });
    auto dependent = [this] {
        ui_->Get<NumberBox>(L"ContextSize").IsEnabled(ui_->On(L"ContextEnabled"));
        ui_->Get<NumberBox>(L"FontSize").IsEnabled(!ui_->On(L"AutoFont"));
        ui_->Get<TextBox>(L"TextColor").IsEnabled(!ui_->On(L"MatchTextColor"));
        ui_->Get<TextBox>(L"ProxyAddress")
            .IsEnabled(!apiBusy_ && ui_->Get<ComboBox>(L"ProxyMode").SelectedIndex() == 2);
        auto preview = ui_->Get<TextBlock>(L"FontPreview");
        preview.FontFamily(xaml::Media::FontFamily(ui_->ReadFont()));
        auto size = ui_->Get<NumberBox>(L"FontSize").Value();
        preview.FontSize(ui_->On(L"AutoFont") || !std::isfinite(size) ? 22 : std::clamp(size, 8.0, 144.0));
    };
    for (auto name : {L"ContextEnabled", L"AutoFont", L"MatchTextColor"})
        ui_->Get<ToggleSwitch>(name).Toggled([dependent](auto const&, auto const&) { dependent(); });
    dependent();
    fonts.RegisterPropertyChangedCallback(ComboBox::TextProperty(),
                                          [dependent](auto const&, auto const&) { dependent(); });
    ui_->Get<NumberBox>(L"FontSize").ValueChanged([dependent](auto const&, auto const&) { dependent(); });
    ui_->Get<ComboBox>(L"ProxyMode").SelectionChanged([dependent](auto const&, auto const&) { dependent(); });
    ui_->Get<ComboBox>(L"TranslationMode").SelectionChanged([this](auto const&, auto const&) {
        UpdateTranslationMode();
    });
    for (auto [name, mode] : std::initializer_list<std::pair<const wchar_t*, int>>{
             {L"VisionChoice", TranslationAiVision}, {L"OcrChoice", TranslationOcrText}}) {
        ui_->Get<xaml::Controls::Primitives::ToggleButton>(name).Click(
            [this, mode](auto const&, auto const&) {
                ui_->Get<ComboBox>(L"TranslationMode").SelectedIndex(mode);
                UpdateTranslationMode();
            });
    }
    UpdateTranslationMode();
    ui_->updating = false;
    for (auto [name, mode] : std::initializer_list<std::pair<const wchar_t*, int>>{
             {L"VisionChoice", TranslationAiVision}, {L"OcrChoice", TranslationOcrText}}) {
        ui_->Get<xaml::Controls::Primitives::ToggleButton>(name).Checked(
            [this, mode](auto const&, auto const&) {
                ui_->Get<ComboBox>(L"TranslationMode").SelectedIndex(mode);
            });
    }
    textPage_.Attach(ui_->root, hwnd_, settings_, key, [this](const std::string& language) {
        try {
            if (callbacks_.targetLanguage)
                callbacks_.targetLanguage(language);
            settings_.textTargetLanguage = language;
        } catch (...) {
            Status(L"目标语言未保存，当前选择在本次会话中有效。");
        }
    });
}
void SettingsWindow::SelectPage(int page) {
    page_ = std::clamp(page, 0, 2);
    ui_->Get<Grid>(L"TextPage")
        .Visibility(page_ == 0 ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    ui_->Get<ScrollViewer>(L"PageScroll")
        .Visibility(page_ > 0 ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    ui_->Get<StackPanel>(L"ScreenPage")
        .Visibility(page_ == 1 ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    ui_->Get<StackPanel>(L"SettingsPage")
        .Visibility(page_ == 2 ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    ui_->Get<Grid>(L"SettingsFooter")
        .Visibility(page_ > 0 ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    ui_->Get<SelectorBar>(L"SettingsTabs")
        .Visibility(page_ == 2 ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    ui_->Get<HyperlinkButton>(L"TextConnection")
        .Visibility(page_ == 0 ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    ui_->Get<Button>(L"ConversationMenu")
        .Visibility(page_ == 0 ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    ui_->Get<TextBlock>(L"ScreenSubtitle")
        .Visibility(page_ == 1 ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    const wchar_t* titles[]{L"文本翻译", L"截图翻译", L"设置"};
    ui_->Get<TextBlock>(L"PageTitle").Text(titles[page_]);
    SetHotkeyEditing(GetActiveWindow() == hwnd_ && ui_->FocusedHotkey());
    ui_->Get<ScrollViewer>(L"PageScroll")
        .ChangeView(nullptr, winrt::box_value(0.0).as<winrt::Windows::Foundation::IReference<double>>(),
                    nullptr);
}
void SettingsWindow::SelectSettingsTab(int tab) {
    const bool changed = settingsTab_ != tab;
    settingsTab_ = std::clamp(tab, 0, 3);
    const wchar_t* names[]{L"SettingsGeneral", L"SettingsConnection", L"SettingsAppearance",
                           L"SettingsAbout"};
    for (int i = 0; i < 4; ++i)
        ui_->Get<StackPanel>(names[i]).Visibility(i == settingsTab_ ? xaml::Visibility::Visible
                                                                    : xaml::Visibility::Collapsed);
    auto tabs = ui_->Get<SelectorBar>(L"SettingsTabs");
    if (tabs.SelectedItem() != tabs.Items().GetAt(settingsTab_))
        tabs.SelectedItem(tabs.Items().GetAt(settingsTab_));
    SetHotkeyEditing(GetActiveWindow() == hwnd_ && ui_->FocusedHotkey());
    if (changed)
        ui_->Get<ScrollViewer>(L"PageScroll")
            .ChangeView(nullptr, winrt::box_value(0.0).as<winrt::Windows::Foundation::IReference<double>>(),
                        nullptr);
}
void SettingsWindow::SetHotkeyEditing(bool editing) {
    if (hotkeyEditing_ == editing)
        return;
    hotkeyEditing_ = editing;
    if (callbacks_.hotkeyEditing)
        callbacks_.hotkeyEditing(editing);
}
void SettingsWindow::UpdateTranslationMode() {
    if (!ui_)
        return;
    const bool textOnly = ui_->Get<ComboBox>(L"TranslationMode").SelectedIndex() == TranslationOcrText;
    for (auto [name, selected] : std::initializer_list<std::pair<const wchar_t*, bool>>{
             {L"VisionChoice", !textOnly}, {L"OcrChoice", textOnly}}) {
        auto choice = ui_->Get<xaml::Controls::Primitives::ToggleButton>(name);
        choice.IsChecked(winrt::box_value(selected).as<winrt::Windows::Foundation::IReference<bool>>());
        choice.IsEnabled(!apiBusy_);
    }
    ui_->Get<ComboBox>(L"TranslationMode").IsEnabled(!apiBusy_);
    ui_->Get<ComboBox>(L"OcrMode").IsEnabled(!apiBusy_ && textOnly);
    ui_->Get<ComboBox>(L"Quality").IsEnabled(!apiBusy_ && !textOnly);
    ui_->Get<ComboBox>(L"OcrMode")
        .Visibility(textOnly ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    ui_->Get<ComboBox>(L"Quality")
        .Visibility(!textOnly ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    auto index = ui_->Get<ComboBox>(L"Provider").SelectedIndex();
    if (index >= 0) {
        auto id = ProviderPresets().at(index).id;
        const bool dedicated = std::string_view(id) == "deepl" || std::string_view(id) == "azure-translator";
        ui_->Get<TextBox>(L"ServiceRegion")
            .Visibility(std::string_view(id) == "azure-translator" ? xaml::Visibility::Visible
                                                                   : xaml::Visibility::Collapsed);
        ui_->Get<ComboBox>(L"Model").IsEnabled(!apiBusy_ && !dedicated);
        ui_->Get<Button>(L"FetchModels").IsEnabled(!apiBusy_ && !dedicated);
    }
    ui_->Get<TextBox>(L"ProxyAddress")
        .IsEnabled(!apiBusy_ && ui_->Get<ComboBox>(L"ProxyMode").SelectedIndex() == 2);
}
void SettingsWindow::Status(const std::wstring& text) {
    if (page_ == 0)
        textPage_.Notify(text);
    if (ui_ && ui_->root)
        ui_->Get<TextBlock>(L"Status").Text(text);
}
void SettingsWindow::SetTranslationMode(int mode) {
    settings_.translationMode = mode;
    if (ui_ && ui_->root) {
        ui_->Get<ComboBox>(L"TranslationMode").SelectedIndex(mode);
        UpdateTranslationMode();
    }
}
void SettingsWindow::ApiBusy(bool busy) {
    apiBusy_ = busy;
    if (!ui_)
        return;
    for (auto name :
         {L"Provider", L"BaseUrl", L"Key", L"Model", L"Timeout", L"RequestParameters", L"FetchModels",
          L"TestApi", L"Apply", L"ServiceRegion", L"ProxyMode", L"ProxyAddress", L"FormatRequestParameters"})
        ui_->Get<Control>(name).IsEnabled(!busy);
    auto progress = ui_->Get<ProgressRing>(L"ApiProgress");
    progress.IsActive(busy);
    progress.Visibility(busy ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    UpdateTranslationMode();
}
void SettingsWindow::InvalidateModels() {
    if (!ui_ || ui_->updating || apiBusy_)
        return;
    auto model = ui_->Get<ComboBox>(L"Model");
    model.SelectedItem(nullptr);
    model.SelectedIndex(-1);
    model.Items().Clear();
    model.Text(L"");
    ui_->Get<TextBlock>(L"ApiResult").Text(L"连接配置已改变，请输入模型名称或重新获取列表。");
}
Settings SettingsWindow::ReadApi(bool requireModel) const {
    auto value = settings_;
    value.provider = ProviderPresets().at(ui_->Get<ComboBox>(L"Provider").SelectedIndex()).id;
    value.baseUrl = ui_->Text(L"BaseUrl");
    value.timeoutSeconds = ui_->Integer(L"Timeout");
    value.requestParameters = Utf8(ui_->Text(L"RequestParameters"));
    (void)ParseRequestParameters(value.requestParameters);
    value.ocrMode = OcrModeFromSelection(ui_->Get<ComboBox>(L"OcrMode").SelectedIndex());
    value.translationMode = ui_->Get<ComboBox>(L"TranslationMode").SelectedIndex();
    value.serviceRegion = Utf8(ui_->Text(L"ServiceRegion"));
    value.proxyMode = ui_->Get<ComboBox>(L"ProxyMode").SelectedIndex();
    value.proxyAddress = ui_->Text(L"ProxyAddress");
    value.translateAll = ui_->On(L"TranslateAll");
    auto model = ui_->Get<ComboBox>(L"Model");
    auto name = std::wstring(model.Text());
    if (name.empty() && model.SelectedItem())
        name = std::wstring(winrt::unbox_value<winrt::hstring>(model.SelectedItem()));
    if (value.provider == "deepl" || value.provider == "azure-translator")
        name = L"translate";
    if (name.empty() && requireModel)
        throw AppError("settings", "请选择或输入模型名称。");
    value.model = name.empty() ? "model-list" : Utf8(name);
    value.connectionProfiles = ui_->profiles;
    value.connectionProfiles[value.provider] = ui_->ReadConnection();
    value.connectionProfiles[value.provider].model = value.model;
    value.encryptedKey = value.connectionProfiles[value.provider].encryptedKey;
    return value;
}
void SettingsWindow::StartApi(bool fetch) {
    if (apiBusy_ || !callbacks_.api)
        return;
    auto value = ReadApi(!fetch);
    Secret secret{Utf8(std::wstring(ui_->Get<PasswordBox>(L"Key").Password()))};
    if (secret.value.empty())
        throw AppError("settings", "请先填写 API 密钥。");
    ApiBusy(true);
    ui_->Get<TextBlock>(L"ApiResult").Text(fetch ? L"正在获取模型列表…" : L"正在进行连接和翻译自检…");
    try {
        callbacks_.api(fetch, value, secret.value);
    } catch (...) {
        ApiFinished({}, L"请求未完成，请检查连接配置。", false);
        throw;
    }
}
void SettingsWindow::ApiFinished(const std::vector<std::string>& models, const std::wstring& message,
                                 bool fetched) {
    if (!ui_)
        return;
    ApiBusy(false);
    if (fetched) {
        auto model = ui_->Get<ComboBox>(L"Model");
        auto previous = model.Text();
        model.Items().Clear();
        int selected = -1;
        for (size_t i = 0; i < models.size(); ++i) {
            auto name = Wide(models[i]);
            model.Items().Append(winrt::box_value(name));
            if (name == previous)
                selected = static_cast<int>(i);
        }
        if (selected >= 0)
            model.SelectedIndex(selected);
        else if (models.size() == 1)
            model.SelectedIndex(0);
        else
            model.Text(previous);
    }
    ui_->Get<TextBlock>(L"ApiResult").Text(message);
}
void SettingsWindow::Save() {
    if (apiBusy_)
        return;
    try {
        auto next = ReadApi(true);
        next.startup = ui_->On(L"Startup");
        next.tray = ui_->On(L"Tray");
        next.textCtrlEnter = ui_->On(L"TextCtrlEnter");
        std::copy(std::begin(ui_->hotkeys), std::end(ui_->hotkeys), std::begin(next.hotkeys));
        next.quality = ui_->Get<ComboBox>(L"Quality").SelectedIndex();
        next.ocrMode = OcrModeFromSelection(ui_->Get<ComboBox>(L"OcrMode").SelectedIndex());
        next.contextEnabled = ui_->On(L"ContextEnabled");
        next.contextSize = ui_->Integer(L"ContextSize");
        next.prompt = Utf8(ui_->Text(L"Prompt"));
        next.spatialOverlay = ui_->On(L"SpatialOverlay");
        next.font = ui_->ReadFont();
        next.matchTextColor = ui_->On(L"MatchTextColor");
        next.autoFont = ui_->On(L"AutoFont");
        next.fontSize = static_cast<float>(ui_->Number(L"FontSize"));
        next.textColor = ParseColor(ui_->Text(L"TextColor"));
        next.outlineColor = ParseColor(ui_->Text(L"OutlineColor"));
        next.outlineWidth = static_cast<float>(ui_->Number(L"OutlineWidth"));
        next.backgroundOpacity = static_cast<float>(ui_->Number(L"Opacity"));
        ValidateSettings(next);
        Secret secret{Utf8(std::wstring(ui_->Get<PasswordBox>(L"Key").Password()))};
        auto error = callbacks_.apply ? callbacks_.apply(next, secret.value) : L"保存服务不可用。";
        if (!error.empty()) {
            Status(error);
            return;
        }
        settings_ = next;
        textPage_.UpdateConnection(settings_, secret.value);
        Status(L"设置已保存。关闭窗口后程序继续在后台运行。");
    } catch (const AppError& e) {
        Status(Wide(e.what()));
    } catch (...) {
        Status(L"请检查数值范围和六位十六进制颜色。");
    }
}
LRESULT CALLBACK SettingsWindow::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(h, m, w, l);
    try {
        switch (m) {
        case WM_SIZE:
            self->Layout();
            return 0;
        case WM_ACTIVATE:
            self->SetHotkeyEditing(LOWORD(w) != WA_INACTIVE && self->ui_ && self->ui_->FocusedHotkey());
            break;
        case WM_SHOWWINDOW:
            if (!w)
                self->SetHotkeyEditing(false);
            break;
        case WM_APP + 43:
            if (self->ui_) {
                auto& ui = *self->ui_;
                if (ui.root.IsLoaded())
                    ui.island.NavigateFocus(xaml::Hosting::XamlSourceFocusNavigationRequest(
                        static_cast<xaml::Hosting::XamlSourceFocusNavigationReason>(w)));
                ui.focusPending = false;
            }
            return 0;
        case WM_APP + 44:
            if (self->ui_) {
                self->ui_->hotkeyFocusPending = false;
                self->SetHotkeyEditing(GetActiveWindow() == h && self->ui_->FocusedHotkey());
            }
            return 0;
        case TextTranslationPage::DispatchMessage:
            self->textPage_.Dispatch();
            return 0;
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(l);
            auto dpi = GetDpiForWindow(h);
            info->ptMinTrackSize = {MulDiv(860, dpi, 96), MulDiv(580, dpi, 96)};
            return 0;
        }
        case WM_SETFOCUS:
            if (self->ui_ && self->ui_->root.IsLoaded())
                self->ui_->island.NavigateFocus(xaml::Hosting::XamlSourceFocusNavigationRequest(
                    xaml::Hosting::XamlSourceFocusNavigationReason::Restore));
            return 0;
        case WM_DPICHANGED: {
            auto r = reinterpret_cast<RECT*>(l);
            SetWindowPos(h, nullptr, r->left, r->top, Width(*r), Height(*r), SWP_NOZORDER | SWP_NOACTIVATE);
            self->UpdateIcons();
            self->Layout();
            return 0;
        }
        case WM_CLOSE: {
            RECT r{};
            GetWindowRect(h, &r);
            if (self->callbacks_.closed)
                self->callbacks_.closed(r, self->page_, self->settingsTab_);
            self->Close();
            return 0;
        }
        case WM_NCDESTROY:
            self->hwnd_ = nullptr;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
        }
    } catch (...) {
        self->Status(L"窗口操作未完成。");
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace sat
