#include "capture/TextDetection.h"
#include "capture/BundledOcr.h"
#include "capture/Capture.h"
#include "common/Platform.h"
#include "providers/Provider.h"
#include <nlohmann/json.hpp>
#include <wincodec.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <algorithm>
using namespace sat;
namespace {
void Require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
Image Read(const wchar_t* path) {
    ComPtr<IWICImagingFactory> f;
    CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)),
            "test");
    ComPtr<IWICBitmapDecoder> decoder;
    CheckHR(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder),
            "test");
    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHR(decoder->GetFrame(0, &frame), "test");
    ComPtr<IWICFormatConverter> converter;
    CheckHR(f->CreateFormatConverter(&converter), "test");
    CheckHR(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                  0, WICBitmapPaletteTypeCustom),
            "test");
    UINT w{}, h{};
    converter->GetSize(&w, &h);
    Image image{int(w), int(h), std::vector<unsigned char>(size_t(w) * h * 4)};
    CheckHR(converter->CopyPixels(nullptr, w * 4, UINT(image.bgra.size()), image.bgra.data()), "test");
    return image;
}
std::string Text(const TextDetection& result) {
    std::string text;
    for (const auto& r : result.regions)
        text += r.text + '\n';
    return text;
}
// Model inventory lets the offline report state exactly which weight set ran.
nlohmann::json ModelInventory() {
    nlohmann::json inventory = nlohmann::json::object();
    std::error_code error;
    for (const char* name : {"det.onnx", "det-server.onnx", "rec-ja.onnx", "rec-ja-server.onnx", "rec-en.onnx",
                             "rec-en-server.onnx", "rec-ko.onnx", "rec-ko-server.onnx", "cls.onnx"})
        inventory[name] = std::filesystem::is_regular_file(std::filesystem::path("models") / name, error);
    return inventory;
}
// UTF-8 code point scan: kana and CJK counts expose Japanese recall in the report.
struct TextStats {
    size_t characters{}, kana{}, kanji{}, japaneseLines{};
};
TextStats CountText(const std::string& text) {
    TextStats stats;
    bool japanese = false;
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        char32_t code = 0;
        size_t extra = 0;
        if (lead < 0x80) {
            code = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            code = lead & 0x1F;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            code = lead & 0x0F;
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            code = lead & 0x07;
            extra = 3;
        } else {
            ++i;
            continue;
        }
        for (size_t k = 1; k <= extra && i + k < text.size(); ++k)
            code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        i += extra + 1;
        ++stats.characters;
        if ((code >= 0x3040 && code <= 0x30FF) || (code >= 0xFF66 && code <= 0xFF9F)) {
            ++stats.kana;
            japanese = true;
        } else if ((code >= 0x4E00 && code <= 0x9FFF) || (code >= 0x3400 && code <= 0x4DBF)) {
            ++stats.kanji;
            japanese = true;
        }
    }
    stats.japaneseLines = japanese ? 1 : 0;
    return stats;
}
// Offline mobile-versus-server measurement used to document the upgrade. The
// caller stages one weight set per directory and compares the JSON reports.
int RunBenchmark(const std::filesystem::path& output) {
    struct Case {
        const wchar_t* path;
        const char* label;
        int mode;
    };
    const Case cases[] = {
        {L"test-fixtures/user/game-data-update-ja.png", "game-data-update-ja", OcrLocalAuto},
        {L"test-fixtures/user/game-data-update-ja.png", "game-data-update-ja/ja", OcrJapanese},
        {L"test-fixtures/user/asahi-article-ja.png", "asahi-article-ja", OcrLocalAuto},
        {L"test-fixtures/user/asahi-article-ja.png", "asahi-article-ja/ja", OcrJapanese},
        {L"test-fixtures/user/bbc-article-en.png", "bbc-article-en", OcrLocalAuto},
        {L"test-fixtures/user/bbc-article-en.png", "bbc-article-en/en", OcrEnglish},
        {L"test-fixtures/user/bbc-headlines-en.png", "bbc-headlines-en/ja", OcrJapanese},
    };
    nlohmann::json report = nlohmann::json::object();
    report["models"] = ModelInventory();
    report["cases"] = nlohmann::json::array();
    for (const auto& item : cases) {
        auto image = Read(item.path);
        // Geometry-only pass separates detector cost from recognizer cost in the report.
        const auto geometryStart = std::chrono::steady_clock::now();
        auto geometry = DetectTextGeometry(image);
        const auto geometryElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - geometryStart);
        const auto start = std::chrono::steady_clock::now();
        auto lines = RecognizeBundled(image, item.mode, {}, false, true, true);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        nlohmann::json rows = nlohmann::json::array();
        double total = 0;
        size_t characters = 0, kana = 0, kanji = 0, japaneseLines = 0;
        for (const auto& line : lines) {
            rows.push_back({{"x", line.rect.left},
                            {"y", line.rect.top},
                            {"w", Width(line.rect)},
                            {"h", Height(line.rect)},
                            {"text", line.text},
                            {"confidence", line.confidence}});
            total += line.confidence;
            characters += line.text.size();
            const auto stats = CountText(line.text);
            kana += stats.kana;
            kanji += stats.kanji;
            japaneseLines += stats.japaneseLines;
        }
        report["cases"].push_back({{"image", item.label},
                                   {"width", image.width},
                                   {"height", image.height},
                                   {"mode", item.mode},
                                   {"elapsed_ms", elapsed.count()},
                                   {"geometry_elapsed_ms", geometryElapsed.count()},
                                   {"geometry_regions", geometry.regions.size()},
                                   {"lines", rows},
                                   {"line_count", lines.size()},
                                   {"character_count", characters},
                                   {"kana_count", kana},
                                   {"kanji_count", kanji},
                                   {"japanese_hit_lines", japaneseLines},
                                   {"mean_confidence", lines.empty() ? 0.0 : total / double(lines.size())}});
        std::cout << "Benchmark " << item.label << ": " << lines.size() << " lines, " << characters
                  << " bytes, " << elapsed.count() << " ms\n";
    }
    std::filesystem::create_directories(output.parent_path());
    std::ofstream stream(output, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot write benchmark report");
    stream << report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    std::cout << "Benchmark report written: " << output.string() << "\n";
    return 0;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        ComScope com;
        if (argc == 3 && std::wstring(argv[1]) == L"--ocr-benchmark")
            return RunBenchmark(argv[2]);
        auto english = Read(L"test-fixtures/installer-en.png");
        if (argc == 2 && std::wstring(argv[1]) == L"--missing-korean") {
            auto whole = Read(L"test-fixtures/korean-sign.jpg");
            auto image = Crop(whole, {0, 0, whole.width / 2, whole.height});
            Require(DetectText(image, {}, OcrKorean).regions.empty(),
                    "Missing Korean model unexpectedly succeeded");
            Require(DetectText(image, {}, OcrAuto).regions.empty(),
                    "Automatic mode accepted unverified Korean text with missing model");
            auto geometry = DetectTextGeometry(image);
            Require(geometry.status == "local-geometry-ai" && !geometry.regions.empty(),
                    "Missing Korean weights broke vision geometry");
            std::cout << "Missing Korean model: safe fallback PASS\n";
            return 0;
        }
        if (argc == 2 && std::wstring(argv[1]) == L"--missing-recognizer") {
            auto single = DetectTextForTranslation(english, {}, OcrJapanese);
            Require(single.status == "local-geometry-ai" && !single.regions.empty() &&
                        !CanTranslateText(single.regions),
                    "production path failed to retain detector output after missing recognizer");
            Require(DetectText(english, {}, OcrJapanese).regions.empty(),
                    "Missing recognizer unexpectedly succeeded");
            auto geometry = DetectTextGeometry(english);
            Require(geometry.status == "local-geometry-ai" && !geometry.regions.empty(),
                    "AI fallback lost available detector geometry");
            for (const auto& r : geometry.regions)
                Require(r.text.empty() && r.imageBox.width > 0 && r.lineHeight > 0,
                        "AI geometry carried invented OCR text");
            std::cout << "Detector-only fallback: PASS\n";
            return 0;
        }
        if (argc == 2 && std::wstring(argv[1]) == L"--missing-assets") {
            auto result = DetectText(english, {}, OcrJapanese);
            Require(result.regions.empty() && result.status == "ocr-unavailable",
                    "Missing assets did not produce safe fallback");
            Require(DetectText(english, {}, 3).status == "direct-ai",
                    "Direct AI attempted to load missing OCR");
            std::cout << "Missing OCR assets: safe fallback PASS\n";
            return 0;
        }
        for (int mode : {OcrJapanese, OcrEnglish}) {
            auto start = std::chrono::steady_clock::now();
            auto result = DetectText(english, {}, mode);
            auto text = Text(result);
            Require(result.status == "bundled-ocr" && text.find("Additional Tasks") != std::string::npos &&
                        text.find("desktop shortcut") != std::string::npos,
                    "Bundled English recognition failed");
            for (const auto& r : result.regions)
                Require(r.confidence >= 0.f && r.confidence <= 1.f, "OCR confidence outside valid range");
            std::cout << "English mode " << mode << ": "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count()
                      << " ms, path=" << (CanTranslateText(result.regions) ? "text" : "vision") << "\n";
        }
        auto geometry = DetectTextGeometry(english);
        Require(!geometry.regions.empty() && geometry.status == "local-geometry-ai",
                "Geometry-only AI path failed");
        for (const auto& r : geometry.regions)
            Require(r.text.empty() && r.imageBox.width > 0, "Geometry-only path ran recognition");
        auto japanese = Read(L"test-fixtures/news-ja.png");
        auto result = DetectText(japanese, {}, 2);
        Require(result.status == "bundled-ocr" && Text(result).find("1200") != std::string::npos &&
                    Text(result).find("古代") != std::string::npos,
                "Bundled Japanese recognition failed");
        std::cout << "Japanese path=" << (CanTranslateText(result.regions) ? "text" : "vision") << "\n";
        for (auto file :
             {L"test-fixtures/upstream/korean-sign.jpg", L"test-fixtures/upstream/korean-scene.jpg"}) {
            auto whole = Read(file);
            const bool scene = std::wstring(file).find(L"scene") != std::wstring::npos;
            // The scene photo's far-left reflection contains unrelated vertical text.
            // Select the sign itself, as a user would select a translation region.
            auto korean = Crop(whole, scene ? RECT{whole.width / 12, whole.height / 5, whole.width * 47 / 100,
                                                   whole.height * 4 / 5}
                                            : RECT{0, 0, whole.width / 2, whole.height});
            const auto anchor = scene ? "종합안내도" : "좋아하는";
            for (int mode : {OcrKorean, OcrLocalAuto, OcrAuto}) {
                if (mode == OcrAuto) {
                    Require(DetectText(korean, {}, mode).status == "direct-ai",
                            "Automatic mode must defer language recognition to vision");
                    auto located = DetectTextGeometry(korean);
                    Require(!located.regions.empty() && !CanTranslateText(located.regions),
                            "Automatic mode accidentally chose text-only translation");
                    for (const auto& region : located.regions)
                        Require(region.text.empty(), "Automatic geometry invented OCR text");
                    continue;
                }
                auto detected = DetectText(korean, {}, mode);
                auto text = Text(detected);
                std::cout << "Korean " << mode << " status=" << detected.status << " text=" << text << "\n";
                Require(detected.status == "bundled-ocr" && text.find(anchor) != std::string::npos,
                        "Korean screenshot lost expected text");
            }
        }
        Image blank{500, 200, std::vector<unsigned char>(500 * 200 * 4, 255)};
        for (auto [path, anchor] :
             {std::pair{L"test-fixtures/user/game-data-update-ja.png", "キャンセル"},
              std::pair{L"test-fixtures/user/handshake-error-en.png", "HandshakeException"},
              std::pair{L"test-fixtures/user/github-dashboard-afternoon-en.png", "Dashboard"}}) {
            auto image = Read(path);
            const auto start = std::chrono::steady_clock::now();
            auto automatic = DetectOcrText(image, {}, OcrLocalAuto);
            const auto text = Text(automatic);
            std::cout << "Local auto: " << text << "elapsed="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count()
                      << " ms\n";
            Require(text.find(anchor) != std::string::npos, "automatic OCR lost real screenshot anchor");
            if (std::wstring(path).find(L"game-data") != std::wstring::npos)
                Require(text.find("データ更新") != std::string::npos &&
                            text.find("247.04") != std::string::npos &&
                            text.find("推奨") != std::string::npos,
                        "automatic Japanese OCR retained Latin gibberish or lost content");
        }
        for (auto [path, mode] : {std::pair{L"test-fixtures/user/bbc-headlines-en.png", OcrEnglish},
                                  std::pair{L"test-fixtures/user/asahi-article-ja.png", OcrJapanese}}) {
            auto image = Read(path);
            auto start = std::chrono::steady_clock::now();
            auto single = DetectOcrText(image, {}, mode);
            Require(single.regions.size() >= 15, "single-pass production OCR lost real screenshot coverage");
            for (const auto& r : single.regions)
                Require(r.imageBox.width > 0 && !r.lines.empty(), "production fallback lost geometry");
            std::cout << "Production OCR mode " << mode << ": "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count()
                      << " ms, regions=" << single.regions.size() << "\n";
        }
        // Explicit strongest-available smoke check: a staged server detector must
        // produce the same production status as the mobile fallback.
        std::error_code modelError;
        if (std::filesystem::is_regular_file(std::filesystem::path("models") / "det-server.onnx", modelError))
            Require(DetectText(english, {}, 1).status == "bundled-ocr",
                    "Server detector smoke check failed");
        Require(DetectText(blank, {}, OcrJapanese).status == "no-text", "Blank image generated OCR regions");
        Require(DetectText(japanese, {}, 3).status == "direct-ai", "Explicit AI path ignored");
        Require(DetectText(japanese, {}, 99).status == "invalid-mode", "Invalid OCR mode accepted");
        std::stop_source source;
        std::jthread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            source.request_stop();
        });
        bool cancelled = false;
        try {
            DetectText(japanese, source.get_token(), OcrJapanese);
        } catch (const Cancelled&) {
            cancelled = true;
        }
        Require(cancelled, "In-flight OCR cancellation ignored");
        Require(DetectText(english, {}, 1).status == "bundled-ocr",
                "Cancelled run poisoned subsequent inference");
        std::cout << "BundledOcrTests: PASS (English, Japanese, Korean, automatic vision, blank, "
                     "cancellation, recovery)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "BundledOcrTests: FAIL: " << e.what() << "\n";
        return 1;
    }
}



