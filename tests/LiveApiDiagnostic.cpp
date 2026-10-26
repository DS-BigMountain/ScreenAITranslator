#include "providers/Provider.h"
#include "settings/Store.h"
#include "common/Diagnostics.h"
#include "common/Platform.h"
#include "capture/Capture.h"
#include "providers/TranslationQuality.h"
#include <wincodec.h>
#include <iostream>
#include <algorithm>
#include <fstream>
namespace {
sat::Image ReadImage(const wchar_t* path) {
    using namespace sat;
    ComPtr<IWICImagingFactory> factory;
    CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
            "diagnostic");
    ComPtr<IWICBitmapDecoder> decoder;
    CheckHR(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                               &decoder),
            "diagnostic");
    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHR(decoder->GetFrame(0, &frame), "diagnostic");
    ComPtr<IWICFormatConverter> converter;
    CheckHR(factory->CreateFormatConverter(&converter), "diagnostic");
    CheckHR(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                  0, WICBitmapPaletteTypeCustom),
            "diagnostic");
    UINT width{}, height{};
    CheckHR(converter->GetSize(&width, &height), "diagnostic");
    Image image{int(width), int(height), std::vector<unsigned char>(size_t(width) * height * 4)};
    CheckHR(converter->CopyPixels(nullptr, width * 4, UINT(image.bgra.size()), image.bgra.data()),
            "diagnostic");
    return image;
}
} // namespace
// Manual diagnostic only: uses saved credentials with an explicit test input, never the desktop.
int wmain(int argc, wchar_t** argv) {
    const bool ocrImage = argc >= 3 && std::wstring(argv[1]) == L"--saved-settings-ocr-image";
    const bool suppliedImage = (argc == 3 || argc == 4 || (argc == 5 && ocrImage)) &&
                               (ocrImage || std::wstring(argv[1]) == L"--saved-settings-image");
    if (!suppliedImage && (argc != 2 || std::wstring(argv[1]) != L"--saved-settings")) {
        std::cerr << "Use --saved-settings or --saved-settings[-ocr]-image IMAGE [RESULT_JSON].\n";
        return 2;
    }
    SetConsoleOutputCP(CP_UTF8);
    std::string key;
    auto clear = [&] {
        if (!key.empty())
            SecureZeroMemory(key.data(), key.size());
    };
    try {
        sat::ComScope com;
        auto logRoot =
            std::filesystem::absolute("test-output") / ("live-image-" + std::to_string(GetTickCount64()));
        sat::InitializeDiagnostics(logRoot);
        sat::DiagnosticContext context(1);
        sat::Store store;
        auto options = store.LoadSettings();
        options.translationMode = ocrImage ? sat::TranslationOcrText : sat::TranslationAiVision;
        if (argc == 5 && ocrImage)
            options.ocrMode = std::stoi(argv[4]);
        options = sat::SelectTranslationConnection(options);
        key = sat::UnprotectSecret(options.encryptedKey);
        if (key.empty()) {
            std::cout << "No saved API key; no request made.\n";
            return 2;
        }
        if (suppliedImage) {
            if (argc >= 4 && std::filesystem::exists(argv[3]))
                throw sat::AppError("diagnostic", "结果文件已存在，未发送请求");
            auto image = ReadImage(argv[2]);
            const auto started = std::chrono::steady_clock::now();
            std::vector<sat::TextRegion> regions;
            std::string data;
            if (ocrImage) {
                auto detected = sat::DetectOcrText(image, {}, options.ocrMode);
                regions = std::move(detected.regions);
                std::erase_if(regions, [](const auto& region) { return region.text.empty(); });
                if (regions.empty())
                    throw sat::AppError("diagnostic", "OCR 没有获得文字，未发送请求");
            } else {
                data = sat::Base64(sat::EncodeJpeg(image, options.quality, {}));
            }
            auto provider = sat::CreateProvider(options.provider);
            auto result = provider->TranslateLocated(data, {}, options, key, {}, regions);
            sat::FlagUntranslatedPassages(result);
            if (result.segments.empty())
                throw sat::AppError("diagnostic", "模型没有返回可翻译文字");
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
            if (argc >= 4) {
                using Json = nlohmann::json;
                Json report = {{"mode", options.translationMode},
                               {"source_language", result.language},
                               {"width", image.width},
                               {"height", image.height},
                               {"request_count", result.requestCount},
                               {"duration_ms", elapsed},
                               {"segments", Json::array()}};
                for (const auto& segment : result.segments) {
                    Json item = {{"original_text", segment.original},
                                 {"translated_text", segment.translated},
                                 {"bbox",
                                  {{"x", segment.box.x},
                                   {"y", segment.box.y},
                                   {"width", segment.box.width},
                                   {"height", segment.box.height}}},
                                 {"position_unavailable", segment.positionUnavailable},
                                 {"untranslated", segment.untranslated},
                                 {"source_line_height", segment.sourceLineHeight},
                                 {"source_lines", Json::array()}};
                    if (segment.sourcePixels) {
                        auto r = *segment.sourcePixels;
                        item["source_pixels"] = {r.left, r.top, r.right, r.bottom};
                    }
                    for (auto r : segment.sourceLines)
                        item["source_lines"].push_back({r.left, r.top, r.right, r.bottom});
                    report["segments"].push_back(std::move(item));
                }
                std::filesystem::path output = argv[3];
                std::filesystem::create_directories(output.parent_path());
                std::ofstream file(output);
                file << report.dump(2);
                if (!file)
                    throw sat::AppError("diagnostic", "无法保存显式请求的结果文件");
            }
            std::cout << "Image diagnostic: " << elapsed << " ms; requests=" << result.requestCount
                      << "; segments=" << result.segments.size() << "; clipped_boxes=" << result.clippedBoxes
                      << "; unpositioned=" << result.unpositionedSegments
                      << "; untranslated=" << result.untranslatedSegments
                      << "; ignored=" << result.ignoredSegments << "; log=" << sat::Utf8(logRoot.wstring())
                      << std::endl;
            clear();
            return 0;
        }
        auto models = sat::FetchModels(options, key, {});
        std::cout << "Models fetched: " << models.size() << "; saved model present: "
                  << (std::find(models.begin(), models.end(), options.model) != models.end() ? "yes" : "no")
                  << std::endl;
        auto result = sat::TestVisionApi(options, key, {});
        std::cout << "Vision API diagnostic PASS; recognized and translated synthetic image, segments: "
                  << result.segments.size() << std::endl;
        clear();
        return 0;
    } catch (const sat::AppError& e) {
        std::cerr << "Diagnostic failed [" << e.stage << "]: " << sat::Sanitize(e.what(), key) << std::endl;
        clear();
        return 1;
    } catch (...) {
        std::cerr << "Diagnostic failed; no configuration was changed.\n";
        clear();
        return 1;
    }
}
