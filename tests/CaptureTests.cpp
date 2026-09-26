#include "capture/Capture.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <limits>
#include <iostream>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void RequiresFailure(F&& function, const char* message) {
    bool failed = false;
    try { function(); } catch (const sat::AppError&) { failed = true; }
    Require(failed, message);
}
std::pair<UINT,UINT> JpegSize(std::vector<unsigned char>& bytes) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    sat::CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "test");
    ComPtr<IWICStream> stream;
    sat::CheckHR(factory->CreateStream(&stream), "test");
    sat::CheckHR(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size())), "test");
    ComPtr<IWICBitmapDecoder> decoder;
    sat::CheckHR(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder), "test");
    ComPtr<IWICBitmapFrameDecode> frame;
    sat::CheckHR(decoder->GetFrame(0, &frame), "test");
    UINT width = 0, height = 0;
    sat::CheckHR(frame->GetSize(&width, &height), "test");
    // Decode all pixels as well, so a JPEG with only a readable header cannot pass.
    WICPixelFormatGUID format{};
    sat::CheckHR(frame->GetPixelFormat(&format), "test");
    ComPtr<IWICFormatConverter> converter;
    sat::CheckHR(factory->CreateFormatConverter(&converter), "test");
    sat::CheckHR(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "test");
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
    sat::CheckHR(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()), "test");
    return {width, height};
}
}

void RunCaptureTests() {
    using namespace sat;
    const Monitor monitor{L"monitor-test", {-1920,-200,0,880}, 144};
    const RECT selection{-1820,-100,-620,500};
    const auto region = NormalizeRegion(monitor, selection);
    const auto restored = RestoreRegion(region, {monitor});
    Require(restored && EqualRect(&*restored, &selection), "negative physical screen coordinates round trip");
    auto moved = monitor;
    OffsetRect(&moved.rect, 4000, 500);
    const auto movedRegion = RestoreRegion(region, {moved});
    Require(movedRegion && movedRegion->left == selection.left + 4000 && movedRegion->top == selection.top + 500,
        "stable identity follows desktop-origin changes");
    auto resized = monitor;
    resized.rect.right += 10;
    Require(!RestoreRegion(region, {resized}), "resolution change invalidates fixed region");
    Require(!RestoreRegion(region, {}), "missing display invalidates fixed region");
    auto corrupt = region;
    corrupt.x = std::numeric_limits<double>::quiet_NaN();
    Require(!RestoreRegion(corrupt, {monitor}), "NaN state is rejected");
    corrupt = region; corrupt.width = 2;
    Require(!RestoreRegion(corrupt, {monitor}), "oversized normalized state is rejected");
    RequiresFailure([&] { NormalizeRegion(monitor, {-2000,0,0,100}); }, "cross-display selection rejected");

    Image source{4,3,std::vector<unsigned char>(48)};
    for (size_t i = 0; i < source.bgra.size(); ++i) source.bgra[i] = static_cast<unsigned char>(i);
    const auto crop = Crop(source, {1,1,3,3});
    Require(crop.width == 2 && crop.height == 2 && crop.bgra[0] == 20 && crop.bgra[8] == 36,
        "crop copies selected pixels with correct source stride");
    RequiresFailure([&] { Crop(source, {-1,0,1,1}); }, "negative crop rejected");
    RequiresFailure([&] { Crop(source, {0,0,5,1}); }, "outside crop rejected");
    Require(Base64({}).empty(), "empty base64");
    Require(Base64({'f'}) == "Zg==" && Base64({'f','o'}) == "Zm8=" && Base64({'f','o','o'}) == "Zm9v", "RFC 4648 padding");

    Image background{100,100,std::vector<unsigned char>(40000,255)};
    for (int y = 20; y < 80; ++y) for (int x = 20; x < 80; ++x)
        for (int c = 0; c < 3; ++c) background.bgra[(y*100+x)*4+c] = 0;
    const auto white = AnalyzeBackground(background, {20,20,80,80});
    Require(white.simple && white.color == RGB(255,255,255) && white.variance == 0,
        "background ring excludes text and measures white median");
    for (int y = 0; y < 100; ++y) for (int x = 0; x < 100; ++x)
        for (int c = 0; c < 3; ++c) background.bgra[(y*100+x)*4+c] = ((x+y)%2) ? 255 : 0;
    Require(!AnalyzeBackground(background, {20,20,80,80}).simple, "high variance ring classified complex");
    Require(!AnalyzeBackground(background, {0,0,100,100}).simple, "no exterior samples uses readable fallback");

    std::stop_source cancellation;
    cancellation.request_stop();
    bool stopped = false;
    try { EncodeJpeg(source, 1, cancellation.get_token()); } catch (const Cancelled&) { stopped = true; }
    Require(stopped, "cancelled encode exits before allocation");
    stopped = false;
    try { CaptureMonitor(monitor, cancellation.get_token()); } catch (const Cancelled&) { stopped = true; }
    Require(stopped, "cancelled capture exits before DXGI initialization");

    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CheckHR(apartment, "test");
    struct ApartmentGuard { ~ApartmentGuard() { CoUninitialize(); } } guard;
    Image large{2400,1200,std::vector<unsigned char>(2400*1200*4,255)};
    const UINT expected[]{1280,1600,2200};
    for (int preset = 0; preset < 3; ++preset) {
        auto jpeg = EncodeJpeg(large, preset, {});
        Require(jpeg.size() > 4 && jpeg[0] == 0xff && jpeg[1] == 0xd8, "actual JPEG encoded");
        const auto [width,height] = JpegSize(jpeg);
        Require(width == expected[preset] && height == expected[preset]/2, "preset scales proportionally to maximum edge");
    }
    auto smallJpeg = EncodeJpeg(source, 0, {});
    Require(JpegSize(smallJpeg) == std::pair<UINT,UINT>{4,3}, "small screenshot is never upscaled");
}

#ifdef SAT_CAPTURE_TEST_MAIN
int main(int argc, char** argv) {
    try {
        RunCaptureTests();
        std::cout << "Capture tests passed\n";
        if (argc == 2 && std::string(argv[1]) == "--live") {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            const auto monitors = sat::EnumerateMonitors();
            Require(!monitors.empty(), "physical monitors enumerated");
            for (const auto& monitor : monitors) {
                const auto start = std::chrono::steady_clock::now();
                const auto captured = sat::CaptureMonitor(monitor, {});
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                Require(captured.width == sat::Width(monitor.rect) && captured.height == sat::Height(monitor.rect), "live capture dimensions match physical monitor");
                std::cout << "Live capture: " << captured.width << 'x' << captured.height << ", " << elapsed << " ms; " << (captured.backend == sat::CaptureBackend::Dxgi ? "DXGI" : "GDI static fallback") << "; memory only\n";
            }
        }
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
#endif
