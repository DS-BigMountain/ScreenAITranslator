#include "capture/Capture.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace sat {
namespace {
using Microsoft::WRL::ComPtr;

size_t ImageBytes(int width, int height) {
    if (width <= 0 || height <= 0 || static_cast<uint64_t>(width) * height > 100000000)
        throw AppError("image", "截图尺寸无效或过大");
    return static_cast<size_t>(width) * height * 4;
}
void ValidateImage(const Image& image) {
    if (image.bgra.size() != ImageBytes(image.width, image.height))
        throw AppError("image", "截图数据无效");
}
bool HasDesktopPixels(const Image& image) {
    // A driver can successfully return an uninitialized black duplication surface.
    // Check RGB, never alpha: alpha is not desktop image data and is often zero.
    for (size_t p = 0; p < image.bgra.size(); p += 4)
        if (image.bgra[p] || image.bgra[p + 1] || image.bgra[p + 2])
            return true;
    return false;
}

// Device interface identity survives desktop-origin changes and display renumbering.
// If Windows supplies no persistent identity, use the GDI name only for this session.
std::wstring DisplayId(const wchar_t* deviceName) {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (EnumDisplayDevicesW(deviceName, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME) && device.DeviceID[0])
        return device.DeviceID;
    return deviceName;
}
Monitor DescribeMonitor(HMONITOR handle) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info))
        throw AppError("capture", "显示器已失效，请重新截图");
    UINT x = 96, y = 96;
    if (FAILED(GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &x, &y)))
        x = 96;
    return {DisplayId(info.szDevice), info.rcMonitor, x};
}

struct MonitorEnumeration {
    std::vector<Monitor> values;
    std::exception_ptr error;
};
BOOL CALLBACK MonitorCallback(HMONITOR monitor, HDC, LPRECT, LPARAM state) {
    auto& enumeration = *reinterpret_cast<MonitorEnumeration*>(state);
    try {
        enumeration.values.push_back(DescribeMonitor(monitor));
    } catch (...) {
        enumeration.error = std::current_exception();
        return FALSE;
    }
    return TRUE;
}

struct ComApartment {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ComApartment() {
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
            CheckHR(result, "image");
    }
    ~ComApartment() {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};
struct AcquiredFrame {
    IDXGIOutputDuplication* duplication;
    ~AcquiredFrame() {
        duplication->ReleaseFrame();
    }
};
struct MappedTexture {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    ~MappedTexture() {
        context->Unmap(texture, 0);
    }
};
std::optional<Image> CopyRectFromScreen(RECT screenRect, std::stop_token stop) {
    CheckStop(stop);
    const int width = Width(screenRect), height = Height(screenRect);
    Image result{width, height, std::vector<unsigned char>(ImageBytes(width, height)),
                 CaptureBackend::GdiFallback};
    HDC screen = GetDC(nullptr);
    if (!screen)
        throw AppError("capture", "无法访问当前桌面");
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = result.width;
    info.bmiHeader.biHeight = -result.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels{};
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!memory || !bitmap) {
        if (memory)
            DeleteDC(memory);
        if (bitmap)
            DeleteObject(bitmap);
        ReleaseDC(nullptr, screen);
        throw AppError("capture", "无法分配截图内存");
    }
    auto previous = SelectObject(memory, bitmap);
    const BOOL copied = BitBlt(memory, 0, 0, result.width, result.height, screen, screenRect.left,
                               screenRect.top, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    if (copied)
        std::memcpy(result.bgra.data(), pixels, result.bgra.size());
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!copied)
        return std::nullopt;
    for (size_t p = 3; p < result.bgra.size(); p += 4)
        result.bgra[p] = 255;
    CheckStop(stop);
    return result;
}
Image CopyStaticDesktop(const Monitor& monitor, std::stop_token stop) {
    auto result = CopyRectFromScreen(monitor.rect, stop);
    if (!result)
        throw AppError("capture", "无法读取当前桌面画面");
    if (!HasDesktopPixels(*result))
        throw AppError("capture", "截图为空白，请重试或切换为窗口模式");
    return std::move(*result);
}
} // namespace

std::vector<Monitor> EnumerateMonitors() {
    MonitorEnumeration enumeration;
    if (!EnumDisplayMonitors(nullptr, nullptr, MonitorCallback, reinterpret_cast<LPARAM>(&enumeration))) {
        if (enumeration.error)
            std::rethrow_exception(enumeration.error);
        throw AppError("capture", "无法获取显示器信息");
    }
    return std::move(enumeration.values);
}

Monitor MonitorAtCursor() {
    POINT point{};
    if (!GetCursorPos(&point))
        throw AppError("capture", "无法获取鼠标位置");
    return DescribeMonitor(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST));
}

FixedRegion NormalizeRegion(const Monitor& monitor, RECT screenRect) {
    if (!Valid(monitor.rect) || !Valid(screenRect) || screenRect.left < monitor.rect.left ||
        screenRect.top < monitor.rect.top || screenRect.right > monitor.rect.right ||
        screenRect.bottom > monitor.rect.bottom)
        throw AppError("selection", "选区必须完全位于一个显示器内");
    const double w = Width(monitor.rect), h = Height(monitor.rect);
    return {monitor.id,
            (screenRect.left - monitor.rect.left) / w,
            (screenRect.top - monitor.rect.top) / h,
            Width(screenRect) / w,
            Height(screenRect) / h,
            Width(monitor.rect),
            Height(monitor.rect)};
}

std::optional<RECT> RestoreRegion(const FixedRegion& region, const std::vector<Monitor>& monitors) {
    if (!std::isfinite(region.x) || !std::isfinite(region.y) || !std::isfinite(region.width) ||
        !std::isfinite(region.height) || region.x < 0 || region.y < 0 || region.width <= 0 ||
        region.height <= 0 || region.x + region.width > 1.000000001 || region.y + region.height > 1.000000001)
        return std::nullopt;
    const auto found = std::find_if(monitors.begin(), monitors.end(),
                                    [&](const Monitor& monitor) { return monitor.id == region.monitorId; });
    if (found == monitors.end())
        return std::nullopt;
    const auto r = found->rect;
    // The spec's explicit invalidation rule takes precedence over its optional
    // small-resolution-change suggestion: require the original physical dimensions.
    if (!Valid(r) || Width(r) != region.monitorWidth || Height(r) != region.monitorHeight)
        return std::nullopt;
    RECT restored{r.left + static_cast<LONG>(std::lround(region.x * Width(r))),
                  r.top + static_cast<LONG>(std::lround(region.y * Height(r))),
                  r.left + static_cast<LONG>(std::lround((region.x + region.width) * Width(r))),
                  r.top + static_cast<LONG>(std::lround((region.y + region.height) * Height(r)))};
    restored.right = std::min(restored.right, r.right);
    restored.bottom = std::min(restored.bottom, r.bottom);
    return Valid(restored) ? std::optional<RECT>(restored) : std::nullopt;
}

Image CaptureMonitor(const Monitor& monitor, std::stop_token stop) {
    CheckStop(stop);
    ImageBytes(Width(monitor.rect), Height(monitor.rect));
    ComPtr<IDXGIFactory1> factory;
    CheckHR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "capture");
    ComPtr<IDXGIAdapter1> selectedAdapter;
    ComPtr<IDXGIOutput1> selectedOutput;
    DXGI_OUTPUT_DESC selectedDescription{};
    for (UINT a = 0; !selectedOutput; ++a) {
        CheckStop(stop);
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapters1(a, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        CheckHR(hr, "capture");
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(o, &output);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            CheckHR(hr, "capture");
            DXGI_OUTPUT_DESC description{};
            CheckHR(output->GetDesc(&description), "capture");
            if (description.AttachedToDesktop && DisplayId(description.DeviceName) == monitor.id) {
                if (!EqualRect(&description.DesktopCoordinates, &monitor.rect))
                    throw AppError("capture", "显示器布局已改变，请重新截图");
                selectedAdapter = adapter;
                CheckHR(output.As(&selectedOutput), "capture");
                selectedDescription = description;
                break;
            }
        }
    }
    if (!selectedOutput)
        throw AppError("capture", "显示器已失效，请重新截图");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    CheckHR(D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device,
                              nullptr, &context),
            "capture");
    ComPtr<IDXGIOutputDuplication> duplication;
    CheckHR(selectedOutput->DuplicateOutput(device.Get(), &duplication), "capture");
    ComPtr<IDXGIResource> resource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    for (;;) {
        CheckStop(stop);
        const HRESULT hr = duplication->AcquireNextFrame(16, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (std::chrono::steady_clock::now() >= deadline)
                return CopyStaticDesktop(monitor, stop);
            continue;
        }
        CheckHR(hr, "capture");
        if (frameInfo.LastPresentTime.QuadPart == 0) {
            // Pointer-only notifications do not carry a new desktop image. This
            // on-demand duplicator has no previous frame that could be reused.
            duplication->ReleaseFrame();
            resource.Reset();
            if (std::chrono::steady_clock::now() >= deadline)
                return CopyStaticDesktop(monitor, stop);
            continue;
        }
        break;
    }
    AcquiredFrame frameGuard{duplication.Get()};
    CheckStop(stop);
    ComPtr<ID3D11Texture2D> texture;
    CheckHR(resource.As(&texture), "capture");
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
        throw AppError("capture", "当前桌面图像格式不受支持");
    const int rawWidth = static_cast<int>(description.Width),
              rawHeight = static_cast<int>(description.Height);
    ImageBytes(rawWidth, rawHeight);
    const bool quarterTurn = selectedDescription.Rotation == DXGI_MODE_ROTATION_ROTATE90 ||
                             selectedDescription.Rotation == DXGI_MODE_ROTATION_ROTATE270;
    const int width = quarterTurn ? rawHeight : rawWidth, height = quarterTurn ? rawWidth : rawHeight;
    if (width != Width(monitor.rect) || height != Height(monitor.rect))
        throw AppError("capture", "显示器分辨率已改变，请重新截图");
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    CheckHR(device->CreateTexture2D(&description, nullptr, &staging), "capture");
    context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "capture");
    MappedTexture mapGuard{context.Get(), staging.Get()};
    Image result{width, height, std::vector<unsigned char>(ImageBytes(width, height)), CaptureBackend::Dxgi};
    for (int y = 0; y < rawHeight; ++y) {
        CheckStop(stop);
        const auto* row =
            static_cast<const unsigned char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        for (int x = 0; x < rawWidth; ++x) {
            int dx = x, dy = y;
            switch (selectedDescription.Rotation) {
            case DXGI_MODE_ROTATION_ROTATE90:
                dx = rawHeight - 1 - y;
                dy = x;
                break;
            case DXGI_MODE_ROTATION_ROTATE180:
                dx = rawWidth - 1 - x;
                dy = rawHeight - 1 - y;
                break;
            case DXGI_MODE_ROTATION_ROTATE270:
                dx = y;
                dy = rawWidth - 1 - x;
                break;
            default:
                break;
            }
            auto* destination = result.bgra.data() + (static_cast<size_t>(dy) * width + dx) * 4;
            std::memcpy(destination, row + static_cast<size_t>(x) * 4, 4);
            destination[3] = 255;
        }
    }
    CheckStop(stop);
    if (!HasDesktopPixels(result))
        return CopyStaticDesktop(monitor, stop);
    return result;
}

Image CaptureRegionQuick(const Monitor& monitor, RECT screenRect, std::stop_token stop) {
    if (!Valid(screenRect) || screenRect.left < monitor.rect.left || screenRect.top < monitor.rect.top ||
        screenRect.right > monitor.rect.right || screenRect.bottom > monitor.rect.bottom)
        throw AppError("image", "截图选区越界");
    if (auto quick = CopyRectFromScreen(screenRect, stop); quick && HasDesktopPixels(*quick))
        return std::move(*quick);
    RECT local = screenRect;
    OffsetRect(&local, -monitor.rect.left, -monitor.rect.top);
    return Crop(CaptureMonitor(monitor, stop), local);
}

bool FramesMatch(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height || a.width <= 0 || a.height <= 0 ||
        a.bgra.size() != b.bgra.size())
        return false;
    const size_t pixels = static_cast<size_t>(a.width) * a.height;
    const size_t tolerance = std::min<size_t>(200, std::max<size_t>(16, pixels / 500));
    size_t changed = 0;
    for (size_t i = 0; i + 3 < a.bgra.size(); i += 4) {
        const int diff = std::abs(int(a.bgra[i]) - int(b.bgra[i])) +
                         std::abs(int(a.bgra[i + 1]) - int(b.bgra[i + 1])) +
                         std::abs(int(a.bgra[i + 2]) - int(b.bgra[i + 2]));
        if (diff > 24 && ++changed > tolerance)
            return false;
    }
    return true;
}

Image Crop(const Image& image, RECT localRect) {
    ValidateImage(image);
    if (!Valid(localRect) || localRect.left < 0 || localRect.top < 0 || localRect.right > image.width ||
        localRect.bottom > image.height)
        throw AppError("image", "截图选区越界");
    Image result{Width(localRect), Height(localRect),
                 std::vector<unsigned char>(ImageBytes(Width(localRect), Height(localRect)))};
    for (int y = 0; y < result.height; ++y)
        std::memcpy(result.bgra.data() + static_cast<size_t>(y) * result.width * 4,
                    image.bgra.data() +
                        (static_cast<size_t>(localRect.top + y) * image.width + localRect.left) * 4,
                    static_cast<size_t>(result.width) * 4);
    return result;
}

std::vector<unsigned char> EncodeJpeg(const Image& image, int quality, std::stop_token stop) {
    CheckStop(stop);
    ValidateImage(image);
    ComApartment apartment;
    static constexpr int maximumEdges[]{1280, 1600, 2200};
    static constexpr float jpegQualities[]{0.72f, 0.82f, 0.90f};
    quality = std::clamp(quality, 0, 2);
    const double scale =
        std::min(1.0, static_cast<double>(maximumEdges[quality]) / std::max(image.width, image.height));
    const UINT width = std::max(1u, static_cast<UINT>(std::lround(image.width * scale)));
    const UINT height = std::max(1u, static_cast<UINT>(std::lround(image.height * scale)));
    ComPtr<IWICImagingFactory> factory;
    CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
            "image");
    ComPtr<IWICBitmap> bitmap;
    CheckHR(factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppBGRA,
                                            image.width * 4, static_cast<UINT>(image.bgra.size()),
                                            const_cast<BYTE*>(image.bgra.data()), &bitmap),
            "image");
    ComPtr<IWICBitmapSource> source;
    CheckHR(bitmap.As(&source), "image");
    ComPtr<IWICBitmapScaler> scaler;
    if (scale < 1.0) {
        CheckHR(factory->CreateBitmapScaler(&scaler), "image");
        CheckHR(scaler->Initialize(source.Get(), width, height, WICBitmapInterpolationModeFant), "image");
        CheckHR(scaler.As(&source), "image");
    }
    CheckStop(stop);
    ComPtr<IWICFormatConverter> converter;
    CheckHR(factory->CreateFormatConverter(&converter), "image");
    CheckHR(converter->Initialize(source.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr,
                                  0, WICBitmapPaletteTypeCustom),
            "image");
    ComPtr<IStream> stream;
    CheckHR(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "image");
    ComPtr<IWICBitmapEncoder> encoder;
    CheckHR(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder), "image");
    CheckHR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "image");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    CheckHR(encoder->CreateNewFrame(&frame, &options), "image");
    PROPBAG2 property{};
    property.pstrName = const_cast<wchar_t*>(L"ImageQuality");
    VARIANT value{};
    value.vt = VT_R4;
    value.fltVal = std::max(image.width <= 640 && image.height <= 320 ? 0.9f : 0.0f, jpegQualities[quality]);
    CheckHR(options->Write(1, &property, &value), "image");
    CheckHR(frame->Initialize(options.Get()), "image");
    CheckHR(frame->SetSize(width, height), "image");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    CheckHR(frame->SetPixelFormat(&format), "image");
    if (format != GUID_WICPixelFormat24bppBGR)
        throw AppError("image", "JPEG 编码格式不受支持");
    CheckHR(frame->WriteSource(converter.Get(), nullptr), "image");
    CheckStop(stop);
    CheckHR(frame->Commit(), "image");
    CheckHR(encoder->Commit(), "image");
    STATSTG stat{};
    CheckHR(stream->Stat(&stat, STATFLAG_NONAME), "image");
    if (stat.cbSize.QuadPart > std::numeric_limits<ULONG>::max())
        throw AppError("image", "编码图片过大");
    std::vector<unsigned char> result(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER offset{};
    CheckHR(stream->Seek(offset, STREAM_SEEK_SET, nullptr), "image");
    ULONG read = 0;
    CheckHR(stream->Read(result.data(), static_cast<ULONG>(result.size()), &read), "image");
    if (read != result.size())
        throw AppError("image", "读取编码图片失败");
    CheckStop(stop);
    return result;
}

std::vector<unsigned char> EncodePng(const Image& image, std::stop_token stop) {
    CheckStop(stop);
    ValidateImage(image);
    ComApartment apartment;
    ComPtr<IWICImagingFactory> factory;
    CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
            "image");
    ComPtr<IWICBitmap> bitmap;
    CheckHR(factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppBGRA,
                                            image.width * 4, static_cast<UINT>(image.bgra.size()),
                                            const_cast<BYTE*>(image.bgra.data()), &bitmap),
            "image");
    CheckStop(stop);
    ComPtr<IStream> stream;
    CheckHR(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "image");
    ComPtr<IWICBitmapEncoder> encoder;
    CheckHR(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "image");
    CheckHR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "image");
    ComPtr<IWICBitmapFrameEncode> frame;
    CheckHR(encoder->CreateNewFrame(&frame, nullptr), "image");
    CheckHR(frame->Initialize(nullptr), "image");
    CheckHR(frame->SetSize(image.width, image.height), "image");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    CheckHR(frame->SetPixelFormat(&format), "image");
    if (format != GUID_WICPixelFormat32bppBGRA)
        throw AppError("image", "PNG 编码格式不受支持");
    CheckHR(frame->WriteSource(bitmap.Get(), nullptr), "image");
    CheckStop(stop);
    CheckHR(frame->Commit(), "image");
    CheckHR(encoder->Commit(), "image");
    STATSTG stat{};
    CheckHR(stream->Stat(&stat, STATFLAG_NONAME), "image");
    if (stat.cbSize.QuadPart > std::numeric_limits<ULONG>::max())
        throw AppError("image", "编码图片过大");
    std::vector<unsigned char> result(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER offset{};
    CheckHR(stream->Seek(offset, STREAM_SEEK_SET, nullptr), "image");
    ULONG read = 0;
    CheckHR(stream->Read(result.data(), static_cast<ULONG>(result.size()), &read), "image");
    if (read != result.size())
        throw AppError("image", "读取编码图片失败");
    CheckStop(stop);
    return result;
}

std::string Base64(const std::vector<unsigned char>& bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned value = (static_cast<unsigned>(bytes[i]) << 16) |
                               (i + 1 < bytes.size() ? static_cast<unsigned>(bytes[i + 1]) << 8 : 0) |
                               (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        result.push_back(alphabet[value >> 18]);
        result.push_back(alphabet[(value >> 12) & 63]);
        result.push_back(i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=');
        result.push_back(i + 2 < bytes.size() ? alphabet[value & 63] : '=');
    }
    return result;
}

Background AnalyzeBackground(const Image& image, RECT localRect) {
    ValidateImage(image);
    // Sample only the exterior ring, avoiding text inside the detected box.
    localRect.left = std::clamp<LONG>(localRect.left, 0, image.width);
    localRect.top = std::clamp<LONG>(localRect.top, 0, image.height);
    localRect.right = std::clamp<LONG>(localRect.right, 0, image.width);
    localRect.bottom = std::clamp<LONG>(localRect.bottom, 0, image.height);
    if (!Valid(localRect))
        return {RGB(20, 20, 20), 10000, 20, false};
    constexpr int ringWidth = 6;
    const int left = std::max(0L, localRect.left - ringWidth), top = std::max(0L, localRect.top - ringWidth);
    const int right = std::min<LONG>(image.width, localRect.right + ringWidth);
    const int bottom = std::min<LONG>(image.height, localRect.bottom + ringWidth);
    std::array<std::array<size_t, 256>, 3> histograms{};
    std::array<double, 3> sums{}, squares{};
    size_t count = 0;
    const int stride = std::max(1, (right - left + bottom - top) / 4096);
    for (int y = top; y < bottom; y += stride) {
        for (int x = left; x < right; x += stride) {
            if (x >= localRect.left && x < localRect.right && y >= localRect.top && y < localRect.bottom) {
                x = static_cast<int>(localRect.right) - stride;
                continue;
            }
            const auto* pixel = image.bgra.data() + (static_cast<size_t>(y) * image.width + x) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                const unsigned sample = pixel[channel];
                ++histograms[channel][sample];
                sums[channel] += sample;
                squares[channel] += sample * sample;
            }
            ++count;
        }
    }
    if (!count)
        return {RGB(20, 20, 20), 10000, 20, false};
    std::array<unsigned char, 3> median{};
    double variance = 0;
    for (int channel = 0; channel < 3; ++channel) {
        size_t accumulated = 0;
        for (int sample = 0; sample < 256; ++sample) {
            accumulated += histograms[channel][sample];
            if (accumulated >= (count + 1) / 2) {
                median[channel] = static_cast<unsigned char>(sample);
                break;
            }
        }
        const double mean = sums[channel] / count;
        variance += std::max(0.0, squares[channel] / count - mean * mean) / 3;
    }
    const float luminance =
        static_cast<float>((0.0722 * sums[0] + 0.7152 * sums[1] + 0.2126 * sums[2]) / count);
    const Background exterior{RGB(median[2], median[1], median[0]), static_cast<float>(variance), luminance,
                              variance < 180.0};

    // On narrow colored strips or tight UI bars (e.g. a 15 px date ribbon between foreign panels), the
    // 6 px exterior ring lands on neighbouring panels instead of the strip's own surface. Sample smooth
    // low-gradient pixels inside the strip (emphasising its 1-2 px interior perimeter) and prefer that
    // cohesive interior surface whenever the exterior ring is noisy or disagrees sharply.
    const int boxW = Width(localRect), boxH = Height(localRect);
    if (std::min(boxW, boxH) <= 24 && std::max(boxW, boxH) >= 20) {
        std::array<size_t, 4096> smoothHist{};
        size_t smoothTotal = 0;
        const auto diffL1 = [](const unsigned char* a, const unsigned char* b) {
            return std::abs(int(a[2]) - int(b[2])) + std::abs(int(a[1]) - int(b[1])) +
                   std::abs(int(a[0]) - int(b[0]));
        };
        const auto binKey = [](const unsigned char* p) {
            return (int(p[2] >> 4) << 8) | (int(p[1] >> 4) << 4) | (p[0] >> 4);
        };
        for (int y = localRect.top; y < localRect.bottom; ++y) {
            for (int x = localRect.left; x < localRect.right; ++x) {
                const auto* p = image.bgra.data() + (static_cast<size_t>(y) * image.width + x) * 4;
                const auto* pl = image.bgra.data() + (static_cast<size_t>(y) * image.width + std::max(0, x - 1)) * 4;
                const auto* pr = image.bgra.data() +
                                 (static_cast<size_t>(y) * image.width + std::min(image.width - 1, x + 1)) * 4;
                const auto* pu = image.bgra.data() + (static_cast<size_t>(std::max(0, y - 1)) * image.width + x) * 4;
                const auto* pd = image.bgra.data() +
                                 (static_cast<size_t>(std::min(image.height - 1, y + 1)) * image.width + x) * 4;
                const int grad = std::max({diffL1(p, pl), diffL1(p, pr), diffL1(p, pu), diffL1(p, pd)});
                if (grad <= 28) {
                    const bool rim = (y < localRect.top + 2 || y >= localRect.bottom - 2 ||
                                      x < localRect.left + 2 || x >= localRect.right - 2);
                    const size_t w = rim ? 2 : 1;
                    smoothHist[binKey(p)] += w;
                    smoothTotal += w;
                }
            }
        }
        if (smoothTotal >= 24) {
            const auto modeIt = std::max_element(smoothHist.begin(), smoothHist.end());
            const int modeBin = int(modeIt - smoothHist.begin());
            const int mr = ((modeBin >> 8) & 15) * 16 + 8;
            const int mg = ((modeBin >> 4) & 15) * 16 + 8;
            const int mb = (modeBin & 15) * 16 + 8;
            std::array<std::array<size_t, 256>, 3> inHist{};
            std::array<double, 3> inSums{}, inSq{};
            size_t inCount = 0;
            for (int y = localRect.top; y < localRect.bottom; ++y) {
                for (int x = localRect.left; x < localRect.right; ++x) {
                    const auto* p = image.bgra.data() + (static_cast<size_t>(y) * image.width + x) * 4;
                    if (std::abs(int(p[2]) - mr) + std::abs(int(p[1]) - mg) + std::abs(int(p[0]) - mb) <= 56) {
                        for (int ch = 0; ch < 3; ++ch) {
                            const unsigned s = p[ch];
                            ++inHist[ch][s];
                            inSums[ch] += s;
                            inSq[ch] += s * s;
                        }
                        ++inCount;
                    }
                }
            }
            const size_t areaPx = static_cast<size_t>(boxW) * static_cast<size_t>(boxH);
            if (inCount >= 16 && inCount * 100 >= areaPx * 18) {
                std::array<unsigned char, 3> inMed{};
                double inVar = 0;
                for (int ch = 0; ch < 3; ++ch) {
                    size_t acc = 0;
                    for (int s = 0; s < 256; ++s) {
                        acc += inHist[ch][s];
                        if (acc >= (inCount + 1) / 2) {
                            inMed[ch] = static_cast<unsigned char>(s);
                            break;
                        }
                    }
                    const double m = inSums[ch] / inCount;
                    inVar += std::max(0.0, inSq[ch] / inCount - m * m) / 3;
                }
                const int extDiff = std::abs(int(inMed[2]) - int(median[2])) +
                                    std::abs(int(inMed[1]) - int(median[1])) +
                                    std::abs(int(inMed[0]) - int(median[0]));
                if (inVar < 240.0 && (variance >= 200.0 || extDiff >= 64)) {
                    const float inLum = static_cast<float>(
                        (0.0722 * inSums[0] + 0.7152 * inSums[1] + 0.2126 * inSums[2]) / inCount);
                    return {RGB(inMed[2], inMed[1], inMed[0]), static_cast<float>(inVar), inLum, inVar < 180.0};
                }
            }
        }
    }
    return exterior;
}
} // namespace sat
