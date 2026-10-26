#include "capture/TextDetection.h"
#include "capture/BundledOcr.h"
#include "overlay/SpatialLayout.h"
#include "providers/Provider.h"
#include <wincodec.h>
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
            bool diagnostic) {
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
    DrawPositioned(target.Get(), items, RGB(20, 20, 20), diagnostic);
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
                for (const auto& item : items)
                    if (!item.suppressed && PtInRect(&item.rect, p))
                        covered = true;
                if (covered)
                    continue;
                size_t pos = (size_t(y) * image.width + x) * 4;
                for (int c = 0; c < 3; ++c)
                    Require(std::abs(int(bytes[pos + c]) - int(image.bgra[pos + c])) <= 1,
                            "source pixel outside translation changed");
            }
    }
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
            Require(suppressed == 0, "BBC headlines or category labels hidden by geometry conflicts");
            Require(
                translations.size() == detected.regions.size() &&
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
    return result;
}
void HandshakeRegression(const char* responsePath) {
    std::ifstream file(responsePath);
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
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        ComScope com;
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
        UnitTests();
        AppearanceRegression();
        if (argc == 1) {
            HandshakeRegression("test-fixtures/user/handshake-ai-response.json");
            HandshakeRegression("test-fixtures/user/handshake-ai-pixel-response.json");
            GameDialogRegression();
            DirectVisionPlacementRegression("test-fixtures/user/github-ai-misplaced-response.json");
            DirectVisionPlacementRegression("test-fixtures/user/github-ai-pixel-response.json");
            ScreenshotRegression();
            UserScreenshotRegression();
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
