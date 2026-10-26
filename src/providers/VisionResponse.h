#pragma once
#include "common/Types.h"
#include <nlohmann/json.hpp>
#include <string_view>

namespace sat {
inline constexpr size_t TranslationSegmentLimit = 256;
inline constexpr size_t TranslationTextLimit = 8192;
inline constexpr size_t TranslationTotalTextLimit = 256 * 1024;

std::string_view UnwrapModelJson(std::string_view source);
struct VisionCoordinates {
    int width{1000}, height{1000};
    bool pixels{};
};
// Read the uploaded JPEG header so request and response use the same coordinate space.
VisionCoordinates UploadedImageCoordinates(const std::string& imageBase64);
// Text remains usable when the model's optional placement estimate is unavailable.
TranslationResult ParseVisionTranslation(const nlohmann::json& root, VisionCoordinates coordinates = {});
} // namespace sat
