#include <winsock2.h>
#include <ws2tcpip.h>
#include "settings/SettingsWindow.h"
#include "settings/WinUIRuntime.h"
#include "common/Platform.h"
#include <nlohmann/json.hpp>
#include <wincodec.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <iostream>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
using namespace sat;
namespace xaml = winrt::Microsoft::UI::Xaml;
using namespace xaml::Controls;
using Json = nlohmann::json;
namespace {
const char* currentStep = "initialization";
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (WinUIRuntime::ProcessMessage(message))
            continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
template <class F> void Wait(F condition) {
    auto until = GetTickCount64() + 10000;
    while (!condition() && GetTickCount64() < until) {
        Pump();
        Sleep(10);
    }
    Check(condition(), (std::string("asynchronous UI operation did not finish: ") + currentStep).c_str());
    Pump();
}
void Invoke(xaml::FrameworkElement root, const wchar_t* name) {
    xaml::Automation::Peers::ButtonAutomationPeer peer(root.FindName(name).as<Button>());
    peer.GetPattern(xaml::Automation::Peers::PatternInterface::Invoke)
        .as<xaml::Automation::Provider::IInvokeProvider>()
        .Invoke();
    Pump();
}
class LocalServer {
    SOCKET listener_{INVALID_SOCKET};
    std::jthread thread_;
    std::mutex mutex_;
    std::vector<Json> requests_;

public:
    unsigned short port{};
    std::atomic<int> status{200}, delay{};
    LocalServer() {
        WSADATA data{};
        Check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Check(listener_ != INVALID_SOCKET, "socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                  listen(listener_, 4) == 0,
              "loopback server could not bind");
        int length = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port = ntohs(address.sin_port);
        thread_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                fd_set sockets;
                FD_ZERO(&sockets);
                FD_SET(listener_, &sockets);
                timeval timeout{0, 100000};
                if (select(0, &sockets, nullptr, nullptr, &timeout) <= 0)
                    continue;
                auto client = accept(listener_, nullptr, nullptr);
                if (client == INVALID_SOCKET)
                    continue;
                DWORD receiveTimeout = 3000;
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receiveTimeout),
                           sizeof(receiveTimeout));
                std::string data;
                char buffer[8192];
                size_t headerEnd = std::string::npos, contentLength{};
                while (!stop.stop_requested() && data.size() < 40 * 1024 * 1024) {
                    int count = recv(client, buffer, sizeof(buffer), 0);
                    if (count <= 0)
                        break;
                    data.append(buffer, count);
                    if (headerEnd == data.npos) {
                        headerEnd = data.find("\r\n\r\n");
                        if (headerEnd != data.npos) {
                            auto headers = data.substr(0, headerEnd);
                            for (auto& c : headers)
                                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            auto at = headers.find("content-length:");
                            if (at != headers.npos)
                                contentLength = std::stoull(headers.substr(at + 15));
                        }
                    }
                    if (headerEnd != data.npos && data.size() >= headerEnd + 4 + contentLength)
                        break;
                }
                try {
                    auto request = Json::parse(data.substr(headerEnd + 4, contentLength));
                    {
                        std::lock_guard lock(mutex_);
                        requests_.push_back(std::move(request));
                    }
                    int wait = delay.exchange(0), responseStatus = status.exchange(200);
                    for (int elapsed = 0; elapsed < wait && !stop.stop_requested(); elapsed += 10)
                        Sleep(10);
                    auto body =
                        responseStatus == 200
                            ? Json{{"choices",
                                    Json::array(
                                        {{{"finish_reason", "stop"},
                                          {"message",
                                           {{"content", "Translated result.\n\nSecond paragraph."}}}}})}}
                                  .dump()
                            : std::string("{\"error\":\"fixture rejection\"}");
                    auto response =
                        "HTTP/1.1 " + std::to_string(responseStatus) +
                        " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\n\r\n" + body;
                    send(client, response.data(), static_cast<int>(response.size()), 0);
                } catch (...) {
                }
                shutdown(client, SD_BOTH);
                closesocket(client);
            }
        });
    }
    ~LocalServer() {
        thread_.request_stop();
        thread_.join();
        closesocket(listener_);
        WSACleanup();
    }
    size_t Count() {
        std::lock_guard lock(mutex_);
        return requests_.size();
    }
    Json Last() {
        std::lock_guard lock(mutex_);
        return requests_.back();
    }
};
class Offscreen {
    HHOOK hook_{};
    static LRESULT CALLBACK Hook(int code, WPARAM w, LPARAM l) {
        if (code == HCBT_CREATEWND) {
            wchar_t name[64]{};
            GetClassNameW(reinterpret_cast<HWND>(w), name, 64);
            if (std::wstring_view(name) == L"SAT.Settings") {
                auto create = reinterpret_cast<CBT_CREATEWNDW*>(l)->lpcs;
                create->x = create->y = -32000;
                create->dwExStyle |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            }
        }
        return CallNextHookEx(nullptr, code, w, l);
    }

public:
    Offscreen() {
        hook_ = SetWindowsHookExW(WH_CBT, Hook, nullptr, GetCurrentThreadId());
        Check(hook_ != nullptr, "offscreen hook failed");
    }
    ~Offscreen() {
        UnhookWindowsHookEx(hook_);
    }
};
void Render(xaml::FrameworkElement root, const std::filesystem::path& path) {
    auto layoutReady = GetTickCount64() + 200;
    Wait([&] {
        root.UpdateLayout();
        return GetTickCount64() >= layoutReady && root.ActualWidth() > 600;
    });
    root.UpdateLayout();
    Pump();
    xaml::Media::Imaging::RenderTargetBitmap bitmap;
    auto render = bitmap.RenderAsync(root);
    Wait([&] { return render.Status() != winrt::Windows::Foundation::AsyncStatus::Started; });
    render.GetResults();
    auto pixels = bitmap.GetPixelsAsync();
    Wait([&] { return pixels.Status() != winrt::Windows::Foundation::AsyncStatus::Started; });
    auto bytes = pixels.GetResults();
    std::vector<unsigned char> data(bytes.Length());
    winrt::Windows::Storage::Streams::DataReader::FromBuffer(bytes).ReadBytes(data);
    Check(bitmap.PixelWidth() > 600 && bitmap.PixelHeight() > 400, "offscreen render has no usable surface");
    ComPtr<IWICImagingFactory> factory;
    CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
            "test");
    ComPtr<IWICStream> stream;
    CheckHR(factory->CreateStream(&stream), "test");
    CheckHR(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "test");
    ComPtr<IWICBitmapEncoder> encoder;
    CheckHR(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "test");
    CheckHR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "test");
    ComPtr<IWICBitmapFrameEncode> frame;
    CheckHR(encoder->CreateNewFrame(&frame, nullptr), "test");
    CheckHR(frame->Initialize(nullptr), "test");
    CheckHR(frame->SetSize(bitmap.PixelWidth(), bitmap.PixelHeight()), "test");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    CheckHR(frame->SetPixelFormat(&format), "test");
    CheckHR(frame->WritePixels(bitmap.PixelHeight(), bitmap.PixelWidth() * 4, static_cast<UINT>(data.size()),
                               data.data()),
            "test");
    CheckHR(frame->Commit(), "test");
    CheckHR(encoder->Commit(), "test");
}
} // namespace
namespace sat {
class TextTranslationUiTest {
public:
    static void Run() {
        LocalServer server;
        Offscreen offscreen;
        WinUIRuntime runtime(true);
        Settings options;
        options.provider = "openai";
        options.model = "gpt-4.1-mini";
        options.baseUrl = L"http://127.0.0.1:" + std::to_wstring(server.port) + L"/v1";
        options.proxyMode = 1;
        options.textTargetLanguage = "en";
        SettingsCallbacks callbacks;
        std::string target;
        callbacks.targetLanguage = [&](const std::string& language) { target = language; };
        SettingsWindow window;
        window.Show(options, "fixture-key", {}, callbacks, false);
        auto root = window.Content();
        ShowWindow(window.Window(), SW_SHOWNOACTIVATE);
        Wait([&] {
            root.UpdateLayout();
            return root.IsLoaded();
        });
        RECT bounds{};
        GetWindowRect(window.Window(), &bounds);
        Check(bounds.right < 0 && bounds.bottom < 0, "test window entered a user monitor");
        auto input = root.FindName(L"TextInput").as<TextBox>();
        auto language = root.FindName(L"TextTarget").as<ComboBox>();
        std::filesystem::create_directories("test-output/text-ui");
        window.textPage_.turns_ = {{"Could you send me the updated document before Friday?",
                                    "你能在周五之前把更新后的文档发给我吗？",
                                    "zh-Hans",
                                    {}}};
        input.Text(L"Please let me know when the translation is ready.");
        language.SelectedIndex(0);
        window.textPage_.RenderConversation();
        currentStep = "reference layout rendering";
        Render(root, std::filesystem::absolute("test-output/text-ui/reference-light.png"));
        Check(input.BorderThickness().Left == 0 &&
                  root.FindName(L"LanguageCard").as<Border>().ActualHeight() >= 70,
              "composer or language card diverged from the approved reference");
        auto referenceMessages = root.FindName(L"Conversation").as<StackPanel>();
        Check(referenceMessages.Children()
                          .GetAt(0)
                          .as<StackPanel>()
                          .Children()
                          .GetAt(0)
                          .as<TextBlock>()
                          .Text() == L"你" &&
                  referenceMessages.Children()
                          .GetAt(1)
                          .as<StackPanel>()
                          .Children()
                          .GetAt(0)
                          .as<TextBlock>()
                          .Text() == L"译文",
              "message role labels are missing");
        root.RequestedTheme(xaml::ElementTheme::Dark);
        auto referenceThemeReady = GetTickCount64() + 350;
        Wait([&] { return GetTickCount64() >= referenceThemeReady; });
        Render(root, std::filesystem::absolute("test-output/text-ui/reference-dark.png"));
        root.RequestedTheme(xaml::ElementTheme::Light);
        auto paneNavigation = root.FindName(L"Navigation").as<NavigationView>();
        Check(paneNavigation.IsPaneOpen() && !paneNavigation.IsPaneToggleButtonVisible() &&
                  root.FindName(L"PaneToggle") == nullptr,
              "navigation pane unexpectedly exposes a collapse control");
        window.textPage_.turns_.clear();
        window.textPage_.RenderConversation();
        language.SelectedIndex(2);
        Pump();
        Check(target == "ja", "target preference callback missing");
        input.Text(L"First source paragraph.\n\nSecond source paragraph.");
        currentStep = "first text reply";
        Invoke(root, L"SendText");
        Wait([&] { return !window.textPage_.busy_; });
        auto conversation = root.FindName(L"Conversation").as<StackPanel>();
        Check(conversation.Children().Size() == 2, "text exchange did not create a request and a reply");
        auto reply = conversation.Children()
                         .GetAt(1)
                         .as<StackPanel>()
                         .FindName(L"MessageContent")
                         .as<StackPanel>()
                         .Children()
                         .GetAt(0)
                         .as<TextBlock>()
                         .Text();
        Check(reply == L"Translated result.\n\nSecond paragraph.",
              "reply contains text other than the translation");
        Check(server.Last()["messages"][0]["content"].get<std::string>().find("Japanese") !=
                  std::string::npos,
              "selected language did not reach the HTTP request");
        currentStep = "bitmap data import";
        auto fileOperation = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(
            std::filesystem::absolute("test-fixtures/user/handshake-error-en.png").wstring());
        Wait([&] { return fileOperation.Status() != winrt::Windows::Foundation::AsyncStatus::Started; });
        winrt::Windows::ApplicationModel::DataTransfer::DataPackage imageData;
        imageData.SetBitmap(winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromFile(
            fileOperation.GetResults()));
        window.textPage_.ImportData(imageData.GetView());
        Wait([&] { return !window.textPage_.loading_; });
        Check(window.textPage_.images_.size() == 1, "clipboard bitmap data did not reach image import");
        window.textPage_.images_.clear();
        window.textPage_.RenderAttachments();
        window.textPage_.ImportFiles(
            {"test-fixtures/user/handshake-error-en.png", "test-fixtures/user/asahi-grid-ja.png"});
        Wait([&] { return !window.textPage_.loading_; });
        Check(window.textPage_.images_.size() == 2, "real English and Japanese screenshots did not import");
        input.Text(L"Translate both images.");
        currentStep = "multiple-image reply";
        Invoke(root, L"SendText");
        Wait([&] { return !window.textPage_.busy_; });
        Check(server.Last()["messages"][1]["content"].size() == 3 && conversation.Children().Size() == 4,
              "mixed text and image exchange lost content");
        Check(window.textPage_.turns_.back().images.size() == 2 &&
                  window.textPage_.turns_.back().images[0].base64.empty(),
              "completed conversation retained full upload data");
        std::filesystem::create_directories("test-output/text-ui");
        currentStep = "conversation rendering";
        Render(root, std::filesystem::absolute("test-output/text-ui/conversation-light.png"));
        root.RequestedTheme(xaml::ElementTheme::Dark);
        auto themeReady = GetTickCount64() + 350;
        Wait([&] { return GetTickCount64() >= themeReady; });
        Render(root, std::filesystem::absolute("test-output/text-ui/conversation-dark.png"));
        root.RequestedTheme(xaml::ElementTheme::Light);
        // 页面截图在同一离屏合成表面采集；窗口重建另行验证会话与控件布局。
        auto initialNavigation = root.FindName(L"Navigation").as<NavigationView>();
        initialNavigation.SelectedItem(initialNavigation.MenuItems().GetAt(1));
        currentStep = "screen page rendering";
        Render(root, std::filesystem::absolute("test-output/text-ui/screen.png"));
        initialNavigation.SelectedItem(initialNavigation.FooterMenuItems().GetAt(0));
        auto initialTabs = root.FindName(L"SettingsTabs").as<SelectorBar>();
        for (int i = 0; i < 4; ++i) {
            currentStep = "settings page rendering";
            initialTabs.SelectedItem(initialTabs.Items().GetAt(i));
            Render(root,
                   std::filesystem::absolute("test-output/text-ui/settings-" + std::to_string(i) + ".png"));
        }
        SetWindowPos(window.Window(), nullptr, -32000, -32000, 860, 580, SWP_NOACTIVATE | SWP_NOZORDER);
        initialNavigation.SelectedItem(initialNavigation.MenuItems().GetAt(0));
        currentStep = "compact page rendering";
        Render(root, std::filesystem::absolute("test-output/text-ui/text-compact.png"));
        SetWindowPos(window.Window(), nullptr, -32000, -32000, Width(bounds), Height(bounds),
                     SWP_NOACTIVATE | SWP_NOZORDER);
        server.status = 400;
        currentStep = "error and retry";
        input.Text(L"Request that fails.");
        Invoke(root, L"SendText");
        Wait([&] { return !window.textPage_.busy_; });
        Check(root.FindName(L"RetryText").as<Button>().Visibility() == xaml::Visibility::Visible &&
                  conversation.Children().Size() == 5,
              "error appeared as assistant translation or retry missing");
        Invoke(root, L"RetryText");
        Wait([&] { return !window.textPage_.busy_; });
        Check(conversation.Children().Size() == 6, "retry duplicated source or failed to add translation");
        server.delay = 600;
        currentStep = "request cancellation";
        input.Text(L"Request to cancel.");
        auto count = server.Count();
        Invoke(root, L"SendText");
        Wait([&] { return server.Count() > count; });
        auto started = GetTickCount64();
        Invoke(root, L"StopText");
        Check(GetTickCount64() - started < 200 && !window.textPage_.busy_, "cancellation blocked the UI");
        Wait([&] { return window.textPage_.worker_.Idle(); });
        Check(conversation.Children().Size() == 7, "cancelled request appended a late reply");
        input.Text(L"Unsent draft.");
        currentStep = "close during bitmap read";
        window.textPage_.ImportData(imageData.GetView());
        window.Close();
        window.Show(options, "fixture-key", {}, callbacks, false);
        root = window.Content();
        Check(root.FindName(L"TextInput").as<TextBox>().Text() == L"Unsent draft." &&
                  window.textPage_.turns_.size() == 4 && window.textPage_.target_ == "ja" &&
                  window.textPage_.images_.empty(),
              "reopening lost conversation, draft, or target");
        auto snapshot = window.TextSession();
        window.Close();
        SettingsWindow restored;
        restored.RestoreTextSession(snapshot);
        restored.Show(options, "fixture-key", {}, callbacks, false);
        Check(restored.textPage_.turns_.size() == 4 && restored.textPage_.draft_ == "Unsent draft.",
              "idle snapshot lost text conversation");
        auto restoredRoot = restored.Content();
        currentStep = "restored window loading";
        ShowWindow(restored.Window(), SW_SHOWNOACTIVATE);
        Wait([&] {
            restoredRoot.UpdateLayout();
            return restoredRoot.IsLoaded();
        });
        auto navigation = restoredRoot.FindName(L"Navigation").as<NavigationView>();
        navigation.SelectedItem(navigation.MenuItems().GetAt(1));
        Check(restoredRoot.FindName(L"ScreenPage").as<StackPanel>().Visibility() == xaml::Visibility::Visible,
              "screen translation navigation failed");
        navigation.SelectedItem(navigation.FooterMenuItems().GetAt(0));
        auto tabs = restoredRoot.FindName(L"SettingsTabs").as<SelectorBar>();
        for (int i = 0; i < 4; ++i) {
            tabs.SelectedItem(tabs.Items().GetAt(i));
            Pump();
        }
        tabs.SelectedItem(tabs.Items().GetAt(0));
        navigation.SelectedItem(navigation.MenuItems().GetAt(0));
        Wait([&] {
            restoredRoot.UpdateLayout();
            return restoredRoot.FindName(L"TextInput").as<TextBox>().ActualWidth() > 100;
        });
        restored.Close();
    }
};
} // namespace sat
int main() {
    try {
        TextTranslationUiTest::Run();
        std::cout << "TextTranslationUiTests: PASS (offscreen UI, loopback HTTP, real screenshot input)\n";
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "WinUI failure: " << Utf8(std::wstring(error.message())) << " (" << error.code().value
                  << ")\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
