#pragma once
#include "common/Types.h"
#include "providers/ContentTranslation.h"
#include "app/Worker.h"
#include <deque>
namespace winrt::Microsoft::UI::Xaml {
struct FrameworkElement;
}
namespace winrt::Windows::ApplicationModel::DataTransfer {
struct DataPackageView;
}
namespace sat {
class TextTranslationPage {
    friend class TextTranslationUiTest;
    struct Impl;
    struct Turn {
        std::string source, result, target;
        std::vector<TranslationAttachment> images;
    };
    std::unique_ptr<Impl> ui_;
    Worker worker_;
    std::mutex queueMutex_;
    std::deque<std::function<void()>> queue_;
    HWND owner_{};
    std::shared_ptr<int> lifetime_;
    Settings settings_;
    std::string key_, draft_, target_{"zh-Hans"};
    std::wstring status_;
    std::vector<Turn> turns_;
    std::vector<TranslationAttachment> images_;
    std::optional<ContentTranslationRequest> retry_;
    std::function<void(const std::string&)> targetChanged_;
    unsigned long long generation_{};
    size_t contextStart_{};
    ULONGLONG contextExpires_{};
    bool busy_{}, loading_{}, composing_{}, initialized_{};
    void Post(std::function<void()> action);
    void Refresh();
    void RenderConversation();
    void RenderAttachments();
    void Send(bool retry = false);
    void Cancel();
    void ImportFiles(std::vector<std::filesystem::path> paths);
    void ImportBytes(std::vector<unsigned char> bytes);
    void PickImages();
    void TrimSession();
    void Error(const std::wstring& message);
    void PasteImages();
    void ImportData(winrt::Windows::ApplicationModel::DataTransfer::DataPackageView content);

public:
    static constexpr UINT DispatchMessage = WM_APP + 45;
    TextTranslationPage();
    ~TextTranslationPage();
    void Attach(winrt::Microsoft::UI::Xaml::FrameworkElement root, HWND owner, const Settings&,
                const std::string& key, std::function<void(const std::string&)> targetChanged);
    void Detach();
    void Dispatch();
    void UpdateConnection(const Settings&, const std::string& key);
    void ClearContext();
    void AdaptLayout(bool compact);
    void Notify(const std::wstring& message) {
        Error(message);
    }
    bool BackgroundIdle();
    std::string Snapshot();
    void Restore(const std::string&);
};
} // namespace sat
