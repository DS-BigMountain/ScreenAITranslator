#include "ImageInput.h"
#include "Capture.h"
#include "common/Platform.h"
#include <wincodec.h>
#include <propvarutil.h>
#include <fstream>
#include <algorithm>
#include <cmath>

namespace sat {
namespace {
Image DecodeImage(IWICImagingFactory* factory, IWICBitmapSource* source, UINT longest, std::stop_token stop) {
    UINT width{}, height{};
    CheckHR(source->GetSize(&width, &height), "image");
    if (!width || !height || width > 100000 || height > 100000 ||
        static_cast<uint64_t>(width) * height > 256 * 1024 * 1024)
        throw AppError("Input", "图片尺寸无效或超过解码限制。");
    auto scale = std::min(1.0, static_cast<double>(longest) / std::max(width, height));
    width = std::max(1u, static_cast<UINT>(std::round(width * scale)));
    height = std::max(1u, static_cast<UINT>(std::round(height * scale)));
    ComPtr<IWICBitmapScaler> scaler;
    CheckHR(factory->CreateBitmapScaler(&scaler), "image");
    CheckHR(scaler->Initialize(source, width, height, WICBitmapInterpolationModeFant), "image");
    ComPtr<IWICFormatConverter> converter;
    CheckHR(factory->CreateFormatConverter(&converter), "image");
    CheckHR(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                  nullptr, 0, WICBitmapPaletteTypeCustom),
            "image");
    Image image;
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.bgra.resize(static_cast<size_t>(width) * height * 4);
    CheckStop(stop);
    CheckHR(
        converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.bgra.size()), image.bgra.data()),
        "image");
    // 透明背景按白色合成，避免 JPEG 丢弃透明通道后产生黑色文字背景。
    for (size_t i = 0; i < image.bgra.size(); i += 4) {
        auto alpha = image.bgra[i + 3];
        for (size_t c = 0; c < 3; ++c)
            image.bgra[i + c] =
                static_cast<unsigned char>((image.bgra[i + c] * alpha + 255 * (255 - alpha) + 127) / 255);
        image.bgra[i + 3] = 255;
    }
    CheckStop(stop);
    return image;
}
} // namespace
TranslationAttachment ImportImage(std::span<const unsigned char> bytes, std::string name,
                                  std::stop_token stop) {
    CheckStop(stop);
    if (bytes.empty() || bytes.size() > MaxInputImageBytes)
        throw AppError("Input", "单张图片不得超过 20 MB。");
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    CheckHR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
            "image");
    ComPtr<IWICStream> stream;
    CheckHR(factory->CreateStream(&stream), "image");
    CheckHR(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())),
            "image");
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                                &decoder)))
        throw AppError("Input", "无法读取图片。请使用 PNG、JPEG、BMP、GIF 或 TIFF 文件。");
    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHR(decoder->GetFrame(0, &frame), "image");
    auto image = DecodeImage(factory.Get(), frame.Get(), 2200, stop);
    ComPtr<IWICBitmap> bitmap;
    CheckHR(factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppBGRA,
                                            image.width * 4, static_cast<UINT>(image.bgra.size()),
                                            image.bgra.data(), &bitmap),
            "image");
    ComPtr<IWICMetadataQueryReader> metadata;
    WICBitmapTransformOptions rotation = WICBitmapTransformRotate0;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata))) {
        PROPVARIANT value{};
        if (SUCCEEDED(metadata->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value)) && value.vt == VT_UI2) {
            const WICBitmapTransformOptions transforms[]{
                WICBitmapTransformRotate0,
                WICBitmapTransformRotate0,
                WICBitmapTransformFlipHorizontal,
                WICBitmapTransformRotate180,
                WICBitmapTransformFlipVertical,
                static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 |
                                                       WICBitmapTransformFlipHorizontal),
                WICBitmapTransformRotate90,
                static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 |
                                                       WICBitmapTransformFlipHorizontal),
                WICBitmapTransformRotate270};
            if (value.uiVal < std::size(transforms))
                rotation = transforms[value.uiVal];
        }
        PropVariantClear(&value);
    }
    ComPtr<IWICBitmapFlipRotator> oriented;
    CheckHR(factory->CreateBitmapFlipRotator(&oriented), "image");
    CheckHR(oriented->Initialize(bitmap.Get(), rotation), "image");
    image = DecodeImage(factory.Get(), oriented.Get(), 2200, stop);
    auto encoded = EncodeJpeg(image, 2, stop);
    CheckHR(factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppBGRA,
                                            image.width * 4, static_cast<UINT>(image.bgra.size()),
                                            image.bgra.data(), bitmap.ReleaseAndGetAddressOf()),
            "image");
    auto thumbnail = DecodeImage(factory.Get(), bitmap.Get(), 256, stop);
    if (encoded.size() > 6 * 1024 * 1024)
        throw AppError("Input", "图片编码后超过大小限制，请先缩小图片。");
    return {std::move(name), Base64(encoded), Base64(EncodeJpeg(thumbnail, 0, stop))};
}
TranslationAttachment ImportImageFile(const std::filesystem::path& path, std::stop_token stop) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    auto size = input.tellg();
    if (!input || size <= 0 || size > static_cast<std::streamoff>(MaxInputImageBytes))
        throw AppError("Input", "无法读取图片，或文件超过 20 MB。");
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), size))
        throw AppError("Input", "图片文件读取失败。");
    return ImportImage(bytes, Utf8(path.filename().wstring()), stop);
}
} // namespace sat
