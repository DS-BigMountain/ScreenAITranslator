#include "VisionResponse.h"
#include "common/Diagnostics.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <wincrypt.h>

namespace sat {
namespace {
using Json = nlohmann::json;
[[noreturn]] void Invalid(const char* reason, const char* message) {
    DiagnosticEvent("response_rejected", {{"reason", reason}});
    throw AppError("Parsing", message);
}
bool HasText(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") != std::string::npos;
}
std::string TextField(const Json& item, const char* name) {
    const auto found = item.find(name);
    if (found == item.end() || found->is_null())
        return {};
    if (!found->is_string() && !found->is_number())
        return {};
    auto text = found->is_string() ? found->get<std::string>() : found->dump();
    if (text.size() > TranslationTextLimit || text.find('\0') != std::string::npos)
        Invalid("text_limit", "模型返回的单段文字超出长度限制或含无效字符");
    return HasText(text) ? text : std::string{};
}
bool ReadBox(const Json& item, Box& box, bool& adjusted, VisionCoordinates space) {
    auto found = item.find("bbox");
    if (found == item.end() || !found->is_object())
        return false;
    std::array<double, 4> values{};
    const char* names[]{"x", "y", "width", "height"};
    for (size_t i = 0; i < values.size(); ++i) {
        auto field = found->find(names[i]);
        if (field == found->end() || !field->is_number())
            return false;
        values[i] = field->get<double>();
        if (!std::isfinite(values[i]) || std::abs(values[i]) > 1000000)
            return false;
    }
    const auto [x, y, width, height] = values;
    if (width <= 0 || height <= 0)
        return false;
    const double left = std::clamp(x, 0., double(space.width)), top = std::clamp(y, 0., double(space.height));
    const double right = std::clamp(x + width, 0., double(space.width)),
                 bottom = std::clamp(y + height, 0., double(space.height));
    if (right <= left || bottom <= top)
        return false;
    const double sx = 1000. / space.width, sy = 1000. / space.height;
    box = {int(std::floor(left * sx)), int(std::floor(top * sy)),
           int(std::ceil(right * sx) - std::floor(left * sx)),
           int(std::ceil(bottom * sy) - std::floor(top * sy))};
    adjusted = left != x || top != y || right != x + width || bottom != y + height || left != box.x ||
               top != box.y || right != box.x + box.width || bottom != box.y + box.height;
    if (space.pixels)
        adjusted = left != x || top != y || right != x + width || bottom != y + height;
    return true;
}
} // namespace

VisionCoordinates UploadedImageCoordinates(const std::string& imageBase64) {
    // WIC-generated JPEG places SOF before image data. Decode a bounded prefix;
    // no image inference, image copying or metadata supplied by the model is needed.
    const DWORD length = DWORD(std::min<size_t>(imageBase64.size(), 65536));
    std::vector<unsigned char> bytes(length);
    DWORD decoded = length;
    if (!length || !CryptStringToBinaryA(imageBase64.data(), length, CRYPT_STRING_BASE64, bytes.data(),
                                         &decoded, nullptr, nullptr))
        return {};
    bytes.resize(decoded);
    if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8)
        return {};
    auto word = [&](size_t at) { return int(bytes[at]) * 256 + bytes[at + 1]; };
    for (size_t at = 2; at + 4 <= bytes.size();) {
        if (bytes[at++] != 0xff)
            return {};
        while (at < bytes.size() && bytes[at] == 0xff)
            ++at;
        if (at + 3 > bytes.size())
            return {};
        const auto marker = bytes[at++];
        if (marker == 0xda || marker == 0xd9)
            return {};
        const auto size = word(at);
        if (size < 2 || at + size > bytes.size())
            return {};
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
            if (size < 8)
                return {};
            const int height = word(at + 3), width = word(at + 5);
            return width > 0 && height > 0 ? VisionCoordinates{width, height, true} : VisionCoordinates{};
        }
        at += size;
    }
    return {};
}

std::string_view UnwrapModelJson(std::string_view source) {
    auto trim = [](std::string_view value) {
        auto first = value.find_first_not_of(" \t\r\n");
        return first == value.npos ? std::string_view{}
                                   : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    };
    source = trim(source);
    if (source.starts_with("\xEF\xBB\xBF"))
        source = trim(source.substr(3));
    if (source.starts_with("```") && source.ends_with("```")) {
        const auto line = source.find('\n');
        if (line != source.npos && line < source.size() - 3) {
            const auto label = trim(source.substr(3, line - 3));
            if (label.empty() || label == "json" || label == "JSON")
                source = trim(source.substr(line + 1, source.size() - line - 4));
        }
    }
    return source;
}

TranslationResult ParseVisionTranslation(const Json& root, VisionCoordinates coordinates) {
    if (coordinates.width <= 0 || coordinates.height <= 0)
        Invalid("coordinate_space", "图片坐标范围无效");
    if (!root.is_object() || !root.contains("segments") || !root["segments"].is_array())
        Invalid("segments_type", "模型返回中缺少有效的 segments 译文数组");
    const auto& items = root["segments"];
    if (items.size() > TranslationSegmentLimit)
        Invalid("segments_limit", "模型返回的译文片段数量超出限制");
    TranslationResult result;
    result.language = TextField(root, "source_language");
    if (result.language.empty() || result.language.size() > 64)
        result.language = "unknown";
    size_t total = 0;
    for (const auto& item : items) {
        if (!item.is_object()) {
            ++result.ignoredSegments;
            continue;
        }
        Segment segment;
        segment.original = TextField(item, "original_text");
        segment.translated = TextField(item, "translated_text");
        if (segment.original.empty() && segment.translated.empty()) {
            ++result.ignoredSegments;
            continue;
        }
        if (segment.translated.empty()) {
            segment.translated = segment.original;
            segment.untranslated = true;
            ++result.untranslatedSegments;
        }
        bool adjusted = false;
        segment.positionUnavailable = !ReadBox(item, segment.box, adjusted, coordinates);
        result.unpositionedSegments += segment.positionUnavailable;
        result.clippedBoxes += adjusted;
        total += segment.original.size() + segment.translated.size();
        if (total > TranslationTotalTextLimit)
            Invalid("total_text_limit", "模型返回的文字总长度超出限制");
        result.segments.push_back(std::move(segment));
    }
    if (result.segments.empty() && !items.empty())
        Invalid("no_usable_text", "模型返回了片段，但未提供有效原文或译文");
    if (result.clippedBoxes)
        DiagnosticEvent("response_geometry_adjusted", {{"clipped_boxes", result.clippedBoxes}});
    DiagnosticEvent("vision_response_summary", {{"segments", result.segments.size()},
                                                {"width", coordinates.width},
                                                {"height", coordinates.height},
                                                {"phase", coordinates.pixels ? "pixels" : "normalized"},
                                                {"clipped_boxes", result.clippedBoxes},
                                                {"unpositioned_segments", result.unpositionedSegments},
                                                {"untranslated_segments", result.untranslatedSegments},
                                                {"ignored_segments", result.ignoredSegments}});
    return result;
}
} // namespace sat
