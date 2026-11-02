// Settings windows remain hidden or outside all monitors. No desktop capture or live API requests.
#include "../src/app/App.cpp"
#include "capture/BundledOcr.h"
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <wincodec.h>
#include <psapi.h>
#include <fstream>
#include <iostream>
#include <future>
using Json = nlohmann::json;
namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
Json Memory() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    Check(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                               sizeof(counters)),
          "Memory query failed");
    return {{"private", counters.PrivateUsage}, {"working_set", counters.WorkingSetSize}};
}
void Write(const std::filesystem::path& path, const Json& value) {
    std::ofstream stream(path);
    stream << value.dump(2);
    Check(bool(stream), "Report write failed");
}
Json Read(const std::filesystem::path& path) {
    std::ifstream stream(path);
    return Json::parse(stream);
}
class OffscreenSettings {
    HHOOK hook_{};
    static bool IsolatedDesktop() {
        wchar_t name[256]{};
        DWORD size{};
        return GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, name, sizeof(name),
                                         &size) &&
               std::wstring_view(name).starts_with(L"SAT.Tests.");
    }
    static LRESULT CALLBACK Hook(int code, WPARAM w, LPARAM l) {
        wchar_t name[64]{};
        if (code >= 0)
            GetClassNameW(reinterpret_cast<HWND>(w), name, 64);
        if (std::wstring_view(name) == L"SAT.Settings") {
            if (code == HCBT_CREATEWND) {
                auto create = reinterpret_cast<CBT_CREATEWNDW*>(l);
                create->lpcs->x = -32000;
                create->lpcs->y = -32000;
                if (!IsolatedDesktop())
                    create->lpcs->dwExStyle |= WS_EX_NOACTIVATE;
            } else if (code == HCBT_ACTIVATE && !IsolatedDesktop())
                return 1;
        }
        return CallNextHookEx(nullptr, code, w, l);
    }

public:
    OffscreenSettings() {
        hook_ = SetWindowsHookExW(WH_CBT, Hook, nullptr, GetCurrentThreadId());
        Check(hook_ != nullptr, "Cannot keep settings wake test outside the desktop");
    }
    ~OffscreenSettings() {
        UnhookWindowsHookEx(hook_);
    }
};
sat::Image ReadUserScreenshot() {
    using namespace sat;
    ComPtr<IWICImagingFactory> f;
    CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)),
            "test");
    ComPtr<IWICBitmapDecoder> decoder;
    CheckHR(f->CreateDecoderFromFilename(L"test-fixtures/user/bbc-headlines-en.png", nullptr, GENERIC_READ,
                                         WICDecodeMetadataCacheOnLoad, &decoder),
            "test");
    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHR(decoder->GetFrame(0, &frame), "test");
    ComPtr<IWICFormatConverter> converter;
    CheckHR(f->CreateFormatConverter(&converter), "test");
    CheckHR(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                  0, WICBitmapPaletteTypeCustom),
            "test");
    UINT w{}, h{};
    CheckHR(converter->GetSize(&w, &h), "test");
    sat::Image result{int(w), int(h), std::vector<unsigned char>(size_t(w) * h * 4)};
    CheckHR(converter->CopyPixels(nullptr, w * 4, UINT(result.bgra.size()), result.bgra.data()), "test");
    return result;
}
} // namespace
namespace sat {
class AppIdleTest {
public:
    static void CheckFirstOpen(App& app, int phase) {
        STARTUPINFOW startup{sizeof(startup)};
        GetStartupInfoW(&startup);
        Check((startup.dwFlags & STARTF_USESHOWWINDOW) && startup.wShowWindow == SW_HIDE,
              "Settings wake regression requires a process started with SW_HIDE");
        OffscreenSettings offscreen;
        if (phase == 0) {
            COPYDATASTRUCT request{};
            SendMessageW(app.window_, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&request));
        } else if (phase == 1)
            SendMessageW(app.window_, TrayMessage, 0, WM_LBUTTONDBLCLK);
        else
            app.Action(OpenSettings);
        const auto settings = app.settingsWindow_.Window();
        Check(settings && IsWindowVisible(settings),
              "First settings request after idle replacement left the window hidden");
        RECT bounds{};
        GetWindowRect(settings, &bounds);
        for (const auto& monitor : EnumerateMonitors()) {
            RECT overlap{};
            Check(!IntersectRect(&overlap, &bounds, &monitor.rect),
                  "Settings wake test entered a user monitor");
        }
        auto pump = [] {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (!WinUIRuntime::ProcessMessage(message)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
        };
        namespace xaml = winrt::Microsoft::UI::Xaml;
        using namespace xaml::Controls;
        auto root = app.settingsWindow_.Content();
        auto navigation = root.FindName(L"Navigation").as<NavigationView>();
        for (int frame = 0; frame < 30; ++frame) {
            root.UpdateLayout();
            pump();
            if (root.IsLoaded() && navigation.ActualWidth() > 0)
                break;
            Sleep(10);
        }
        navigation.SelectedItem(navigation.MenuItems().GetAt(1));
        root.UpdateLayout();
        pump();
        root.FindName(L"Close").as<Button>().Focus(xaml::FocusState::Programmatic);
        pump();
        auto checkRegistration = [&](bool registered) {
            for (const auto& key : app.settings_.hotkeys) {
                Check(key.key != 0, "Focus regression requires all shortcuts configured");
                bool available = RegisterHotKey(nullptr, 901, key.modifiers | MOD_NOREPEAT, key.key) != FALSE;
                if (available)
                    UnregisterHotKey(nullptr, 901);
                Check(available != registered, "Shortcut registration does not match editing focus");
            }
        };
        checkRegistration(true);
        for (int i = 0; i < 3; ++i) {
            auto editor = root.FindName(L"Hotkey" + std::to_wstring(i)).as<TextBox>();
            Check(editor.Focus(xaml::FocusState::Programmatic),
                  ("Cannot focus shortcut editor offscreen: loaded=" + std::to_string(root.IsLoaded()) +
                   ", visible=" + std::to_string(int(editor.Visibility())) +
                   ", width=" + std::to_string(editor.ActualWidth()))
                      .c_str());
            pump();
            for (int frame = 0; frame < 30 && !app.hotkeys_->Suspended(); ++frame) {
                root.UpdateLayout();
                pump();
                Sleep(10);
            }
            auto focused = xaml::Input::FocusManager::GetFocusedElement(root.XamlRoot())
                               .try_as<xaml::FrameworkElement>();
            Check(app.hotkeys_->Suspended(), ("Shortcut editor did not suspend global shortcuts: state=" +
                                              std::to_string(int(editor.FocusState())) + ", focused=" +
                                              (focused ? Utf8(std::wstring(focused.Name())) : "none") +
                                              ", active=" + std::to_string(GetActiveWindow() == settings))
                                                 .c_str());
            checkRegistration(false);
        }
        Check(root.FindName(L"Close").as<Button>().Focus(xaml::FocusState::Programmatic),
              "Cannot move focus out of shortcut editor");
        pump();
        for (int frame = 0; frame < 30 && app.hotkeys_->Suspended(); ++frame) {
            pump();
            Sleep(10);
        }
        Check(!app.hotkeys_->Suspended(), "Leaving shortcut editors did not resume shortcuts");
        checkRegistration(true);
        root.FindName(L"Hotkey0").as<TextBox>().Focus(xaml::FocusState::Programmatic);
        pump();
        navigation.SelectedItem(navigation.MenuItems().GetAt(0));
        pump();
        Check(!app.hotkeys_->Suspended(), "Page navigation left shortcuts suspended");
        checkRegistration(true);
        navigation.SelectedItem(navigation.MenuItems().GetAt(1));
        pump();
        root.FindName(L"Hotkey0").as<TextBox>().Focus(xaml::FocusState::Programmatic);
        pump();
        SendMessageW(settings, WM_CLOSE, 0, 0);
        Check(!app.settingsWindow_.Window(), "Settings wake test did not close");
        checkRegistration(true);
        app.hotkeys_->Suspend();
        SendMessageW(app.window_, WM_HOTKEY, 3, 0);
        Check(!app.settingsWindow_.Window(), "Queued shortcut executed while shortcuts were suspended");
        Check(app.hotkeys_->Resume().empty(), "Cannot restore shortcuts after queued message check");
    }
    static int Run(int phase, const std::filesystem::path& root, HANDLE instance, std::string resume) {
        WinUIRuntime runtime(true);
        Json report = {{"phase", phase}, {"initial", Memory()}};
        std::exception_ptr failure;
        int handoffs{};
        App app(
            true, root, CreateProvider,
            [&](const std::string& payload) {
                ++handoffs;
                report["before_handoff"] = Memory();
                Write(root / (L"phase-" + std::to_wstring(phase) + L".json"), report);
                if (phase == 0)
                    Write(root / L"snapshot.json", Json::parse(payload));
                return StartIdleReplacement(Executable(), instance, payload,
                                            L"--phase " + std::to_wstring(phase + 1) + L" --root \"" +
                                                root.wstring() + L"\"");
            },
            std::move(resume));
        app.Post([&] {
            try {
                auto step = [&](const char* name) {
                    Write(root / L"progress.json", {{"phase", phase}, {"step", name}});
                };
                step("controller-started");
                report["background"] = Memory();
                Check(!WinUIRuntime::Initialized(), "Background process initialized XAML");
                Check(GetModuleHandleW(L"onnxruntime.dll") == nullptr,
                      "Background process retained OCR runtime");
                // The production registration must still own the configured shortcut.
                auto hotkey = app.settings_.hotkeys[2];
                auto probe = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                             GetModuleHandleW(nullptr), nullptr);
                bool stolen =
                    RegisterHotKey(probe, 101, hotkey.modifiers | MOD_NOREPEAT, hotkey.key) != FALSE;
                if (stolen)
                    UnregisterHotKey(probe, 101);
                DestroyWindow(probe);
                Check(!stolen, "Wake hotkey was not registered");
                if (phase) {
                    auto expected = Read(root / L"snapshot.json");
                    Check(Json::parse(app.IdleSnapshot()) == expected,
                          "Handoff lost region/context or extended context expiry");
                    Check(app.key_ == "idle-test-key" && app.settings_.ocrMode == OcrKorean,
                          "Settings changed during idle handoff");
                    Check(report["background"]["private"].get<size_t>() < 24 * 1024 * 1024,
                          "Clean background private memory exceeds 24 MiB");
                    Check(report["background"]["working_set"].get<size_t>() < 40 * 1024 * 1024,
                          "Clean background working set exceeds 40 MiB");
                    step("first-open-after-idle");
                    CheckFirstOpen(app, phase);
                    report["first_open"] = phase == 1 ? "tray_double_click" : "settings_menu";
                } else {
                    step("first-open-after-background-start");
                    CheckFirstOpen(app, phase);
                    report["first_open"] = "relaunch";
                    app.sessionRegion_ = FixedRegion{L"test-monitor", .1, .2, .3, .4, 1920, 1080};
                    app.context_.Restore({{"user screenshot text", "用户截图译文"}},
                                         GetTickCount64() + 20 * 60 * 1000);
                    auto previous = Json::parse(app.IdleSnapshot());
                    Check(!app.CommitSelection({L"test-monitor", {0, 0, 1920, 1080}, 96}, {}, true) &&
                              Json::parse(app.IdleSnapshot()) == previous,
                          "cancelled reselection cleared prior region or context");
                    Check(app.CommitSelection({L"test-monitor", {0, 0, 1920, 1080}, 96},
                                              RECT{100, 200, 400, 500}, true) &&
                              app.context_.Get(true, 8).empty() && app.sessionRegion_->x == 100. / 1920.,
                          "committed reselection did not replace region and clear context");
                    app.sessionRegion_ = FixedRegion{L"test-monitor", .1, .2, .3, .4, 1920, 1080};
                    app.context_.Restore({{"user screenshot text", "用户截图译文"}},
                                         previous.at("expires").get<ULONGLONG>());
                    TranslationContext expired;
                    expired.Restore({{"expired", "过期"}}, GetTickCount64() - 1);
                    Check(expired.Get(true, 8).empty(), "Expired context was restored");
                    // If creating the replacement fails, do not destroy the working controller.
                    auto restart = app.idleRestart_;
                    app.idleRestart_ = [&](const std::string& payload) {
                        return StartIdleReplacement((root / L"missing.exe").wstring(), instance, payload);
                    };
                    app.usedResources_ = true;
                    app.TryIdle();
                    Check(IsWindow(app.window_), "Failed handoff destroyed controller");
                    app.idleRestart_ = restart;
                }
                if (phase == 2) {
                    Write(root / L"phase-2.json", report);
                    DestroyWindow(app.window_);
                    return;
                }
                step("open-settings");
                app.ShowSettings(false);
                Check(!IsWindowVisible(app.settingsWindow_.Window()), "Test displayed settings");
                app.TryIdle();
                Check(handoffs == 0, "Open settings were discarded");
                // Blocked UI/worker states must never trigger a replacement, even if a timer fires.
                step("close-settings");
                SendMessageW(app.settingsWindow_.Window(), WM_CLOSE, 0, 0);
                Check(!app.settingsWindow_.Window(), "Settings did not close");
                app.apiActive_ = true;
                app.TryIdle();
                Check(handoffs == 0, "Active API was discarded");
                app.apiActive_ = false;
                app.stage_ = Stage::Displaying;
                app.TryIdle();
                Check(handoffs == 0, "Active translation was discarded");
                app.stage_ = Stage::Idle;
                std::promise<void> started, finish;
                auto wait = finish.get_future().share();
                app.worker_.Submit([&](std::stop_token) {
                    started.set_value();
                    wait.wait();
                });
                started.get_future().wait();
                app.TryIdle();
                Check(handoffs == 0, "Running worker was discarded");
                finish.set_value();
                step("ocr");
                {
                    auto image = ReadUserScreenshot();
                    Check(!RecognizeBundled(image, OcrEnglish, {}).empty(),
                          "Real user screenshot OCR failed");
                }
                report["after_close"] = Memory();
                app.ArmIdle();
                step("waiting-idle");
            } catch (...) {
                failure = std::current_exception();
                DestroyWindow(app.window_);
            }
        });
        // Post above is queued before the controller exists; Run's first posted event drives it.
        std::jthread wake([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                HWND h{};
                while ((h = FindWindowExW(nullptr, h, L"SAT.Controller", nullptr))) {
                    DWORD pid{};
                    GetWindowThreadProcessId(h, &pid);
                    if (pid == GetCurrentProcessId()) {
                        PostMessageW(h, UiMessage, 0, 0);
                        return;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        auto code = app.Run();
        wake.request_stop();
        wake.join();
        if (failure)
            std::rethrow_exception(failure);
        Check(phase == 2 || handoffs == 1, "Idle transition did not occur exactly once");
        return code;
    }
};
} // namespace sat
int wmain(int argc, wchar_t** argv) {
    std::filesystem::path root;
    HANDLE instance{};
    struct TestDesktop {
        HDESK handle{};
        ~TestDesktop() {
            if (handle)
                CloseDesktop(handle);
        }
    } desktop;
    try {
        int phase = -1;
        for (int i = 1; i < argc; ++i) {
            if (std::wstring_view(argv[i]) == L"--phase" && i + 1 < argc)
                phase = _wtoi(argv[++i]);
            else if (std::wstring_view(argv[i]) == L"--root" && i + 1 < argc)
                root = argv[++i];
        }
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_HOTKEY_CLASS};
        InitCommonControlsEx(&controls);
        if (phase >= 0) {
            auto command = GetCommandLineW();
            auto end = command[0] == L'"' ? wcschr(command + 1, L'"') : wcschr(command, L' ');
            Check(end != nullptr, "Missing command arguments");
            auto resume = sat::ReceiveIdleReplacement(end + 1);
            instance = resume.instance ? resume.instance : CreateMutexW(nullptr, FALSE, nullptr);
            int result = sat::AppIdleTest::Run(phase, root, instance, std::move(resume.payload));
            CloseHandle(instance);
            return result;
        }
        root =
            std::filesystem::absolute(L"test-output") / (L"idle-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(root);
        sat::Settings settings;
        settings.tray = false;
        settings.startup = false;
        settings.ocrMode = sat::OcrKorean;
        settings.encryptedKey = sat::ProtectSecret("idle-test-key");
        HWND probe = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
        int shortcutIndex = 0;
        for (UINT key = VK_F13; key <= VK_F24 && shortcutIndex < 3; ++key)
            if (RegisterHotKey(probe, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, key)) {
                settings.hotkeys[shortcutIndex++] = {MOD_CONTROL | MOD_ALT | MOD_SHIFT, key};
                UnregisterHotKey(probe, 1);
            }
        DestroyWindow(probe);
        Check(shortcutIndex == 3, "Insufficient free test shortcuts");
        sat::Store(root).SaveSettings(settings);
        auto exe = sat::Executable();
        std::wstring command = L"\"" + exe + L"\" --phase 0 --root \"" + root.wstring() + L"\"";
        auto desktopName = L"SAT.Tests." + std::to_wstring(GetCurrentProcessId());
        desktop.handle = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        Check(desktop.handle != nullptr, "Cannot create isolated desktop for focus regression");
        STARTUPINFOW startup{sizeof(startup)};
        startup.lpDesktop = desktopName.data();
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION child{};
        Check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                             nullptr, &startup, &child),
              "Cannot start hidden lifecycle test");
        CloseHandle(child.hThread);
        auto until = GetTickCount64() + 35000;
        while (GetTickCount64() < until && !std::filesystem::exists(root / L"phase-2.json") &&
               !std::filesystem::exists(root / L"error.json"))
            Sleep(50);
        auto exited = WaitForSingleObject(child.hProcess, 1000);
        DWORD code{};
        GetExitCodeProcess(child.hProcess, &code);
        CloseHandle(child.hProcess);
        if (std::filesystem::exists(root / L"error.json"))
            throw std::runtime_error(Read(root / L"error.json").dump());
        Check(exited == WAIT_OBJECT_0 && code == 0, "First lifecycle process failed");
        Check(std::filesystem::exists(root / L"phase-2.json"), "Resumed process failed or timed out");
        for (int phaseIndex = 0; phaseIndex < 3; ++phaseIndex)
            std::cout << Read(root / (L"phase-" + std::to_wstring(phaseIndex) + L".json")).dump() << "\n";
        std::cout << "IdleLifecycleTests: PASS (offscreen first settings requests via relaunch, tray and "
                     "menu, real screenshot OCR, two handoffs, hotkeys, session, memory, failed launch)\n";
        return 0;
    } catch (const std::exception& e) {
        if (instance)
            CloseHandle(instance);
        if (!root.empty() && !std::filesystem::exists(root / L"error.json"))
            Write(root / L"error.json", {{"error", e.what()}});
        std::cerr << e.what() << "\n";
        return 1;
    }
}
