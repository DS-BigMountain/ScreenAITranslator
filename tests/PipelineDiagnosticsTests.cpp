#include "app/TranslationPipeline.h"
#include "common/Diagnostics.h"
#include <wincodec.h>
#include <fstream>
#include <iostream>
#include <thread>

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
    int delayMs_;

public:
    explicit FixtureProvider(bool fail, int delayMs = 0) : fail_(fail), delayMs_(delayMs) {}
    TranslationResult Translate(const std::string&, const std::vector<ContextItem>&, const Settings&,
                                const std::string&, std::stop_token) override {
        throw std::runtime_error("located translation expected");
    }
    TranslationResult TranslateLocated(const std::string& image, const std::vector<ContextItem>&,
                                       const Settings& settings, const std::string&, std::stop_token stop,
                                       const std::vector<TextRegion>& regions) override {
        for (int waited = 0; waited < delayMs_; waited += 10) {
            if (stop.stop_requested())
                throw Cancelled{};
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (settings.translationMode == TranslationOcrText) {
            Require(image.empty() && regions.size() >= 20 && HasRecognizedText(regions),
                    "OCR mode uploaded an image or discarded all local text");
            auto request = BuildRequest(image, {}, settings, false, regions).dump();
            Require(request.find("image_url") == std::string::npos &&
                        request.find("data:image") == std::string::npos,
                    "OCR mode serialized image data");
        } else {
            // The reference ordering recognizes first and sends the measured rows together with the
            // image, so the model returns one translation per source_id instead of locating text by
            // itself. Losing the regions here would bring back the uncovered lines this contract fixes.
            Require(!image.empty() && regions.size() >= 20 && HasRecognizedText(regions),
                    "vision mode lost the recognized source rows");
            auto request = BuildRequest(image, {}, settings, false, regions).dump();
            Require(request.find("source_regions") != std::string::npos &&
                        request.find("image_url") != std::string::npos,
                    "vision mode did not combine the image with its source regions");
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
        // One entry routes by provider capability: dedicated text services receive recognized text,
        // every image model receives the capture while the local detector still supplies geometry.
        const std::vector<std::string> providers{"deepseek", "deepl"};
        for (const auto* name : {L"github-dashboard-en.png", L"github-browser-dashboard-en.png"}) {
            for (const auto& provider : providers) {
                DiagnosticContext context(++operation);
                auto image = ReadFixture(std::filesystem::path(L"test-fixtures/user") / name);
                RECT region{0, 0, image.width, image.height};
                Monitor monitor{L"fixture", region, 96};
                options.provider = provider;
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
                Require(timeout && phases == std::vector{TranslationPhase::Recognizing, TranslationPhase::Requesting},
                        "API failure was swallowed or continued into layout");
            }
        }
        for (const auto& provider : providers) {
            DiagnosticContext context(++operation);
            auto image = ReadFixture(L"test-fixtures/user/github-browser-dashboard-en.png");
            RECT region{0, 0, image.width, image.height};
            Monitor monitor{L"fixture", region, 96};
            options.provider = provider;
            std::vector<TranslationPhase> phases;
            auto prepared = PrepareTranslation(
                std::move(image), region, monitor, options, {}, "PRIVATE_TEST_KEY", {},
                [](const std::string&) { return std::make_unique<FixtureProvider>(false, 600); },
                [&](TranslationPhase phase) { phases.push_back(phase); });
            Require(phases == std::vector{TranslationPhase::Recognizing, TranslationPhase::Requesting, TranslationPhase::Layout} &&
                        prepared.blocks.size() == 1 && prepared.blocks.front().image &&
                        prepared.result.requestCount == 1 && prepared.textOnly,
                    "extracted pipeline lost display data or phase order");
            // Every provider receives recognized text only. Recognition runs ahead of the request, so
            // its duration is measured and precedes the translation call.
            Require(prepared.ocrMs > 0 && prepared.apiMs > 0 &&
                        prepared.totalMs + 200 >= prepared.ocrMs + prepared.apiMs,
                    "run lost its recognition stage or its stage timings");
        }
        {
            DiagnosticContext context(++operation);
            options.provider = "deepl";
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
            // A vision model receives a capture whose local recognition found nothing: the whole
            // image goes out instead of the run failing on an empty page.
            DiagnosticContext context(++operation);
            options.provider = "openai";
            Image blank{640, 320, std::vector<unsigned char>(640 * 320 * 4, 255)};
            bool reachedProvider = false;
            try {
                PrepareTranslation(
                    std::move(blank), {0, 0, 640, 320}, {L"blank", {0, 0, 640, 320}, 96}, options, {},
                    "PRIVATE_TEST_KEY", {},
                    [&reachedProvider](const std::string&) -> std::unique_ptr<ITranslationProvider> {
                        reachedProvider = true;
                        throw std::runtime_error("fallback reached the provider");
                    },
                    [](TranslationPhase) {});
            } catch (const std::runtime_error&) {
            }
            Require(reachedProvider, "empty OCR did not fall back to the whole-image route");
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
        // Recognition runs ahead of every request now, so all eight routed runs finish it: four
        // simulated API failures, two successful runs, the empty capture whose empty result stops a
        // text service before any request, and the empty capture that falls back to the whole-image
        // route. The cancelled run stops before routing and reaches neither stage.
        Require(timeouts == 4 && completedOcr == 8 && routes == 8,
                "logs did not separate successful local OCR from simulated API timeout");
        std::cout << "Pipeline diagnostics passed: real screenshots, simulated API, no visible windows\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
