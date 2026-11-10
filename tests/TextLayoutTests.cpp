#include "capture/TextDetection.h"
#include "capture/BundledOcr.h"
#include "app/SourceMatching.h"
#include "overlay/SpatialLayout.h"
#include "overlay/TextRim.h"
#include "providers/Provider.h"
#include <wincodec.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <algorithm>
using namespace sat;
namespace {
void Require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
Image ReadImage(const wchar_t* path) {
    ComPtr<IWICImagingFactory> f;
    CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)),
            "test");
    ComPtr<IWICBitmapDecoder> d;
    CheckHR(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &d),
            "test");
    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHR(d->GetFrame(0, &frame), "test");
    ComPtr<IWICFormatConverter> c;
    CheckHR(f->CreateFormatConverter(&c), "test");
    CheckHR(c->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                          WICBitmapPaletteTypeCustom),
            "test");
    UINT w{}, h{};
    c->GetSize(&w, &h);
    Image image{int(w), int(h), std::vector<unsigned char>(size_t(w) * h * 4)};
    CheckHR(c->CopyPixels(nullptr, w * 4, UINT(image.bgra.size()), image.bgra.data()), "test");
    return image;
}
void Render(const Image& image, const std::vector<PositionedText>& items, const std::filesystem::path& path,
            bool diagnostic, const Settings& settings = {}) {
    ComPtr<IWICImagingFactory> wic;
    CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)),
            "test");
    ComPtr<IWICBitmap> bitmap;
    CheckHR(wic->CreateBitmap(image.width, image.height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                              &bitmap),
            "test");
    ComPtr<ID2D1Factory> factory;
    CheckHR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()), "test");
    ComPtr<ID2D1RenderTarget> target;
    CheckHR(factory->CreateWicBitmapRenderTarget(
                bitmap.Get(),
                D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
                &target),
            "test");
    ComPtr<ID2D1Bitmap> background;
    CheckHR(target->CreateBitmap(
                D2D1::SizeU(image.width, image.height), image.bgra.data(), image.width * 4,
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
                &background),
            "test");
    target->BeginDraw();
    target->DrawBitmap(background.Get());
    DrawPositioned(target.Get(), items, &image, RGB(20, 20, 20), settings, 96, diagnostic);
    CheckHR(target->EndDraw(), "test");
    ComPtr<IWICStream> stream;
    CheckHR(wic->CreateStream(&stream), "test");
    CheckHR(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "test");
    ComPtr<IWICBitmapEncoder> encoder;
    CheckHR(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "test");
    CheckHR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "test");
    ComPtr<IWICBitmapFrameEncode> frame;
    CheckHR(encoder->CreateNewFrame(&frame, nullptr), "test");
    CheckHR(frame->Initialize(nullptr), "test");
    CheckHR(frame->WriteSource(bitmap.Get(), nullptr), "test");
    CheckHR(frame->Commit(), "test");
    CheckHR(encoder->Commit(), "test");
    // All pixels outside the translated regions must preserve the source, including separators and omitted
    // text.
    if (!diagnostic) {
        std::vector<unsigned char> bytes(image.bgra.size());
        CheckHR(bitmap->CopyPixels(nullptr, image.width * 4, UINT(bytes.size()), bytes.data()), "test");
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x) {
                POINT p{x, y};
                bool covered = false;
                for (const auto& item : items) {
                    // The rebuilt surface repairs a ring around each box, so the reach follows the repair
                    // margin instead of the bare box.
                    RECT reach = item.rect;
                    InflateRect(&reach, 24, 24);
                    if (!item.suppressed && PtInRect(&reach, p))
                        covered = true;
                }
                if (covered)
                    continue;
                size_t pos = (size_t(y) * image.width + x) * 4;
                for (int c = 0; c < 3; ++c)
                    Require(std::abs(int(bytes[pos + c]) - int(image.bgra[pos + c])) <= 1,
                            "source pixel outside translation changed");
            }
    }
}
// Renders the three rim styles on a dark half and a light half: the inverse mode has to stay
// readable in both directions. One row per style, and the timing loop measures each style through
// the real end-draw path so the stacked strokes show their cost.
void RenderRims(const std::filesystem::path& output) {
    ComPtr<IWICImagingFactory> wic;
    CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)),
            "test");
    ComPtr<ID2D1Factory> factory;
    CheckHR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()), "test");
    ComPtr<IDWriteFactory> write;
    CheckHR(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(write.GetAddressOf())),
            "test");
    ComPtr<IDWriteTextFormat> format;
    CheckHR(write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 38.f, L"zh-CN",
                                    &format),
            "test");
    CheckHR(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "test");
    const wchar_t* sample = L"屏幕翻译 Overlay 42";
    ComPtr<IDWriteTextLayout> layout;
    CheckHR(write->CreateTextLayout(sample, UINT32(wcslen(sample)), format.Get(), 460.f, 60.f, &layout), "test");
    const int w = 960, h = 330;
    const auto solid = [](COLORREF c, float a = 1.f) {
        return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a);
    };
    auto paint = [&](int style, const std::filesystem::path* path) {
        ComPtr<IWICBitmap> bitmap;
        CheckHR(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap), "test");
        ComPtr<ID2D1RenderTarget> target;
        CheckHR(factory->CreateWicBitmapRenderTarget(
                    bitmap.Get(),
                    D2D1::RenderTargetProperties(
                        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
                    &target),
                "test");
        ComPtr<ID2D1SolidColorBrush> fill, rim, light;
        CheckHR(target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &fill), "test");
        CheckHR(target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &rim), "test");
        CheckHR(target->CreateSolidColorBrush(solid(RGB(184, 188, 194)), &light), "test");
        target->BeginDraw();
        target->Clear(solid(RGB(74, 79, 87)));
        target->FillRectangle(D2D1::RectF(w / 2.f, 0, float(w), float(h)), light.Get());
        // Three rows: the default rim width in inverse mode, a wide rim in inverse mode, and the
        // custom colour mode, which carries a chosen hue. Both halves are mid grey so a black rim on
        // the dark half and a white rim on the light half stay visible.
        struct Row {
            float y;
            float stroke;
            bool inverse;
        };
        const Row rows[3] = {{26.f, 1.5f, true}, {118.f, 3.f, true}, {216.f, 3.f, false}};
        for (const auto& row : rows) {
            fill->SetColor(solid(RGB(245, 245, 245)));
            rim->SetColor(row.inverse ? solid(RGB(16, 16, 16)) : solid(RGB(255, 59, 48)));
            auto dark =
                CreateRimRenderer(target.Get(), factory.Get(), fill.Get(), rim.Get(), row.stroke, style, 1.f);
            CheckHR(layout->Draw(nullptr, dark.Get(), 26.f, row.y), "test");
            fill->SetColor(solid(RGB(18, 18, 20)));
            rim->SetColor(row.inverse ? solid(RGB(250, 250, 250)) : solid(RGB(10, 132, 255)));
            auto bright =
                CreateRimRenderer(target.Get(), factory.Get(), fill.Get(), rim.Get(), row.stroke, style, 1.f);
            CheckHR(layout->Draw(nullptr, bright.Get(), w / 2.f + 26.f, row.y), "test");
        }
        CheckHR(target->EndDraw(), "test");
        if (!path)
            return;
        ComPtr<IWICStream> stream;
        CheckHR(wic->CreateStream(&stream), "test");
        CheckHR(stream->InitializeFromFilename(path->c_str(), GENERIC_WRITE), "test");
        ComPtr<IWICBitmapEncoder> encoder;
        CheckHR(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "test");
        CheckHR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "test");
        ComPtr<IWICBitmapFrameEncode> frame;
        CheckHR(encoder->CreateNewFrame(&frame, nullptr), "test");
        CheckHR(frame->Initialize(nullptr), "test");
        CheckHR(frame->WriteSource(bitmap.Get(), nullptr), "test");
        CheckHR(frame->Commit(), "test");
        CheckHR(encoder->Commit(), "test");
    };
    for (int style = 0; style < 3; ++style) {
        const auto path = output / (std::wstring(L"rim-") + wchar_t(L'0' + style) + L".png");
        paint(style, &path);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i)
            paint(style, nullptr);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 20;
        std::cout << "rim style " << style << ": " << ms << " ms per six-run page\n";
    }
    std::cout << "Rim samples written; styles 0/1/2 are hard, rough, glow\n";
}
void AppearanceRegression() {
    Image image{700, 300, std::vector<unsigned char>(700 * 300 * 4, 255)};
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = image.width;
    info.bmiHeader.biHeight = -image.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits{};
    auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    Require(bitmap && bits, "fixture bitmap unavailable");
    auto old = SelectObject(dc, bitmap);
    memcpy(bits, image.bgra.data(), image.bgra.size());
    SetBkMode(dc, TRANSPARENT);
    auto draw = [&](int x, int y, int size, COLORREF color, const wchar_t* text) {
        auto font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                                ANTIALIASED_QUALITY, 0, L"Segoe UI");
        auto previous = SelectObject(dc, font);
        SetTextColor(dc, color);
        TextOutW(dc, x, y, text, int(wcslen(text)));
        SelectObject(dc, previous);
        DeleteObject(font);
    };
    draw(40, 35, 44, RGB(20, 70, 180), L"Original blue heading");
    draw(40, 140, 24, RGB(180, 35, 45), L"First line of the red paragraph");
    draw(40, 175, 24, RGB(180, 35, 45), L"Second line with the same font size");
    draw(40, 245, 20, RGB(150, 150, 150), L"Subtle gray caption");
    GdiFlush();
    memcpy(image.bgra.data(), bits, image.bgra.size());
    for (size_t i = 3; i < image.bgra.size(); i += 4)
        image.bgra[i] = 255;
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    TranslationResult result{"en",
                             {{"Original blue heading", "蓝色标题", {20, 60, 950, 270}},
                              {"First line of the red paragraph\nSecond line with the same font size",
                               "两行正文的字号由实际字形高度确定。",
                               {20, 430, 950, 270}},
                              {"Subtle gray caption", "灰色图注", {20, 780, 600, 160}}}};
    Settings settings;
    auto layout = LayoutPositioned(result, image, settings, 96);
    const COLORREF expected[]{RGB(20, 70, 180), RGB(180, 35, 45), RGB(150, 150, 150)};
    for (size_t i = 0; i < layout.size(); ++i) {
        Require(layout[i].sourceColor.has_value(), "original color not measured");
        auto actual = *layout[i].sourceColor;
        Require(std::abs(int(GetRValue(actual)) - int(GetRValue(expected[i]))) <= 8 &&
                    std::abs(int(GetGValue(actual)) - int(GetGValue(expected[i]))) <= 8 &&
                    std::abs(int(GetBValue(actual)) - int(GetBValue(expected[i]))) <= 8,
                "foreground color differs from glyph core");
        Require(!layout[i].suppressed, "AI fallback fixture suppressed reliable text");
    }
    Require(layout[0].fontSize > 26 && layout[0].sourceRect.top == 18 && Height(layout[0].sourceRect) == 81,
            "AI heading changed its model coordinates or lost measured glyph font size");
    Require(layout[1].detectedLines.size() == 2 && layout[1].fontSize < 28,
            "AI paragraph height inflated the font");
    auto scaled = LayoutPositioned(result, image, settings, 192);
    Require(std::abs(scaled[0].fontSize - layout[0].fontSize) < 1,
            "physical glyph height changed with monitor DPI");
    settings.matchTextColor = false;
    Require(!LayoutPositioned(result, image, settings, 96)[0].sourceColor, "manual color override ignored");
    std::filesystem::create_directories("test-output/appearance");
    Render(image, {}, "test-output/appearance/original.png", false);
    Render(image, layout, "test-output/appearance/translated.png", false);
}
void ChatBubbleRegression() {
    const auto image = ReadImage(L"test-fixtures/user/chat-bubbles-en.png");
    TranslationResult result{"en"};
    auto add = [&](const char* original, const char* translated, RECT rect) {
        Segment segment;
        segment.original = original;
        segment.translated = translated;
        segment.sourcePixels = rect;
        result.segments.push_back(std::move(segment));
    };
    add("Tibo", "Tibo", {108, 34, 168, 65});
    add("Joined August 2025", "2025年8月加入", {478, 178, 690, 204});
    add("928.8k Followers", "92.88万粉丝", {493, 215, 676, 241});
    add("View Profile", "查看个人资料", {520, 276, 650, 302});
    add("Tibo · Oct 24", "Tibo · 10月24日", {614, 377, 769, 403});
    add("DM if you want to help scale dots to 1B users and have done similar things in the past",
        "如果你能帮助将 dots 扩展到10亿用户，且有相关经验，请私信我。", {570, 418, 1140, 477});
    add("i can help you!", "我可以帮你！", {977, 505, 1137, 540});
    add("Just Chat", "只是聊天", {810, 759, 1029, 806});
    add("Cast Sydney Sweeney for a ad, thank me later!!", "找西德尼·斯威尼拍广告，回头谢我！",
        {573, 991, 1082, 1023});
    add("1:20 AM", "凌晨1:20", {1043, 1034, 1110, 1053});
    add("you are genius, what if we casted you instead of her?", "你真是个天才，如果选你来代替她呢？",
        {47, 1090, 604, 1124});
    add("1:28 AM", "凌晨1:28", {542, 1132, 605, 1153});
    const std::filesystem::path output = "test-output/chat-bubbles";
    std::filesystem::create_directories(output);
    auto layout = LayoutPositioned(result, image, Settings{}, 96);
    for (size_t i : {size_t(6), size_t(8), size_t(9)}) {
        const auto& item = layout[i];
        Require(!item.suppressed && !item.overflow, "translated blue message is hidden or clipped");
        Require(GetBValue(item.background) > 210 && GetRValue(item.background) < 115 &&
                    GetGValue(item.background) > 140 && GetGValue(item.background) < 170,
                "blue message surface replaced by a white mask");
        Require(item.sourceColor && GetRValue(*item.sourceColor) > 210,
                "white source glyphs were not detected on the blue bubble");
    }
    Require(GetRValue(layout[1].background) >= 250 && GetGValue(layout[1].background) >= 250 &&
                std::abs(int(GetRValue(layout[1].background)) - int(GetBValue(layout[1].background))) <= 2,
            "white page acquired a tinted translation mask");
    Render(image, layout, output / L"translated.png", false);
    // Exercise the draw path itself: sampled white text must also receive contrast correction
    // when a complex surface or a caller supplies a light replacement background.
    auto fallback = layout;
    fallback[6].background = RGB(247, 249, 252);
    fallback[6].sourceColor = RGB(255, 255, 255);
    Render(image, fallback, output / L"contrast-fallback.png", false);
    const auto rendered = ReadImage((output / L"contrast-fallback.png").c_str());
    int darkPixels = 0;
    for (int y = fallback[6].rect.top; y < fallback[6].rect.bottom; ++y)
        for (int x = fallback[6].rect.left; x < fallback[6].rect.right; ++x) {
            const auto* p = &rendered.bgra[(size_t(y) * rendered.width + x) * 4];
            darkPixels += p[0] < 80 && p[1] < 80 && p[2] < 80;
        }
    Require(darkPixels > 30, "sampled white glyphs bypassed contrast correction during rendering");
    auto vision = result;
    for (auto& s : vision.segments) {
        const auto r = *s.sourcePixels;
        const int x = MulDiv(r.left, 1000, image.width), y = MulDiv(r.top, 1000, image.height);
        s.box = {x, y, MulDiv(r.right, 1000, image.width) - x,
                 MulDiv(r.bottom, 1000, image.height) - y};
        s.sourcePixels.reset();
    }
    auto visionLayout = LayoutPositioned(vision, image, Settings{}, 96);
    Require(GetBValue(visionLayout[6].background) > 210 && GetRValue(visionLayout[6].background) < 115,
            "AI coordinates lost the blue message background");
    Render(image, visionLayout, output / L"vision-translated.png", false);
    Settings manual;
    manual.matchTextColor = false;
    auto manualLayout = LayoutPositioned(result, image, manual, 192);
    Require(!manualLayout[6].sourceColor && !manualLayout[6].overflow,
            "manual text color or DPI scaling broke a blue message");
    Render(image, manualLayout, output / L"manual-color-192dpi.png", false);
    std::cout << "Chat bubble appearance: PASS (real screenshot, blue surfaces, rendered contrast, AI boxes, DPI)\n";
}
void UnitTests() {
    Image image{900, 700, std::vector<unsigned char>(900 * 700 * 4, 255)};
    {
        TranslationResult result{"en", {{"Text", "译文", {100, 100, 300, 100}}}};
        Settings settings;
        settings.spatialOverlay = false;
        auto blocks =
            BuildOverlay(result, image, {0, 0, 900, 700}, {L"test", {0, 0, 900, 700}, 96}, settings);
        Require(blocks.size() == 1 && blocks[0].image && blocks[0].sourceResult &&
                    blocks[0].positioned.empty() && blocks[0].text == L"译文",
                "non-spatial view lost original image, copy text or deferred layout");
        Require(!BuildOverlay(result, image, {0, 0, 900, 700}, {L"test", {0, 0, 900, 700}, 96}, settings,
                              false)[0]
                     .image,
                "notice must not create a translation toolbar");
    }
    auto confidence =
        GroupTextLines({{{40, 100, 820, 122}, "first", .98f}, {{40, 140, 820, 162}, "second", .78f}}, image);
    Require(confidence.size() == 1 && confidence[0].confidence == .78f && !CanTranslateText(confidence),
            "paragraph merging lost the weakest OCR confidence");
    auto inlineConfidence =
        GroupTextLines({{{133, 661, 167, 690}, "1", .74f}, {{153, 662, 346, 688}, "List text", .99f}}, image);
    Require(inlineConfidence.size() == 1 && inlineConfidence[0].confidence == .74f,
            "inline merging lost the weakest OCR confidence");
    auto regions = GroupTextLines({{{40, 100, 820, 122}, "line one"},
                                   {{20, 140, 820, 162}, "line two"},
                                   {{40, 210, 800, 232}, "separate passage"},
                                   {{40, 300, 400, 322}, "left column"},
                                   {{490, 300, 850, 322}, "right column"}},
                                  image);
    Require(regions.size() == 4 && regions[0].lines.size() == 2,
            "paragraph grouping or column isolation failed");
    Require(regions[0].lineHeight == 22, "paragraph height used as font height");
    auto marker = GroupTextLines({{{133, 661, 167, 690}, "1"}, {{153, 662, 346, 688}, "List text"}}, image);
    Require(marker.size() == 1 && marker[0].text == "1 List text",
            "overlapping inline marker was not joined");
    auto separate = GroupTextLines(
        {{{40, 100, 400, 122}, "heading"}, {{430, 100, 470, 122}, "badge"}, {{40, 140, 820, 162}, "body"}},
        image);
    Require(separate.size() == 3, "paragraph union enclosed an independent badge");
    for (int x = 20; x < 820; ++x) {
        auto p = (size_t(132) * image.width + x) * 4;
        image.bgra[p] = image.bgra[p + 1] = image.bgra[p + 2] = 220;
    }
    Require(GroupTextLines({{{20, 100, 820, 122}, "above"}, {{20, 140, 820, 162}, "below"}}, image).size() ==
                2,
            "horizontal separator was crossed");
    TranslationResult result{"ja", {{"source paragraph", "这是一段位于原位置的译文。", {0, 0, 1, 1}}}};
    result.segments[0].sourcePixels = RECT{40, 260, 850, 475};
    result.segments[0].sourceLineHeight = 22;
    result.segments[0].sourceLines = {{40, 260, 850, 282}, {40, 300, 850, 322}, {40, 340, 850, 362},
                                      {40, 380, 850, 402}, {40, 420, 850, 442}, {40, 453, 600, 475}};
    result.protectedRegions = {{40, 180, 800, 202}, {40, 520, 850, 580}};
    Settings settings;
    for (UINT dpi : {96u, 144u, 192u}) {
        auto layout = LayoutPositioned(result, image, settings, dpi);
        const auto& a = layout[0];
        Require(a.sourceRect.top == 260 && a.sourceRect.bottom == 475, "DPI altered physical OCR bounds");
        Require(a.rect.top >= 240 && a.rect.bottom <= 490 && !a.suppressed,
                "paragraph escaped its anchored area");
        Require(a.fontSize <= 24.1f, "multiline paragraph inflated font");
        auto longer = result;
        for (int i = 0; i < 100; ++i)
            longer.segments[0].translated += "完整内容必须保留。";
        auto overflow = LayoutPositioned(longer, image, settings, dpi);
        Require(overflow[0].overflow && EqualRect(&a.rect, &overflow[0].rect),
                "long text moved its source region");
        Require(overflow[0].fontSize >= std::min(a.fontSize, 12 * dpi / 96.f),
                "font fell below readability floor");
    }
    settings.autoFont = false;
    settings.fontSize = 70;
    auto fixed = LayoutPositioned(result, image, settings, 96);
    Require(fixed[0].fontSize == 70, "fixed font changed");
    {
        auto headline = result;
        headline.protectedRegions.clear();
        auto& s = headline.segments[0];
        s.sourcePixels = RECT{40, 100, 301, 156};
        s.sourceLines = {{40, 100, 301, 125}, {40, 127, 239, 156}};
        s.sourceLineHeight = 28;
        s.translated = "北爱尔兰事务大臣将\n禁止德鲁姆克里\n游行";
        Settings automatic;
        auto wrapped = LayoutPositioned(headline, image, automatic, 96);
        s.translated = "北爱尔兰事务大臣将禁止德鲁姆克里游行";
        auto continuous = LayoutPositioned(headline, image, automatic, 96);
        Require(!wrapped[0].overflow && wrapped[0].contentHeight == continuous[0].contentHeight,
                "model hard wraps clipped a short translated title");
    }
    auto duplicate = result;
    duplicate.segments.push_back(duplicate.segments[0]);
    auto ambiguous = LayoutPositioned(duplicate, image, settings, 96);
    Require(ambiguous[0].suppressed && ambiguous[1].suppressed,
            "overlapping regions erased neighboring text");
    result.segments[0].sourcePixels.reset();
    result.segments[0].sourceLineHeight = 0;
    result.segments[0].box = {100, 370, 800, 300};
    settings.autoFont = true;
    auto fallback = LayoutPositioned(result, image, settings, 96);
    Require(!fallback[0].suppressed && fallback[0].layout && fallback[0].sourceRect.top == 259 &&
                fallback[0].sourceRect.bottom == 469,
            "valid AI coordinates were replaced or rejected by pixel analysis");
    std::stop_source stop;
    stop.request_stop();
    bool cancelled = false;
    try {
        DetectText(image, stop.get_token());
    } catch (const Cancelled&) {
        cancelled = true;
    }
    Require(cancelled, "OCR ignored cancellation");
    Require(DetectText(Image{}).status == "invalid-image", "invalid image not rejected");
}
void ScreenshotRegression() {
    auto image = ReadImage(L"test-fixtures/news-ja.png");
    auto detected = DetectText(image, {}, OcrJapanese);
    if (detected.regions.empty()) {
        RecognizeBundled(image, OcrJapanese, {});
        throw std::runtime_error("Bundled screenshot OCR must not be skipped");
    }
    const auto main = std::find_if(detected.regions.begin(), detected.regions.end(),
                                   [](const TextRegion& r) { return r.rect.top >= 250 && r.rect.top < 270; });
    Require(main != detected.regions.end() && main->lines.size() == 6 && main->rect.bottom >= 473 &&
                main->rect.bottom < 480,
            "news paragraph geometry was not recovered");
    const auto prize =
        std::find_if(detected.regions.begin(), detected.regions.end(),
                     [](const TextRegion& r) { return r.rect.top >= 510 && r.rect.top < 530; });
    Require(prize != detected.regions.end() && prize->lines.size() == 2 && prize->rect.right >= 899,
            "terminal Japanese glyph remains outside source mask");
    TranslationResult result;
    result.protectedRegions.reserve(detected.regions.size());
    for (const auto& region : detected.regions)
        result.protectedRegions.push_back(region.rect);
    Segment s;
    s.original = main->text;
    s.translated = "正文译文应保留在原段落内。";
    s.sourcePixels = main->rect;
    s.sourceLines = main->lines;
    s.sourceLineHeight = main->lineHeight;
    result.segments.push_back(s);
    Settings settings;
    auto layout = LayoutPositioned(result, image, settings, 96);
    Require(!layout[0].suppressed && layout[0].rect.top > 240 && layout[0].rect.bottom < 500,
            "news paragraph overlaps a link or the prize paragraph");
    std::filesystem::create_directories("test-output/news-regression");
    Render(image, layout, "test-output/news-regression/partial.png", false);
    std::cout << "Screenshot OCR regression: PASS (six-line paragraph, terminal glyph, unchanged surrounding "
                 "pixels)\n";
}
void UserScreenshotRegression() {
    for (const auto* name : {L"bbc-headlines-en", L"asahi-article-ja", L"bbc-article-en",
                             L"asahi-election-ja", L"asahi-grid-ja"}) {
        auto image = ReadImage(
            (std::filesystem::path(L"test-fixtures/user") / (std::wstring(name) + L".png")).c_str());
        auto detected = DetectText(image, {}, OcrJapanese);
        Require(detected.status == "bundled-ocr" && detected.regions.size() >= 15,
                "real screenshot OCR coverage regressed");
        nlohmann::json translations = nlohmann::json::object();
        if (std::wstring(name) == L"bbc-headlines-en") {
            std::ifstream file("test-fixtures/user/bbc-headlines-en-translations.json");
            file >> translations;
        }
        TranslationResult result;
        for (const auto& r : detected.regions) {
            Segment s;
            s.original = r.text;
            s.translated = translations.value(std::to_string(r.id), r.text.empty() ? "未识别区域" : r.text);
            s.sourceId = r.id;
            s.sourcePixels = r.rect;
            s.sourceLines = r.lines;
            s.sourceLineHeight = r.lineHeight;
            result.segments.push_back(s);
            result.protectedRegions.push_back(r.rect);
        }
        auto items = LayoutPositioned(result, image, Settings{}, 96);
        size_t suppressed = 0;
        for (const auto& item : items)
            suppressed += item.suppressed;
        if (std::wstring(name) == L"bbc-headlines-en") {
            size_t overflow = 0;
            for (const auto& item : items)
                overflow += item.overflow;
            std::cout << "BBC detail: regions=" << detected.regions.size()
                      << " translations=" << translations.size() << " overflow=" << overflow
                      << " suppressed=" << suppressed << '\n';
            for (size_t k = 0; k < items.size(); ++k)
                if (items[k].overflow)
                    std::cout << "  overflow[" << k << "] font=" << items[k].fontSize
                              << " band=" << items[k].bandHeight << " rect=(" << items[k].rect.left << ","
                              << items[k].rect.top << "," << items[k].rect.right << "," << items[k].rect.bottom
                              << ")\n";
            Require(suppressed == 0, "BBC headlines or category labels hidden by geometry conflicts");
            // The fixture maps translations by region id, so the invariant is that every recognized
            // region receives a translation, not that the detector keeps one fixed region count: the
            // row merge deliberately fuses fragments of one caption into a single passage.
            Require(std::all_of(detected.regions.begin(), detected.regions.end(),
                                [&](const TextRegion& r) {
                                    return translations.contains(std::to_string(r.id));
                                }) &&
                        std::none_of(items.begin(), items.end(), [](const auto& item) { return item.overflow; }),
                    "saved real BBC translations overflow or lose region mapping");
            for (const auto* anchor : {"Trump announces deal", "curb fuel prices", "Drumcree parade",
                                       "Hurricane Isaias", "cockroach", "watch it", "election defeat",
                                       "Australian surfer", "Northern Ireland", "Business"}) {
                auto found =
                    std::find_if(detected.regions.begin(), detected.regions.end(),
                                 [&](const auto& r) { return r.text.find(anchor) != std::string::npos; });
                Require(found != detected.regions.end(), "BBC text anchor missing");
            }
            auto supplemented = result;
            auto headline =
                std::find_if(supplemented.segments.begin(), supplemented.segments.end(), [](const auto& s) {
                    return s.original.find("Trump announces deal") != std::string::npos;
                });
            const auto index = size_t(headline - supplemented.segments.begin());
            auto original = *headline->sourcePixels;
            std::erase_if(supplemented.protectedRegions, [&](RECT r) { return EqualRect(&r, &original); });
            headline->box = detected.regions[index].imageBox;
            headline->sourceId = 0;
            headline->sourcePixels.reset();
            headline->sourceLines.clear();
            headline->sourceLineHeight = 0;
            auto supplementedLayout = LayoutPositioned(supplemented, image, Settings{}, 96);
            Require(!supplementedLayout[index].suppressed && supplementedLayout[index].layout &&
                        supplemented.segments[index].translated == result.segments[index].translated,
                    "vision-supplemented real headline lost its translation or layout");
        }
        if (std::wstring(name) == L"bbc-article-en" || std::wstring(name) == L"asahi-grid-ja")
            Require(suppressed == 0, "dense article contains hidden paragraphs");
        auto output = std::filesystem::path(L"test-output/user-regression") / name;
        std::filesystem::create_directories(output);
        Render(image, items, output / L"layout.png", false);
        const auto* pixels = image.bgra.data();
        RECT area{0, 0, image.width, image.height};
        auto blocks =
            BuildOverlay(result, std::move(image), area, Monitor{L"offscreen", area, 96}, Settings{});
        Require(blocks.size() == 1 && blocks[0].image && blocks[0].image->bgra.data() == pixels,
                "overlay duplicated the captured pixel buffer");
        Require(blocks[0].positioned.size() == items.size() && !blocks[0].text.empty(),
                "moving the screenshot lost overlay views");
        std::weak_ptr<const Image> retained = blocks[0].image;
        blocks.clear();
        Require(retained.expired(), "closed overlay retained its screenshot");
        std::cout << "User fixture " << Utf8(name) << ": " << detected.regions.size() << " regions, "
                  << suppressed << " suppressed\n";
    }
}
void VerticalColumnRegression() {
    // A manga balloon arrives as neighbouring strips, one glyph per row. The replacement has to keep that
    // direction and reuse every strip, so the translated balloon covers the area the original columns
    // occupied instead of one strip squeezed by the whole sentence.
    auto image = ReadImage(L"test-fixtures/user/asahi-article-ja.png");
    constexpr int top = 120;
    const RECT rightStrip{200, top, 231, top + 160};
    const RECT leftStrip{166, top, 197, top + 120};
    TranslationResult result;
    Segment segment;
    segment.original = "それはちょっと";
    segment.translated = "这有点不太行吧真的";
    segment.sourcePixels = RECT{166, top, 231, top + 160};
    segment.sourceLines = {rightStrip, leftStrip};
    segment.sourceLineHeight = 160.f;
    result.segments.push_back(segment);
    auto layout = LayoutPositioned(result, image, Settings{}, 96);
    Require(layout.size() == 1 && !layout[0].suppressed, "vertical passage was dropped");
    const auto& item = layout[0];
    Require(item.columnTexts.size() == 2, "vertical passage did not reuse every source strip");
    Require(item.fontSize <= 31.f && item.fontSize >= 18.f, "vertical passage lost its glyph size");
    Require(!item.overflow, "vertical passage overflowed its balloon");
    UINT32 rows{};
    item.columnTexts.front().layout->GetLineMetrics(nullptr, 0, &rows);
    Require(rows >= 2, "vertical passage did not stack one glyph per row");
    // The first strip sits to the right, so it carries the start of the sentence.
    Require(item.columnTexts.front().rect.left > item.columnTexts.back().rect.left,
            "vertical strips lost their reading order");
    std::cout << "Vertical column: strips=" << item.columnTexts.size() << " font=" << item.fontSize
              << " rows=" << rows << '\n';
}
TranslationResult ReadRecordedResponse(const std::filesystem::path& path) {
    std::ifstream file(path);
    nlohmann::json report;
    file >> report;
    TranslationResult result;
    for (const auto& row : report.at("segments")) {
        Segment segment;
        segment.original = row.at("original_text").get<std::string>();
        segment.translated = row.at("translated_text").get<std::string>();
        const auto& box = row.at("bbox");
        segment.box = {box.at("x"), box.at("y"), box.at("width"), box.at("height")};
        auto rect = [](const auto& r) -> RECT { return {r.at(0), r.at(1), r.at(2), r.at(3)}; };
        if (row.contains("source_pixels"))
            segment.sourcePixels = rect(row.at("source_pixels"));
        for (const auto& r : row.at("source_lines"))
            segment.sourceLines.push_back(rect(r));
        segment.sourceLineHeight = row.at("source_line_height");
        segment.positionUnavailable = row.at("position_unavailable");
        segment.untranslated = row.at("untranslated");
        result.segments.push_back(std::move(segment));
    }
    if (report.contains("uncovered_lines"))
        for (const auto& r : report.at("uncovered_lines"))
            result.uncoveredLines.push_back(
                {r.at(0).get<LONG>(), r.at(1).get<LONG>(), r.at(2).get<LONG>(), r.at(3).get<LONG>()});
    return result;
}
void WriteRecordedResponse(const TranslationResult& result, const std::filesystem::path& path) {
    nlohmann::json report;
    report["segments"] = nlohmann::json::array();
    auto rect = [](RECT r) { return nlohmann::json::array({r.left, r.top, r.right, r.bottom}); };
    for (const auto& segment : result.segments) {
        nlohmann::json row;
        row["original_text"] = segment.original;
        row["translated_text"] = segment.translated;
        row["bbox"] = {{"x", segment.box.x},
                       {"y", segment.box.y},
                       {"width", segment.box.width},
                       {"height", segment.box.height}};
        if (segment.sourcePixels)
            row["source_pixels"] = rect(*segment.sourcePixels);
        row["source_lines"] = nlohmann::json::array();
        for (auto line : segment.sourceLines)
            row["source_lines"].push_back(rect(line));
        row["source_line_height"] = segment.sourceLineHeight;
        row["position_unavailable"] = segment.positionUnavailable;
        row["untranslated"] = segment.untranslated;
        report["segments"].push_back(std::move(row));
    }
    report["uncovered_lines"] = nlohmann::json::array();
    for (auto line : result.uncoveredLines)
        report["uncovered_lines"].push_back(rect(line));
    std::ofstream file(path);
    file << report.dump(2);
}
void HandshakeRegression(const char* responsePath) {    std::ifstream file(responsePath);
    const std::string response(std::istreambuf_iterator<char>(file), {});
    auto result = ParseTranslation(response);
    auto image = ReadImage(L"test-fixtures/user/handshake-error-en.png");
    auto layout = LayoutPositioned(result, image, Settings{}, 96);
    Require(layout.size() == 1 && !layout[0].suppressed && layout[0].layout && layout[0].rect.top <= 31 &&
                layout[0].rect.bottom >= 140,
            "AI single-line hint failed to cover the complete three-line error paragraph");
    std::filesystem::create_directories("test-output/handshake-regression");
    Render(image, layout, "test-output/handshake-regression/translated.png", false);
    result.segments[0].untranslated = true;
    auto untranslated = LayoutPositioned(result, image, Settings{}, 96);
    Require(untranslated[0].suppressed, "untranslated prose painted over original content");
}
void GameDialogRegression() {
    auto result = ReadRecordedResponse("test-fixtures/user/game-data-update-ocr-response.json");
    auto image = ReadImage(L"test-fixtures/user/game-data-update-ja.png");
    auto layout = LayoutPositioned(result, image, Settings{}, 96);
    Require(layout.size() == 7 && !layout[1].suppressed && !layout[1].overflow,
            "real game dialog lost its translated notice");
    UINT32 count{};
    layout[1].layout->GetLineMetrics(nullptr, 0, &count);
    std::vector<DWRITE_LINE_METRICS> lines(count);
    CheckHR(layout[1].layout->GetLineMetrics(lines.data(), count, &count), "test");
    Require(count == 3 && lines[0].newlineLength && lines[1].newlineLength,
            "notice, size field and Wi-Fi instruction lost their semantic line breaks");
    std::filesystem::create_directories("test-output/game-dialog-regression");
    Render(image, layout, "test-output/game-dialog-regression/translated.png", false);
}
void DirectVisionPlacementRegression(const char* responsePath) {
    auto result = ReadRecordedResponse(responsePath);
    auto image = ReadImage(L"test-fixtures/user/github-dashboard-afternoon-en.png");
    auto layout = LayoutPositioned(result, image, Settings{}, 96);
    Require(std::none_of(layout.begin(), layout.end(), [](const auto& item) { return item.suppressed; }),
            "valid AI boxes triggered an automatic whole-page reading fallback");
    for (size_t i = 0; i < layout.size(); ++i) {
        const auto& box = result.segments[i].box;
        Require(layout[i].sourceRect.left == MulDiv(box.x, image.width, 1000) &&
                    layout[i].sourceRect.top == MulDiv(box.y, image.height, 1000),
                "pixel inspection moved a model-supplied patch onto another control");
    }
    RECT area{0, 0, image.width, image.height};
    auto blocks = BuildOverlay(result, std::move(image), area, {L"offscreen", area, 96}, Settings{});
    for (const auto& segment : result.segments)
        Require(blocks[0].text.find(Wide(segment.translated)) != std::wstring::npos,
                "AI position fallback discarded a completed translation");
}
class ReadingGlyphRenderer final : public IDWriteTextRenderer {
    LONG refs_{1};
    ID2D1RenderTarget* target_;
    ID2D1Factory* factory_;
    ID2D1Brush* brush_;

public:
    ReadingGlyphRenderer(ID2D1RenderTarget* target, ID2D1Factory* factory, ID2D1Brush* brush)
        : target_(target), factory_(factory), brush_(brush) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID i, void** out) override {
        if (!out)
            return E_POINTER;
        if (i == __uuidof(IUnknown) || i == __uuidof(IDWritePixelSnapping) ||
            i == __uuidof(IDWriteTextRenderer)) {
            *out = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --refs_;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* value) override {
        *value = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* value) override {
        D2D1_MATRIX_3X2_F m;
        target_->GetTransform(&m);
        memcpy(value, &m, sizeof(m));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* value) override {
        *value = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE,
                                           const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*,
                                           IUnknown*) override {
        ComPtr<ID2D1PathGeometry> geometry;
        CheckHR(factory_->CreatePathGeometry(&geometry), "reading");
        ComPtr<ID2D1GeometrySink> sink;
        CheckHR(geometry->Open(&sink), "reading");
        CheckHR(run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances,
                                                  run->glyphOffsets, run->glyphCount, run->isSideways,
                                                  run->bidiLevel % 2, sink.Get()),
                "reading");
        CheckHR(sink->Close(), "reading");
        D2D1_MATRIX_3X2_F previous;
        target_->GetTransform(&previous);
        target_->SetTransform(D2D1::Matrix3x2F::Translation(x, y) * previous);
        target_->FillGeometry(geometry.Get(), brush_);
        target_->SetTransform(previous);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*,
                                                IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL,
                                               IUnknown*) override {
        return E_NOTIMPL;
    }
};
void RenderReading(const Image& image, const OverlayBlock& block, const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> wic;
    CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)),
            "reading");
    ComPtr<IWICBitmap> bitmap;
    CheckHR(wic->CreateBitmap(image.width, image.height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                              &bitmap),
            "reading");
    ComPtr<ID2D1Factory> factory;
    CheckHR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()), "reading");
    ComPtr<ID2D1RenderTarget> target;
    CheckHR(factory->CreateWicBitmapRenderTarget(
                bitmap.Get(),
                D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
                &target),
            "reading");
    auto color = [](COLORREF c, float a = 1.f) {
        return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a);
    };
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0));
    ComPtr<ID2D1Bitmap> source;
    CheckHR(target->CreateBitmap(
                D2D1::SizeU(image.width, image.height), image.bgra.data(), image.width * 4,
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
                &source),
            "reading");
    target->DrawBitmap(source.Get());
    ComPtr<ID2D1SolidColorBrush> brush;
    CheckHR(target->CreateSolidColorBrush(color(block.background, block.opacity), &brush), "reading");
    target->FillRectangle(D2D1::RectF(0, 0, float(image.width), float(image.height)), brush.Get());
    brush->SetColor(color(RGB(20, 26, 38)));
    const float headerBottom = float(block.closeButton.bottom + 1);
    target->FillRectangle(D2D1::RectF(0, 0, float(image.width), headerBottom), brush.Get());
    const auto close = block.closeButton;
    brush->SetColor(color(RGB(180, 45, 60)));
    target->FillRectangle(
        D2D1::RectF(float(close.left), float(close.top), float(close.right), float(close.bottom)),
        brush.Get());
    brush->SetColor(color(RGB(235, 240, 250)));
    ComPtr<IDWriteFactory> writer;
    CheckHR(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(writer.GetAddressOf())),
            "reading");
    ComPtr<IDWriteTextFormat> caption;
    CheckHR(writer->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13, L"zh-CN",
                                     &caption),
            "reading");
    const wchar_t* title = L"译文 · 滚轮查看";
    target->DrawText(title, UINT32(wcslen(title)), caption.Get(),
                     D2D1::RectF(10, 0, float(close.left) - 6, headerBottom), brush.Get());
    ComPtr<ReadingGlyphRenderer> renderer;
    renderer.Attach(new ReadingGlyphRenderer(target.Get(), factory.Get(), brush.Get()));
    brush->SetColor(color(RGB(232, 236, 244)));
    for (const auto& paragraph : block.paragraphs) {
        CheckHR(paragraph.layout->Draw(nullptr, renderer.Get(), float(paragraph.rect.left),
                                       float(paragraph.rect.top)),
                "reading");
    }
    brush->SetColor(color(RGB(88, 119, 235)));
    target->DrawRectangle(D2D1::RectF(1, 1, float(image.width) - 1, float(image.height) - 1), brush.Get(), 2);
    CheckHR(target->EndDraw(), "reading");
    ComPtr<IWICStream> stream;
    CheckHR(wic->CreateStream(&stream), "reading");
    CheckHR(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "reading");
    ComPtr<IWICBitmapEncoder> encoder;
    CheckHR(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "reading");
    CheckHR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "reading");
    ComPtr<IWICBitmapFrameEncode> frame;
    CheckHR(encoder->CreateNewFrame(&frame, nullptr), "reading");
    CheckHR(frame->Initialize(nullptr), "reading");
    CheckHR(frame->WriteSource(bitmap.Get(), nullptr), "reading");
    CheckHR(frame->Commit(), "reading");
    CheckHR(encoder->Commit(), "reading");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        ComScope com;
        if (argc == 6 && std::wstring(argv[1]) == L"--attach-geometry") {
            // Adopts local OCR geometry for recorded model segments, so one entry can be compared
            // against the model rectangles and against the recognized rectangles.
            auto image = ReadImage(argv[2]);
            auto result = ReadRecordedResponse(argv[3]);
            auto detected = DetectOcrText(image, {}, std::stoi(argv[4]));
            auto report = AttachSourceGeometry(result, image.width, image.height, detected.regions);
            std::filesystem::path output = argv[5];
            std::filesystem::create_directories(output.parent_path());
            WriteRecordedResponse(result, output);
            std::cout << "regions=" << detected.regions.size() << " segments=" << report.segments
                      << " adopted=" << report.adopted << " rows=" << report.matchedLines
                      << " uncovered=" << report.uncoveredLines.size()
                      << " rejected_text=" << report.rejectedText
                      << " rejected_space=" << report.rejectedSpace
                      << " rejected_bounds=" << report.rejectedBounds << '\n';
            return 0;
        }
        if (argc == 4 && std::wstring(argv[1]) == L"--diagnose-response") {
            auto image = ReadImage(argv[2]);
            auto result = ReadRecordedResponse(argv[3]);
            auto layout = LayoutPositioned(result, image, Settings{}, 96);
            for (size_t i = 0; i < layout.size(); ++i) {
                const auto& item = layout[i];
                std::cout << i << (item.suppressed ? " SUPPRESSED" : " ok") << " src=(" << item.sourceRect.left
                          << "," << item.sourceRect.top << "," << item.sourceRect.right << ","
                          << item.sourceRect.bottom << ") rect=(" << item.rect.left << "," << item.rect.top
                          << "," << item.rect.right << "," << item.rect.bottom << ") font=" << item.fontSize
                          << " band=" << item.bandHeight << " core=" << item.coreHeight
                          << " lines=" << item.detectedLines.size() << " overflow=" << (item.overflow ? 1 : 0)
                          << " erase=" << item.eraseLines.size() << " layout=" << (item.layout ? 1 : 0)
                          << '\n';
            }
            return 0;
        }
        if (argc == 5 && std::wstring(argv[1]) == L"--render-response") {
            auto image = ReadImage(argv[2]);
            auto result = ReadRecordedResponse(argv[3]);
            auto layout = LayoutPositioned(result, image, Settings{}, 96);
            std::filesystem::path output = argv[4];
            std::filesystem::create_directories(output);
            Render(image, layout, output / L"translated.png", false);
            Render(image, layout, output / L"geometry.png", true);
            std::cout << "Recorded response rendered; segments=" << result.segments.size() << '\n';
            return 0;
        }
        if ((argc == 5 || argc == 7) && std::wstring(argv[1]) == L"--render-ocr") {
            // The production route hands every recognized region to the translation service and places
            // the returned text on the measured rows. This command renders that contract directly:
            // one segment per recognized region, so coverage, face size and row placement can be
            // inspected without a network round trip. The optional width and style draw the rim that
            // the placement path applies.
            auto image = ReadImage(argv[2]);
            // The production route recognizes through DetectOcrText, which keeps low-confidence rows so
            // the translation service can confirm them instead of losing the whole page.
            auto detected = DetectOcrText(image, {}, std::stoi(argv[3]));
            TranslationResult result;
            result.language = "source";
            for (const auto& region : detected.regions) {
                Segment segment;
                segment.original = region.text;
                segment.translated = region.text;
                segment.sourceId = region.id;
                segment.sourcePixels = region.rect;
                segment.sourceLines = region.lines;
                segment.sourceLineHeight = region.lineHeight;
                result.segments.push_back(std::move(segment));
                result.protectedRegions.push_back(region.rect);
            }
            Settings settings;
            if (argc == 7) {
                settings.outlineWidth = float(std::stod(argv[5]));
                settings.outlineStyle = std::stoi(argv[6]);
            }
            auto layout = LayoutPositioned(result, image, settings, 96);
            std::filesystem::path output = argv[4];
            std::filesystem::create_directories(output);
            Render(image, layout, output / L"translated.png", false, settings);
            Render(image, layout, output / L"geometry.png", true, settings);
            size_t suppressed = 0, overflow = 0;
            for (const auto& item : layout) {
                suppressed += item.suppressed;
                overflow += item.overflow;
            }
            std::cout << "OCR route rendered; regions=" << result.segments.size() << " suppressed=" << suppressed
                      << " overflow=" << overflow << " uncovered=" << result.uncoveredLines.size() << '\n';
            return 0;
        }
        if (argc == 5 && std::wstring(argv[1]) == L"--render-reading") {
            auto image = ReadImage(argv[2]);
            auto result = ReadRecordedResponse(argv[3]);
            Settings settings;
            settings.spatialOverlay = false;
            settings.autoFont = false;
            settings.fontSize = 18;
            RECT area{0, 0, image.width, image.height};
            Monitor monitor{L"reading", area, 96};
            auto blocks = BuildOverlay(result, image, area, monitor, settings, false);
            std::filesystem::path output = argv[4];
            std::filesystem::create_directories(output);
            RenderReading(image, blocks.front(), output / L"reading.png");
            std::cout << "Reading panel rendered; paragraphs=" << blocks.front().paragraphs.size() << '\n';
            return 0;
        }
        if (argc == 5 && std::wstring(argv[1]) == L"--dump-ocr") {
            // Exports detector boxes so a candidate detector can be compared box by box.
            auto image = ReadImage(argv[2]);
            auto detected = DetectOcrText(image, {}, std::stoi(argv[3]));
            std::ofstream file(argv[4]);
            if (!file)
                throw std::runtime_error("dump target unavailable");
            file << "{\"regions\":[";
            for (size_t i = 0; i < detected.regions.size(); ++i) {
                const auto& region = detected.regions[i];
                if (i)
                    file << ',';
                file << "{\"id\":" << region.id << ",\"rect\":[" << region.rect.left << ','
                     << region.rect.top << ',' << region.rect.right << ',' << region.rect.bottom
                     << "],\"lineHeight\":" << region.lineHeight << '}';
            }
            file << "]}\n";
            std::cout << "OCR boxes dumped; regions=" << detected.regions.size() << '\n';
            return 0;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--render-rim") {
            std::filesystem::path output = argv[2];
            std::filesystem::create_directories(output);
            RenderRims(output);
            return 0;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--ocr-manga") {
            // Runs the manga engine directly so its detection and vertical-column reading can be
            // inspected without the routing layer in between.
            auto image = ReadImage(argv[2]);
            std::cout << "manga weights available: " << (MangaOcrAvailable() ? 1 : 0) << '\n';
            auto lines = RecognizeMangaBundled(image, {});
            for (const auto& line : lines)
                std::cout << "rect=(" << line.rect.left << ',' << line.rect.top << ',' << line.rect.right
                          << ',' << line.rect.bottom << ") conf=" << line.confidence << " text=["
                          << line.text << "]\n";
            std::cout << "Manga OCR: lines=" << lines.size() << '\n';
            return 0;
        }
        if (argc == 4 && std::wstring(argv[1]) == L"--ocr-text") {
            // Prints what recognition read for every region, which is how a vertical column or a
            // rotated line shows whether the reading path found it.
            auto image = ReadImage(argv[2]);
            auto detected = DetectOcrText(image, {}, std::stoi(argv[3]));
            for (const auto& region : detected.regions)
                std::cout << "id=" << region.id << " rect=(" << region.rect.left << ',' << region.rect.top
                          << ',' << region.rect.right << ',' << region.rect.bottom
                          << ") conf=" << region.confidence << " text=[" << region.text << "]\n";
            std::cout << "OCR text: regions=" << detected.regions.size() << '\n';
            return 0;
        }
        if (argc == 4 && std::wstring(argv[1]) == L"--layout-bands") {
            // Prints the band and face each region ends up with, so the ink refinement can be read
            // against the detector box directly.
            auto image = ReadImage(argv[2]);
            auto detected = DetectOcrText(image, {}, std::stoi(argv[3]));
            TranslationResult result;
            result.language = "source";
            for (const auto& region : detected.regions) {
                Segment segment;
                segment.original = region.text;
                segment.translated = region.text;
                segment.sourceId = region.id;
                segment.sourcePixels = region.rect;
                segment.sourceLines = region.lines;
                segment.sourceLineHeight = region.lineHeight;
                result.segments.push_back(std::move(segment));
                result.protectedRegions.push_back(region.rect);
            }
            auto layout = LayoutPositioned(result, image, Settings{}, 96);
            for (const auto& item : layout)
                std::cout << "src=(" << item.sourceRect.left << ',' << item.sourceRect.top << ','
                          << item.sourceRect.right << ',' << item.sourceRect.bottom
                          << ") srcH=" << Height(item.sourceRect) << " band=" << item.bandHeight
                          << " core=" << item.coreHeight << " font=" << item.fontSize
                          << " rows=" << item.detectedLines.size() << '\n';
            return 0;
        }
        UnitTests();
        AppearanceRegression();
        ChatBubbleRegression();
        if (argc == 2 && std::wstring(argv[1]) == L"--appearance") {
            HandshakeRegression("test-fixtures/user/handshake-ai-response.json");
            HandshakeRegression("test-fixtures/user/handshake-ai-pixel-response.json");
            GameDialogRegression();
            std::cout << "TextLayoutTests appearance: PASS\n";
            return 0;
        }
        if (argc == 1) {
            HandshakeRegression("test-fixtures/user/handshake-ai-response.json");
            HandshakeRegression("test-fixtures/user/handshake-ai-pixel-response.json");
            GameDialogRegression();
            DirectVisionPlacementRegression("test-fixtures/user/github-ai-misplaced-response.json");
            DirectVisionPlacementRegression("test-fixtures/user/github-ai-pixel-response.json");
            ScreenshotRegression();
            UserScreenshotRegression();
            VerticalColumnRegression();
            std::ifstream response("test-fixtures/user/github-dashboard-ai-web-response.json");
            Require(bool(response), "user vision response fixture missing");
            const std::string source(std::istreambuf_iterator<char>(response), {});
            auto result = ParseTranslation(source);
            auto image = ReadImage(L"test-fixtures/user/github-dashboard-afternoon-en.png");
            RECT area{0, 0, image.width, image.height};
            auto blocks =
                BuildOverlay(result, std::move(image), area, Monitor{L"offscreen", area, 96}, Settings{});
            Require(result.segments.size() == 52 && result.clippedBoxes == 1 && blocks.size() == 1 &&
                        blocks[0].positioned.size() == 52,
                    "user vision response lost segments during layout");
            for (const auto& segment : result.segments)
                Require(blocks[0].text.find(Wide(segment.translated)) != std::wstring::npos,
                        "full reading lost a completed user translation");
            auto unpositioned = result;
            unpositioned.segments.back().positionUnavailable = true;
            unpositioned.segments.front().untranslated = true;
            auto fallbackImage = ReadImage(L"test-fixtures/user/github-dashboard-afternoon-en.png");
            auto retained = BuildOverlay(unpositioned, std::move(fallbackImage), area,
                                         Monitor{L"offscreen", area, 96}, Settings{});
            Require(retained[0].positioned.back().suppressed && retained[0].positioned.front().suppressed &&
                        !Valid(retained[0].positioned.back().sourceRect) &&
                        retained[0].text.find(Wide(result.segments.back().translated)) !=
                            std::wstring::npos &&
                        retained[0].text.find(L"【未完成翻译，保留原文】") != std::wstring::npos,
                    "unusable placement drew at the origin or lost full-reading content");
        }
        if (argc >= 3) {
            auto image = ReadImage(argv[1]);
            auto detected = DetectText(image, {}, OcrJapanese);
            std::cout << "OCR: " << detected.status << ", regions: " << detected.regions.size() << "\n";
            if (detected.status == "ocr-unavailable")
                RecognizeBundled(image, OcrJapanese, {});
            Require(!detected.regions.empty(), "OCR fixture produced no regions");
            std::filesystem::path output = argv[2];
            std::filesystem::create_directories(output);
            nlohmann::json translations = nlohmann::json::object();
            if (argc >= 4) {
                std::ifstream file(argv[3]);
                file >> translations;
            }
            TranslationResult result;
            for (const auto& r : detected.regions) {
                Segment s;
                s.original = r.text;
                s.translated = translations.value(std::to_string(r.id), r.text);
                s.sourcePixels = r.rect;
                s.sourceLines = r.lines;
                s.sourceLineHeight = r.lineHeight;
                s.sourceId = r.id;
                result.segments.push_back(s);
                result.protectedRegions.push_back(r.rect);
            }
            Settings settings;
            auto items = LayoutPositioned(result, image, settings, 96);
            nlohmann::json report = nlohmann::json::array();
            auto box = [](RECT r) { return nlohmann::json::array({r.left, r.top, r.right, r.bottom}); };
            for (size_t i = 0; i < items.size(); ++i) {
                const auto& a = items[i];
                nlohmann::json lines = nlohmann::json::array();
                for (auto line : a.detectedLines)
                    lines.push_back(box(line));
                report.push_back({{"id", result.segments[i].sourceId},
                                  {"text", result.segments[i].original},
                                  {"source", box(a.sourceRect)},
                                  {"layout", box(a.rect)},
                                  {"lines", lines},
                                  {"font", a.fontSize},
                                  {"overflow", a.overflow},
                                  {"suppressed", a.suppressed}});
            }
            std::ofstream file(output / L"regions.json");
            file << report.dump(2);
            file.close();
            Render(image, items, output / L"translated.png", false);
            Render(image, items, output / L"geometry.png", true);
        }
        std::cout << "TextLayoutTests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "TextLayoutTests: FAIL: " << e.what() << "\n";
        return 1;
    }
}
