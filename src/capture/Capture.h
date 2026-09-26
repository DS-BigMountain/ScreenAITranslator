#pragma once
#include "common/Types.h"
namespace sat {
std::vector<Monitor> EnumerateMonitors();
Monitor MonitorAtCursor();
std::optional<RECT> RestoreRegion(const FixedRegion& region, const std::vector<Monitor>& monitors);
FixedRegion NormalizeRegion(const Monitor& monitor, RECT screenRect);
// All capture resources are created on demand and released with this call.
Image CaptureMonitor(const Monitor& monitor, std::stop_token stop);
Image Crop(const Image& image, RECT localRect);
std::vector<unsigned char> EncodeJpeg(const Image& image, int quality, std::stop_token stop);
std::string Base64(const std::vector<unsigned char>& bytes);
struct Background { COLORREF color{}; float variance{}, luminance{}; bool simple{}; };
Background AnalyzeBackground(const Image& image, RECT localRect);
}
