// Verifies that one screen translation records the exact request bytes, the complete response, and
// every measured stage in one directory. The test also guards the pruning rule and the PNG capture.
#include "app/TranslationPipeline.h"
#include "common/Diagnostics.h"
#include "common/Platform.h"
#include "common/Trace.h"
#include "capture/Capture.h"
#include "providers/Provider.h"
#include <wincodec.h>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using namespace sat;
namespace {
void Require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input),
                                      std::istreambuf_iterator<char>());
}
bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
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
// Stands in for a vision model: it records the request the same way the real provider does, then
// returns a placement result, which keeps the pipeline path under test.
class FixtureProvider final : public ITranslationProvider {
public:
    TranslationResult Translate(const std::string&, const std::vector<ContextItem>&, const Settings&,
                                const std::string&, std::stop_token) override {
        throw std::runtime_error("located translation expected");
    }
    TranslationResult TranslateLocated(const std::string& image, const std::vector<ContextItem>&,
                                       const Settings& settings, const std::string&, std::stop_token,
                                       const std::vector<TextRegion>& regions) override {
        auto request = BuildRequest(image, {}, settings, false, regions);
        TraceRequest(request, regions, image, false, settings.translationMode == TranslationOcrText);
        TraceResponse(200, R"({"choices":[{"message":{"content":"{}"}}]})", "/chat/completions", "POST");
        TranslationResult result;
        result.language = "en";
        result.requestCount = 1;
        Segment segment;
        segment.original = "Screen text";
        segment.translated = "屏幕文字";
        segment.box = {40, 40, 400, 80};
        result.segments.push_back(std::move(segment));
        TraceParsed(result);
        return result;
    }
};
size_t DirectoryCount(const std::filesystem::path& root) {
    size_t count = 0;
    std::error_code code;
    for (std::filesystem::directory_iterator it(root, code), end; !code && it != end; it.increment(code))
        count += it->is_directory() ? 1 : 0;
    return count;
}
} // namespace

int main() {
    try {
        ComScope com;
        auto root = std::filesystem::absolute("test-output") /
                    ("trace-" + std::to_string(GetCurrentProcessId()) + "-" +
                     std::to_string(GetTickCount64()));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        InitializeTrace(root, true, 3);
        Require(TraceEnabled(), "trace stayed disabled after initialization");

        Settings options;
        options.provider = "deepseek";
        options.model = "trace-model";
        options.quality = 0;
        options.spatialOverlay = false;
        Image picture{16, 16, std::vector<unsigned char>(16 * 16 * 4, 128)};
        const auto jpeg = EncodeJpeg(picture, 1, {});
        const auto base64 = Base64(jpeg);
        const std::string reply =
            R"({"choices":[{"message":{"content":"{\"source_language\":\"en\",\"segments\":[]}"}}]})";
        std::filesystem::path unit;
        {
            DiagnosticContext context(1);
            TraceSession session("unit", nlohmann::json::object({{"provider", "fixture"}}));
            Require(static_cast<bool>(session), "trace session did not open");
            unit = session.Directory();
            TextRegion region;
            region.id = 1;
            region.text = "Hello";
            region.confidence = 1;
            region.rect = {0, 0, 16, 16};
            region.lines = {region.rect};
            region.lineHeight = 8;
            auto request = BuildRequest(base64, {}, options, false, {region});
            TraceRequest(request, {region}, base64, false, false);
            TraceResponse(200, reply, "/chat/completions", "POST");
            TranslationResult parsed;
            parsed.language = "en";
            parsed.requestCount = 1;
            TraceParsed(parsed);
            {
                DiagnosticSpan api("api");
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
        }
        Require(std::filesystem::exists(unit / "session.json"), "session metadata missing");
        Require(std::filesystem::exists(unit / "timing.md"), "timing summary missing");
        Require(std::filesystem::exists(unit / "timing.json"), "timing record missing");
        Require(std::filesystem::exists(unit / "parsed.json"), "parsed record missing");
        const auto request = ReadAll(unit / "1" / "request.json");
        Require(Has(request, "trace-model"), "request record lost the model name");
        Require(!Has(request, "base64,"), "request record kept the inline image payload");
        Require(Has(request, "temperature"), "request record lost sampling fields");
        const auto system = ReadAll(unit / "1" / "request.system.txt");
        Require(Has(system, "screen translation engine"), "system prompt missing from the trace");
        const auto user = ReadAll(unit / "1" / "request.user.txt");
        Require(Has(user, "translation_preferences"), "user payload missing from the trace");
        Require(std::filesystem::exists(unit / "1" / "request.regions.json"), "source regions missing");
        Require(ReadBytes(unit / "upload.jpg") == jpeg, "stored upload differs from the sent bytes");
        Require(ReadAll(unit / "1" / "response.json") == reply, "stored response differs from the reply");
        Require(Has(ReadAll(unit / "1" / "response.content.txt"), "source_language"),
                "model content extraction failed");
        const auto document = ReadAll(unit / "session.json");
        Require(Has(document, "\"kind\": \"unit\""), "session record lost its kind");
        Require(Has(document, "\"files\""), "session record lists no recorded files");
        Require(Has(document, "1/request.json"), "session record lost the request file entry");
        const auto timing = ReadAll(unit / "timing.md");
        Require(Has(timing, "# 请求明细"), "timing summary has no heading");
        Require(Has(timing, "| api |") || Has(timing, "| ⤷ api |"), "timing summary lost the api stage");
        Require(Has(ReadAll(unit / "timing.json"), "duration_ms"), "timing record lost durations");

        // The pipeline path: capture, recognition, and layout reach the same directory.
        std::filesystem::path pipeline;
        {
            DiagnosticContext context(2);
            TraceSession session("pipeline");
            pipeline = session.Directory();
            auto image = ReadFixture(L"test-fixtures/user/github-dashboard-en.png");
            RECT region{0, 0, image.width, image.height};
            Monitor monitor{L"fixture", region, 96};
            Settings run = options;
            run.ocrMode = OcrEnglish;
            auto prepared = PrepareTranslation(std::move(image), region, monitor, run, {}, "PRIVATE_TEST_KEY",
                                               {},
                                               [](const std::string&) {
                                                   return std::make_unique<FixtureProvider>();
                                               },
                                               [](TranslationPhase) {});
            Require(prepared.blocks.size() == 1, "pipeline lost the overlay image");
        }
        Require(std::filesystem::exists(pipeline / "capture.png"), "capture image missing");
        const auto png = ReadBytes(pipeline / "capture.png");
        Require(png.size() > 8 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G',
                "capture image is not a PNG");
        Require(std::filesystem::exists(pipeline / "ocr.json"), "recognition record missing");
        Require(Has(ReadAll(pipeline / "ocr.json"), "\"confidence\""), "recognition record lost confidence");
        const auto pipelineTiming = ReadAll(pipeline / "timing.md");
        Require(Has(pipelineTiming, "ocr"), "timing summary lost the recognition stage");
        Require(Has(pipelineTiming, "layout"), "timing summary lost the layout stage");
        Require(Has(ReadAll(pipeline / "session.json"), "\"ocr_ms\""), "session summary lost recognition time");
        Require(std::filesystem::exists(pipeline / "1" / "request.json"), "pipeline request record missing");

        // Pruning keeps only the newest directories.
        auto pruneRoot = root / "prune";
        std::filesystem::create_directories(pruneRoot);
        InitializeTrace(pruneRoot, true, 2);
        for (int i = 0; i < 3; ++i) {
            DiagnosticContext context(10 + i);
            TraceSession session("prune");
            Require(static_cast<bool>(session), "prune session did not open");
        }
        Require(DirectoryCount(pruneRoot) == 2, "trace pruning kept the wrong number of directories");
        InitializeTrace({}, false);
        Require(!TraceEnabled(), "trace stayed enabled after shutdown");
        std::cout << "trace checks passed" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "trace check failed: " << error.what() << std::endl;
        return 1;
    }
}
