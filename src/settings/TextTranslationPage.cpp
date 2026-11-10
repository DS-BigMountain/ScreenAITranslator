#include "TextTranslationPage.h"
#include "capture/ImageInput.h"
#include "common/Platform.h"
#include "common/Secret.h"
#include "settings/Store.h"
#include "providers/Catalog.h"
#include <nlohmann/json.hpp>
#include <commdlg.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <robuffer.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <algorithm>

namespace sat {
namespace xaml = winrt::Microsoft::UI::Xaml;
using namespace xaml::Controls;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Storage::Streams;
using Json = nlohmann::json;
namespace {
xaml::Media::Imaging::WriteableBitmap Thumbnail(const std::string& base64) {
    DWORD count{};
    if (base64.size() > 128 * 1024 ||
        !CryptStringToBinaryA(base64.c_str(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64, nullptr,
                              &count, nullptr, nullptr))
        throw AppError("image", "图片预览数据无效。");
    std::vector<BYTE> bytes(count);
    if (!CryptStringToBinaryA(base64.c_str(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64,
                              bytes.data(), &count, nullptr, nullptr))
        throw AppError("image", "图片预览数据无效。");
    ComPtr<IWICImagingFactory> factory;
    CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
            "image");
    ComPtr<IWICStream> stream;
    CheckHR(factory->CreateStream(&stream), "image");
    CheckHR(stream->InitializeFromMemory(bytes.data(), count), "image");
    ComPtr<IWICBitmapDecoder> decoder;
    CheckHR(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder),
            "image");
    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHR(decoder->GetFrame(0, &frame), "image");
    UINT width{}, height{};
    CheckHR(frame->GetSize(&width, &height), "image");
    if (!width || !height || width > 256 || height > 256)
        throw AppError("image", "图片预览尺寸无效。");
    ComPtr<IWICFormatConverter> converter;
    CheckHR(factory->CreateFormatConverter(&converter), "image");
    CheckHR(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                  nullptr, 0, WICBitmapPaletteTypeCustom),
            "image");
    xaml::Media::Imaging::WriteableBitmap bitmap(width, height);
    BYTE* pixels{};
    CheckHR(bitmap.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&pixels),
            "image");
    CheckHR(converter->CopyPixels(nullptr, width * 4, width * height * 4, pixels), "image");
    bitmap.Invalidate();
    return bitmap;
}
TextBlock MessageText(const std::string& text) {
    TextBlock block;
    block.TextWrapping(xaml::TextWrapping::Wrap);
    block.IsTextSelectionEnabled(true);
    block.FontSize(16);
    block.LineHeight(25);
    block.Text(Wide(text));
    return block;
}
} // namespace
struct TextTranslationPage::Impl {
    xaml::FrameworkElement root{nullptr};
    double composerHeight{128};
    bool compact{};
    template <class T> T Get(const wchar_t* name) const {
        return root.FindName(name).as<T>();
    }
};
TextTranslationPage::TextTranslationPage() = default;
TextTranslationPage::~TextTranslationPage() {
    Detach();
    worker_.Stop();
}
void TextTranslationPage::Post(std::function<void()> action) {
    std::lock_guard lock(queueMutex_);
    if (!owner_)
        return;
    queue_.push_back(std::move(action));
    PostMessageW(owner_, DispatchMessage, 0, 0);
}
void TextTranslationPage::Dispatch() {
    std::deque<std::function<void()>> work;
    {
        std::lock_guard lock(queueMutex_);
        work.swap(queue_);
    }
    for (auto& action : work)
        action();
}
void TextTranslationPage::Error(const std::wstring& text) {
    status_ = text;
    Refresh();
}
void TextTranslationPage::Refresh() {
    if (!ui_)
        return;
    bool active = busy_ || loading_;
    for (auto name : {L"SendText", L"TextInput", L"TextTarget", L"ClearConversation"})
        ui_->Get<Control>(name).IsEnabled(!active);
    for (auto name : {L"AddImage"})
        ui_->Get<Control>(name).IsEnabled(!active && images_.size() < 4 &&
                                          !IsTextService(settings_.provider));
    ui_->Get<Button>(L"StopText")
        .Visibility(active ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    ui_->Get<Button>(L"RetryText")
        .Visibility(retry_ && !active ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    ui_->Get<StackPanel>(L"TextEmpty")
        .Visibility(turns_.empty() ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
    auto status = ui_->Get<TextBlock>(L"TextStatus");
    status.Text(status_);
    status.Visibility(status_.empty() ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
    ui_->Get<TextBlock>(L"SendHint")
        .Text(settings_.textCtrlEnter ? L"Ctrl+Enter 翻译 · Enter 换行" : L"Enter 翻译 · Shift+Enter 换行");
    std::wstring connection = Preset(settings_.provider).name;
    connection += L" · " + Wide(settings_.model);
    if (key_.empty())
        connection += L" · 请配置连接";
    else if (IsTextService(settings_.provider))
        connection += L" · 仅支持文字";
    ui_->Get<TextBox>(L"TextInput")
        .PlaceholderText(IsTextService(settings_.provider) ? L"输入要翻译的文字"
                                                           : L"输入要翻译的内容，或粘贴、拖入图片");
    ui_->Get<TextBlock>(L"TextConnectionLabel").Text(connection);
    ui_->Get<HyperlinkButton>(L"ScreenConnection").Content(winrt::box_value(L"共用连接：" + connection));
}
void TextTranslationPage::RenderConversation() {
    if (!ui_)
        return;
    auto panel = ui_->Get<StackPanel>(L"Conversation");
    panel.Children().Clear();
    for (const auto& turn : turns_) {
        for (bool user : {true, false}) {
            if (!user && turn.result.empty())
                continue;
            auto messageTemplate =
                ui_->root.Resources()
                    .Lookup(winrt::box_value(user ? L"UserMessageTemplate" : L"TranslationMessageTemplate"))
                    .as<xaml::DataTemplate>();
            auto row = messageTemplate.LoadContent().as<StackPanel>();
            row.Tag(winrt::box_value(user));
            row.MaxWidth(
                std::max(180.0, panel.ActualWidth() > 0 ? panel.ActualWidth() * (user ? .79 : .84) : 640.0));
            auto content = row.FindName(L"MessageContent").as<StackPanel>();
            if (user) {
                for (const auto& attachment : turn.images) {
                    xaml::Controls::Image image;
                    image.Source(Thumbnail(attachment.thumbnail));
                    image.MaxWidth(230);
                    image.MaxHeight(190);
                    image.Stretch(xaml::Media::Stretch::Uniform);
                    image.HorizontalAlignment(xaml::HorizontalAlignment::Left);
                    ToolTipService::SetToolTip(image, winrt::box_value(Wide(attachment.name)));
                    content.Children().Append(image);
                }
            }
            const auto& text = user ? turn.source : turn.result;
            if (!text.empty())
                content.Children().Append(MessageText(text));
            if (!user) {
                auto copy = row.FindName(L"CopyTranslation").as<Button>();
                copy.Click([this, text](auto const&, auto const&) {
                    try {
                        DataPackage data;
                        data.SetText(Wide(text));
                        Clipboard::SetContent(data);
                        Error(L"译文已复制。");
                    } catch (...) {
                        Error(L"无法写入剪贴板。");
                    }
                });
            }
            panel.Children().Append(row);
        }
    }
    Refresh();
    panel.UpdateLayout();
    auto scroll = ui_->Get<ScrollViewer>(L"ConversationScroll");
    scroll.ChangeView(
        nullptr,
        winrt::box_value(scroll.ScrollableHeight()).as<winrt::Windows::Foundation::IReference<double>>(),
        nullptr, true);
}
void TextTranslationPage::RenderAttachments() {
    if (!ui_)
        return;
    auto panel = ui_->Get<StackPanel>(L"ImageAttachments");
    panel.Children().Clear();
    ui_->Get<ScrollViewer>(L"AttachmentScroll")
        .Visibility(images_.empty() ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
    for (size_t i = 0; i < images_.size(); ++i) {
        StackPanel tile;
        tile.Spacing(5);
        tile.Width(110);
        Grid preview;
        preview.Height(74);
        xaml::Controls::Image image;
        image.Source(Thumbnail(images_[i].thumbnail));
        image.Height(74);
        image.Stretch(xaml::Media::Stretch::UniformToFill);
        ToolTipService::SetToolTip(image, winrt::box_value(Wide(images_[i].name)));
        preview.Children().Append(image);
        Button remove;
        FontIcon close;
        close.Glyph(L"\uE711");
        close.FontSize(11);
        remove.Content(close);
        remove.Style(ui_->root.Resources().Lookup(winrt::box_value(L"IconButton")).as<xaml::Style>());
        remove.Width(24);
        remove.Height(24);
        remove.MinHeight(24);
        remove.Padding({4, 4, 4, 4});
        remove.HorizontalAlignment(xaml::HorizontalAlignment::Right);
        remove.VerticalAlignment(xaml::VerticalAlignment::Top);
        remove.Margin({2, 2, 2, 2});
        ToolTipService::SetToolTip(remove, winrt::box_value(L"移除图片"));
        remove.Click([this, i](auto const&, auto const&) {
            if (busy_ || loading_ || i >= images_.size())
                return;
            images_.erase(images_.begin() + i);
            RenderAttachments();
            Refresh();
        });
        preview.Children().Append(remove);
        tile.Children().Append(preview);
        TextBlock label;
        label.Text(Wide(images_[i].name));
        label.FontSize(12);
        label.Opacity(.75);
        label.TextTrimming(xaml::TextTrimming::CharacterEllipsis);
        tile.Children().Append(label);
        panel.Children().Append(tile);
    }
}
void TextTranslationPage::Cancel() {
    ++generation_;
    worker_.Cancel();
    bool wasActive = busy_ || loading_;
    busy_ = loading_ = false;
    if (wasActive)
        Error(L"已停止。可重试或继续输入。");
    else
        Refresh();
}
void TextTranslationPage::ClearContext() {
    contextStart_ = turns_.size();
    contextExpires_ = 0;
}
void TextTranslationPage::UpdateConnection(const Settings& settings, const std::string& key) {
    bool changed = settings_.provider != settings.provider || settings_.baseUrl != settings.baseUrl ||
                   settings_.model != settings.model || (!key_.empty() && key_ != key) ||
                   settings_.requestParameters != settings.requestParameters ||
                   settings_.serviceRegion != settings.serviceRegion;
    if (changed) {
        Cancel();
        ClearContext();
    }
    settings_ = settings;
    if (!key_.empty())
        SecureZeroMemory(key_.data(), key_.size());
    key_ = key;
    Refresh();
}
void TextTranslationPage::Send(bool again) {
    if (!ui_ || busy_ || loading_ || composing_)
        return;
    try {
        if (key_.empty())
            throw AppError("settings", "请先在模型连接中填写密钥并保存设置。");
        ContentTranslationRequest request;
        if (again && retry_)
            request = *retry_;
        else {
            request.text = Utf8(std::wstring(ui_->Get<TextBox>(L"TextInput").Text()));
            request.target = target_;
            request.images = images_;
        }
        if (request.text.find_first_not_of(" \r\n\t") == request.text.npos && request.images.empty()) {
            Error(L"请输入文字或添加图片。");
            return;
        }
        if (request.text.size() > 64 * 1024)
            throw AppError("Input", "文字超过 64 KB 限制。");
        if (!request.images.empty() && IsTextService(settings_.provider))
            throw AppError("Input", "当前专用翻译服务仅支持文字，请选择支持图片的模型。");
        request.context.clear();
        if (settings_.contextEnabled && GetTickCount64() < contextExpires_) {
            auto begin = std::max(contextStart_, turns_.size() > static_cast<size_t>(settings_.contextSize)
                                                     ? turns_.size() - settings_.contextSize
                                                     : size_t{});
            for (size_t i = begin; i < turns_.size(); ++i)
                if (!turns_[i].source.empty() && !turns_[i].result.empty() &&
                    turns_[i].target == request.target)
                    request.context.push_back({turns_[i].source, turns_[i].result});
        }
        if (!again) {
            Turn turn{request.text, {}, request.target, request.images};
            for (auto& image : turn.images)
                image.base64.clear();
            turns_.push_back(std::move(turn));
            images_.clear();
            draft_.clear();
            ui_->Get<TextBox>(L"TextInput").Text(L"");
            TrimSession();
            RenderAttachments();
            RenderConversation();
        }
        retry_ = request;
        busy_ = true;
        Error(L"正在翻译…");
        auto id = ++generation_;
        auto options = settings_;
        auto secret = std::make_shared<Secret>(key_);
        worker_.Submit([this, id, request = std::move(request), options = std::move(options),
                        secret](std::stop_token stop) {
            try {
                ComScope com;
                auto result = TranslateContent(request, options, secret->value, stop);
                CheckStop(stop);
                Post([this, id, result = std::move(result)] {
                    if (!ui_ || id != generation_)
                        return;
                    busy_ = false;
                    if (!turns_.empty())
                        turns_.back().result = result;
                    contextExpires_ = GetTickCount64() + 30 * 60 * 1000;
                    retry_.reset();
                    status_.clear();
                    TrimSession();
                    RenderConversation();
                    ui_->Get<TextBox>(L"TextInput").Focus(xaml::FocusState::Programmatic);
                });
            } catch (const Cancelled&) {
            } catch (const AppError& error) {
                auto message = Wide(Sanitize(error.what(), secret->value));
                Post([this, id, message] {
                    if (ui_ && id == generation_) {
                        busy_ = false;
                        Error(L"翻译失败：" + message);
                    }
                });
            } catch (...) {
                Post([this, id] {
                    if (ui_ && id == generation_) {
                        busy_ = false;
                        Error(L"翻译未完成，请检查连接和模型能力。");
                    }
                });
            }
        });
    } catch (const AppError& error) {
        Error(Wide(error.what()));
    } catch (...) {
        Error(L"无法发送，请检查输入和连接设置。");
    }
}
void TextTranslationPage::ImportFiles(std::vector<std::filesystem::path> paths) {
    if (!ui_ || busy_ || (loading_ && !paths.empty()))
        return;
    if (IsTextService(settings_.provider)) {
        Error(L"当前专用翻译服务仅支持文字。");
        return;
    }
    if (paths.empty() || paths.size() + images_.size() > 4) {
        Error(L"每次最多添加四张图片。");
        return;
    }
    loading_ = true;
    Error(L"正在读取图片…");
    auto id = ++generation_;
    worker_.Submit([this, id, paths = std::move(paths)](std::stop_token stop) {
        try {
            std::vector<TranslationAttachment> imported;
            for (const auto& path : paths)
                imported.push_back(ImportImageFile(path, stop));
            CheckStop(stop);
            Post([this, id, imported = std::move(imported)]() mutable {
                if (!ui_ || id != generation_)
                    return;
                loading_ = false;
                for (auto& image : imported)
                    images_.push_back(std::move(image));
                status_.clear();
                RenderAttachments();
                Refresh();
            });
        } catch (const Cancelled&) {
        } catch (const AppError& error) {
            auto message = Wide(error.what());
            Post([this, id, message] {
                if (ui_ && id == generation_) {
                    loading_ = false;
                    Error(message);
                }
            });
        } catch (...) {
            Post([this, id] {
                if (ui_ && id == generation_) {
                    loading_ = false;
                    Error(L"无法读取图片。");
                }
            });
        }
    });
}
void TextTranslationPage::ImportBytes(std::vector<unsigned char> bytes) {
    if (!ui_ || busy_ || images_.size() >= 4 || IsTextService(settings_.provider))
        return;
    loading_ = true;
    Error(L"正在读取图片…");
    auto id = ++generation_;
    worker_.Submit([this, id, bytes = std::move(bytes)](std::stop_token stop) {
        try {
            auto image = ImportImage(bytes, "clipboard.png", stop);
            Post([this, id, image = std::move(image)]() mutable {
                if (!ui_ || id != generation_)
                    return;
                loading_ = false;
                images_.push_back(std::move(image));
                status_.clear();
                RenderAttachments();
                Refresh();
            });
        } catch (const Cancelled&) {
        } catch (...) {
            Post([this, id] {
                if (ui_ && id == generation_) {
                    loading_ = false;
                    Error(L"无法读取剪贴板图片，或图片超过 20 MB。");
                }
            });
        }
    });
}
void TextTranslationPage::PickImages() {
    if (busy_ || loading_)
        return;
    std::vector<wchar_t> buffer(32768);
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner_;
    dialog.lpstrFilter = L"图片文件\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp\0所有文件\0*.*\0\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog))
        return;
    std::filesystem::path first(buffer.data());
    auto next = buffer.data() + wcslen(buffer.data()) + 1;
    std::vector<std::filesystem::path> paths;
    if (!*next)
        paths.push_back(first);
    else
        for (; *next; next += wcslen(next) + 1)
            paths.push_back(first / next);
    ImportFiles(std::move(paths));
}
void TextTranslationPage::PasteImages() {
    try {
        ImportData(Clipboard::GetContent());
    } catch (...) {
        Error(L"无法读取剪贴板图片。");
    }
}
void TextTranslationPage::ImportData(DataPackageView content) {
    if (!ui_ || busy_ || loading_ || images_.size() >= 4 || IsTextService(settings_.provider))
        return;
    auto weak = std::weak_ptr<int>(lifetime_);
    auto id = ++generation_;
    loading_ = true;
    Error(L"正在读取剪贴板…");
    [](TextTranslationPage* self, std::weak_ptr<int> weak, unsigned long long id,
       DataPackageView content) -> winrt::fire_and_forget {
        try {
            if (content.Contains(StandardDataFormats::Bitmap())) {
                auto reference = co_await content.GetBitmapAsync();
                auto stream = co_await reference.OpenReadAsync();
                if (!stream.Size() || stream.Size() > MaxInputImageBytes)
                    throw AppError("Input", "剪贴板图片超过 20 MB。");
                DataReader reader(stream);
                auto size = static_cast<uint32_t>(stream.Size());
                auto loaded = co_await reader.LoadAsync(size);
                if (loaded != size)
                    throw AppError("Input", "剪贴板图片读取不完整。");
                std::vector<unsigned char> bytes(size);
                reader.ReadBytes(bytes);
                if (weak.expired())
                    co_return;
                if (id != self->generation_)
                    co_return;
                self->loading_ = false;
                self->ImportBytes(std::move(bytes));
            } else if (content.Contains(StandardDataFormats::StorageItems())) {
                auto items = co_await content.GetStorageItemsAsync();
                if (weak.expired())
                    co_return;
                if (id != self->generation_)
                    co_return;
                std::vector<std::filesystem::path> paths;
                for (auto const& item : items)
                    if (item.IsOfType(winrt::Windows::Storage::StorageItemTypes::File))
                        paths.emplace_back(item.Path().c_str());
                self->loading_ = false;
                self->ImportFiles(std::move(paths));
            } else {
                if (!weak.expired() && id == self->generation_) {
                    self->loading_ = false;
                    self->Error(L"剪贴板中没有图片。");
                }
            }
        } catch (...) {
            if (!weak.expired() && id == self->generation_) {
                self->loading_ = false;
                self->Error(L"无法读取剪贴板图片。");
            }
        }
    }(this, weak, id, content);
}
void TextTranslationPage::Attach(xaml::FrameworkElement root, HWND owner, const Settings& settings,
                                 const std::string& key, std::function<void(const std::string&)> changed) {
    ui_ = std::make_unique<Impl>();
    ui_->root = root;
    {
        std::lock_guard lock(queueMutex_);
        owner_ = owner;
    }
    lifetime_ = std::make_shared<int>(0);
    targetChanged_ = std::move(changed);
    if (!initialized_) {
        target_ = settings.textTargetLanguage;
        initialized_ = true;
    }
    UpdateConnection(settings, key);
    auto target = ui_->Get<ComboBox>(L"TextTarget");
    int selected = 0;
    const auto& languages = TextTargetLanguages();
    for (size_t i = 0; i < languages.size(); ++i) {
        target.Items().Append(winrt::box_value(languages[i].label));
        if (target_ == languages[i].id)
            selected = static_cast<int>(i);
    }
    target.SelectedIndex(selected);
    target.SelectionChanged([this](auto const& sender, auto const&) {
        auto index = sender.template as<ComboBox>().SelectedIndex();
        if (index >= 0) {
            target_ = TextTargetLanguages().at(index).id;
            if (targetChanged_)
                targetChanged_(target_);
        }
    });
    auto input = ui_->Get<TextBox>(L"TextInput");
    input.Text(Wide(draft_));
    input.TextCompositionStarted([this](auto const&, auto const&) { composing_ = true; });
    input.TextCompositionEnded([this](auto const&, auto const&) { composing_ = false; });
    input.PreviewKeyDown([this](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
        if (args.Key() != winrt::Windows::System::VirtualKey::Enter || composing_ || GetKeyState(VK_MENU) < 0)
            return;
        bool control = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (!shift && (settings_.textCtrlEnter ? control : !control)) {
            args.Handled(true);
            Send();
        }
    });
    input.Paste([this](auto const&, TextControlPasteEventArgs const& args) {
        try {
            auto content = Clipboard::GetContent();
            if (content.Contains(StandardDataFormats::Bitmap()) ||
                content.Contains(StandardDataFormats::StorageItems())) {
                args.Handled(true);
                PasteImages();
            }
        } catch (...) {
            Error(L"无法读取剪贴板。");
        }
    });
    ui_->Get<Button>(L"SendText").Click([this](auto const&, auto const&) { Send(); });
    ui_->Get<Button>(L"RetryText").Click([this](auto const&, auto const&) { Send(true); });
    ui_->Get<Button>(L"StopText").Click([this](auto const&, auto const&) { Cancel(); });
    ui_->Get<Button>(L"AddImage").Click([this](auto const&, auto const&) {
        try {
            PickImages();
        } catch (...) {
            Error(L"无法打开图片选择器。");
        }
    });
    ui_->Get<Button>(L"ClearConversation").Click([this](auto const&, auto const&) {
        Cancel();
        turns_.clear();
        retry_.reset();
        ClearContext();
        status_.clear();
        RenderConversation();
    });
    auto page = ui_->Get<Grid>(L"TextPage");
    page.DragOver([this](auto const&, xaml::DragEventArgs const& args) {
        args.AcceptedOperation(!busy_ && !loading_ && !IsTextService(settings_.provider) &&
                                       images_.size() < 4 &&
                                       args.DataView().Contains(StandardDataFormats::StorageItems())
                                   ? DataPackageOperation::Copy
                                   : DataPackageOperation::None);
        args.Handled(true);
    });
    page.Drop([this](auto const&, xaml::DragEventArgs const& args) {
        args.Handled(true);
        [](TextTranslationPage* self, xaml::DragEventArgs args, std::weak_ptr<int> weak,
           unsigned long long id) -> winrt::fire_and_forget {
            auto deferral = args.GetDeferral();
            try {
                auto items = co_await args.DataView().GetStorageItemsAsync();
                if (!weak.expired() && id == self->generation_ && !self->busy_ && !self->loading_) {
                    std::vector<std::filesystem::path> paths;
                    for (auto const& item : items)
                        if (item.IsOfType(winrt::Windows::Storage::StorageItemTypes::File))
                            paths.emplace_back(item.Path().c_str());
                    self->ImportFiles(std::move(paths));
                }
            } catch (...) {
                if (!weak.expired())
                    self->Error(L"无法读取拖入的图片。");
            }
            deferral.Complete();
        }(this, args, lifetime_, generation_);
    });
    auto conversation = ui_->Get<StackPanel>(L"Conversation");
    conversation.SizeChanged([this](auto const& sender, auto const&) {
        auto panel = sender.template as<StackPanel>();
        for (auto const& child : panel.Children()) {
            auto row = child.as<xaml::FrameworkElement>();
            row.MaxWidth(
                std::max(180.0, panel.ActualWidth() * (winrt::unbox_value<bool>(row.Tag()) ? .79 : .84)));
        }
    });
    ui_->Get<Grid>(L"ComposerToolbar").SizeChanged([this](auto const& sender, auto const&) {
        ui_->Get<TextBlock>(L"SendHint")
            .Visibility(sender.template as<Grid>().ActualWidth() < 590 ? xaml::Visibility::Collapsed
                                                                       : xaml::Visibility::Visible);
    });
    ui_->Get<xaml::Controls::Primitives::Thumb>(L"ComposerResize")
        .DragDelta([this](auto const&, auto const& args) {
            ui_->composerHeight =
                std::clamp(ui_->Get<TextBox>(L"TextInput").MinHeight() + args.VerticalChange(), 88.0,
                           ui_->compact ? 140.0 : 240.0);
            ui_->Get<TextBox>(L"TextInput").MinHeight(ui_->composerHeight);
        });
    RenderConversation();
    RenderAttachments();
    Refresh();
}
void TextTranslationPage::AdaptLayout(bool compact) {
    if (!ui_)
        return;
    ui_->compact = compact;
    ui_->Get<TextBox>(L"TextInput")
        .MinHeight(compact ? std::min(88.0, ui_->composerHeight) : ui_->composerHeight);
}
void TextTranslationPage::Detach() {
    if (ui_)
        draft_ = Utf8(std::wstring(ui_->Get<TextBox>(L"TextInput").Text()));
    lifetime_.reset();
    Cancel();
    {
        std::lock_guard lock(queueMutex_);
        owner_ = nullptr;
        queue_.clear();
    }
    ui_.reset();
    targetChanged_ = {};
    composing_ = false;
    if (!key_.empty())
        SecureZeroMemory(key_.data(), key_.size());
    key_.clear();
}
bool TextTranslationPage::BackgroundIdle() {
    return worker_.Idle() && images_.empty() && (!retry_ || retry_->images.empty());
}
void TextTranslationPage::TrimSession() {
    auto bytes = [&] {
        size_t total = 0;
        for (const auto& turn : turns_) {
            total += Json(turn.source).dump().size() + Json(turn.result).dump().size() + 256;
            for (const auto& image : turn.images)
                total += Json(image.name).dump().size() + image.thumbnail.size() + 64;
        }
        return total;
    };
    while (turns_.size() > 1 && (turns_.size() > 20 || bytes() > 640 * 1024)) {
        turns_.erase(turns_.begin());
        if (contextStart_)
            --contextStart_;
    }
}
std::string TextTranslationPage::Snapshot() {
    if (ui_)
        draft_ = Utf8(std::wstring(ui_->Get<TextBox>(L"TextInput").Text()));
    TrimSession();
    Json value{{"target", target_},
               {"draft", draft_},
               {"contextStart", contextStart_},
               {"expires", contextExpires_},
               {"turns", Json::array()}};
    value["connection"] = {settings_.provider, Utf8(settings_.baseUrl), settings_.model,
                           settings_.requestParameters, settings_.serviceRegion};
    for (const auto& turn : turns_) {
        Json entry{{"source", turn.source},
                   {"result", turn.result},
                   {"target", turn.target},
                   {"images", Json::array()}};
        for (const auto& image : turn.images)
            entry["images"].push_back({{"name", image.name}, {"thumbnail", image.thumbnail}});
        value["turns"].push_back(std::move(entry));
    }
    return value.dump();
}
void TextTranslationPage::Restore(const std::string& state) {
    if (state.size() > 1024 * 1024)
        throw AppError("idle", "文本翻译会话超过大小限制。");
    auto value = Json::parse(state);
    auto target = value.at("target").get<std::string>();
    (void)FindTextTarget(target);
    auto draft = value.at("draft").get<std::string>();
    if (draft.size() > 64 * 1024 || value.at("turns").size() > 20)
        throw AppError("idle", "文本翻译会话无效。");
    std::vector<Turn> turns;
    for (const auto& entry : value.at("turns")) {
        Turn turn{entry.at("source").get<std::string>(),
                  entry.at("result").get<std::string>(),
                  entry.at("target").get<std::string>(),
                  {}};
        (void)FindTextTarget(turn.target);
        if (turn.source.size() > 64 * 1024 || turn.result.size() > 128 * 1024 ||
            entry.at("images").size() > 4)
            throw AppError("idle", "文本翻译会话无效。");
        (void)Wide(turn.source);
        (void)Wide(turn.result);
        for (const auto& image : entry.at("images")) {
            TranslationAttachment attachment{
                image.at("name").get<std::string>(), {}, image.at("thumbnail").get<std::string>()};
            if (attachment.name.size() > 4096 || attachment.thumbnail.size() > 128 * 1024)
                throw AppError("idle", "文本翻译图片预览无效。");
            turn.images.push_back(std::move(attachment));
        }
        turns.push_back(std::move(turn));
    }
    target_ = std::move(target);
    draft_ = std::move(draft);
    turns_ = std::move(turns);
    contextStart_ = std::min(turns_.size(), value.value("contextStart", size_t{}));
    contextExpires_ = value.value("expires", ULONGLONG{});
    auto now = GetTickCount64();
    if (contextExpires_ <= now || contextExpires_ - now > 30 * 60 * 1000)
        ClearContext();
    if (value.contains("connection")) {
        const auto& connection = value.at("connection");
        settings_.provider = connection.at(0).get<std::string>();
        settings_.baseUrl = Wide(connection.at(1).get<std::string>());
        settings_.model = connection.at(2).get<std::string>();
        settings_.requestParameters = connection.at(3).get<std::string>();
        settings_.serviceRegion = connection.at(4).get<std::string>();
    }
    initialized_ = true;
    if (!turns_.empty() && turns_.back().result.empty() && turns_.back().images.empty())
        retry_ = ContentTranslationRequest{turns_.back().source, turns_.back().target};
}
} // namespace sat
