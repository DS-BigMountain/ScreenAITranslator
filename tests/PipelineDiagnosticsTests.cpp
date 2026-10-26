#include "app/TranslationPipeline.h"
#include "common/Diagnostics.h"
#include <wincodec.h>
#include <fstream>
#include <iostream>

using namespace sat;
namespace {
void Require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

Image ReadFixture(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> factory;
    CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
            "fixture");
    ComPtr<IWICBitmapDecoder> decoder;
    CheckHR(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                               WICDecodeMetadataCacheOnLoad, &decoder),
            "fixture");
    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHR(decoder->GetFrame(0, &frame), "fixture");
    ComPtr<IWICFormatConverter> converter;
    CheckHR(factory->CreateFormatConverter(&converter), "fixture");
    CheckHR(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                  0, WICBitmapPaletteTypeCustom),
            "fixture");
    UINT width{}, height{};
    CheckHR(converter->GetSize(&width, &height), "fixture");
    Image image{int(width), int(height), std::vector<unsigned char>(size_t(width) * height * 4)};
    CheckHR(converter->CopyPixels(nullptr, width * 4, UINT(image.bgra.size()), image.bgra.data()), "fixture");
    return image;
}

class FixtureProvider final : public ITranslationProvider {
    bool fail_;

public:
    explicit FixtureProvider(bool fail) : fail_(fail) {}
    TranslationResult Translate(const std::string&, const std::vector<ContextItem>&, const Settings&,
                                const std::string&, std::stop_token) override {
        throw std::runtime_error("located translation expected");
    }
    TranslationResult TranslateLocated(const std::string& image, const std::vector<ContextItem>&,
                                       const Settings& settings, const std::string&, std::stop_token,
                                       const std::vector<TextRegion>& regions) override {
        if (settings.translationMode == TranslationOcrText) {
            Require(image.empty() && regions.size() >= 20 && HasRecognizedText(regions),
                    "OCR mode uploaded an image or discarded all local text");
            auto request = BuildRequest(image, {}, settings, false, regions).dump();
            Require(request.find("image_url") == std::string::npos &&
                        request.find("data:image") == std::string::npos,
                    "OCR mode serialized image data");
        } else {
            Require(!image.empty() && regions.empty(), "AI mode received local OCR regions");
            auto request = BuildRequest(image, {}, settings, false, regions).dump();
            Require(request.find("source_regions") == std::string::npos &&
                        request.find("image_url") != std::string::npos,
                    "AI mode did not use the direct image protocol");
        }
        if (fail_)
            throw AppError("Requesting", "请求超时");
        TranslationResult result;
        result.language = "en";
        result.requestCount = 1;
        Segment segment;
        segment.original = "PRIVATE_SOURCE_SENTINEL";
        segment.translated = "离屏诊断译文";
        if (!regions.empty()) {
            segment.sourceId = regions.front().id;
            segment.sourcePixels = regions.front().rect;
        } else {
            segment.box = {20, 20, 500, 100};
        }
        result.segments.push_back(std::move(segment));
        return result;
    }
};
} // namespace

int main() {
    try {
        ComScope com;
        auto root = std::filesystem::absolute("test-output") /
                    ("pipeline-diagnostics-" + std::to_string(GetCurrentProcessId()) + "-" +
                     std::to_string(GetTickCount64()));
        InitializeDiagnostics(root);
        unsigned long long operation = 0;
        Settings options;
        options.quality = 0;
        options.spatialOverlay = false;
        for (const auto* name : {L"github-dashboard-en.png", L"github-browser-dashboard-en.png"}) {
            for (int mode : {TranslationAiVision, TranslationOcrText}) {
                DiagnosticContext context(++operation);
                auto image = ReadFixture(std::filesystem::path(L"test-fixtures/user") / name);
                RECT region{0, 0, image.width, image.height};
                Monitor monitor{L"fixture", region, 96};
                options.translationMode = mode;
                options.ocrMode = OcrEnglish;
                std::vector<TranslationPhase> phases;
                bool timeout = false;
                try {
                    PrepareTranslation(
                        std::move(image), region, monitor, options, {}, "PRIVATE_TEST_KEY", {},
                        [](const std::string&) { return std::make_unique<FixtureProvider>(true); },
                        [&](TranslationPhase phase) { phases.push_back(phase); });
                } catch (const AppError& error) {
                    timeout = error.stage == "Requesting";
                }
                Require(timeout && phases == std::vector{TranslationPhase::Requesting},
                        "API failure was swallowed or continued into layout");
                if (operation == 1)
                    Require(GetModuleHandleW(L"onnxruntime.dll") == nullptr,
                            "AI mode loaded local OCR runtime");
            }
        }
        for (int mode : {TranslationAiVision, TranslationOcrText}) {
            DiagnosticContext context(++operation);
            auto image = ReadFixture(L"test-fixtures/user/github-browser-dashboard-en.png");
            RECT region{0, 0, image.width, image.height};
            Monitor monitor{L"fixture", region, 96};
            options.translationMode = mode;
            std::vector<TranslationPhase> phases;
            auto prepared = PrepareTranslation(
                std::move(image), region, monitor, options, {}, "PRIVATE_TEST_KEY", {},
                [](const std::string&) { return std::make_unique<FixtureProvider>(false); },
                [&](TranslationPhase phase) { phases.push_back(phase); });
            Require(phases == std::vector{TranslationPhase::Requesting, TranslationPhase::Layout} &&
                        prepared.blocks.size() == 1 && prepared.blocks.front().image &&
                        prepared.result.requestCount == 1 &&
                        prepared.textOnly == (mode == TranslationOcrText),
                    "extracted pipeline lost display data or phase order");
        }
        {
            DiagnosticContext context(++operation);
            options.translationMode = TranslationOcrText;
            Image blank{640, 320, std::vector<unsigned char>(640 * 320 * 4, 255)};
            bool rejected = false;
            try {
                PrepareTranslation(
                    std::move(blank), {0, 0, 640, 320}, {L"blank", {0, 0, 640, 320}, 96}, options, {},
                    "PRIVATE_TEST_KEY", {},
                    [](const std::string&) -> std::unique_ptr<ITranslationProvider> {
                        throw std::runtime_error("empty OCR must not call any API");
                    },
                    [](TranslationPhase) {});
            } catch (const AppError& error) {
                rejected = error.stage == "ocr";
            }
            Require(rejected, "empty OCR switched to an image request");
        }
        {
            DiagnosticContext context(++operation);
            std::stop_source stop;
            stop.request_stop();
            bool cancelled = false;
            try {
                PrepareTranslation(
                    {}, {}, {}, options, {}, "", stop.get_token(),
                    [](const std::string&) -> std::unique_ptr<ITranslationProvider> {
                        throw std::runtime_error("cancelled pipeline called API");
                    },
                    [](TranslationPhase) {});
            } catch (const Cancelled&) {
                cancelled = true;
            }
            Require(cancelled, "pipeline ignored cancellation");
        }
        std::ifstream log(root / "diagnostic.jsonl");
        std::string line;
        int timeouts = 0, completedOcr = 0, routes = 0;
        while (std::getline(log, line)) {
            Require(line.find("PRIVATE_") == std::string::npos &&
                        line.find("离屏诊断译文") == std::string::npos,
                    "pipeline logged source text, translation or key");
            auto row = nlohmann::json::parse(line);
            if (row.value("stage", "") == "api" && row.value("reason", "") == "timeout")
                ++timeouts;
            if (row.value("stage", "") == "ocr" && row.value("outcome", "") == "ok") {
                ++completedOcr;
                std::cout << "operation=" << row["operation"] << " OCR=" << row["duration_ms"]
                          << " ms regions=" << row["regions"] << '\n';
            }
            if (row.value("event", "") == "route_selected")
                ++routes;
        }
        Require(timeouts == 4 && completedOcr == 4 && routes == 6,
                "logs did not separate successful local OCR from simulated API timeout");
        std::cout << "Pipeline diagnostics passed: real screenshots, simulated API, no visible windows\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
