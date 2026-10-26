#pragma once
#include "common/Types.h"
namespace sat {
struct TextLine {
    RECT rect{};
    std::string text;
    float confidence{};
};
struct TextRegion {
    int id{};
    RECT rect{};
    std::string text;
    std::vector<RECT> lines;
    float lineHeight{};
    Box imageBox{};
    float confidence{};
};
struct TextDetection {
    std::vector<TextRegion> regions;
    std::string status;
    size_t unrecognizedLines{};
};
std::vector<TextRegion> GroupTextLines(std::vector<TextLine> lines, const Image& image);
TextDetection DetectTextGeometry(const Image& image, std::stop_token stop = {});
TextDetection DetectText(const Image& image, std::stop_token stop = {}, int mode = 0);
// Production path: one detector pass, retaining geometry if recognition fails.
TextDetection DetectTextForTranslation(const Image& image, std::stop_token stop = {}, int mode = 0);
// Pure OCR: preserve recognized text regardless of confidence; never request AI.
TextDetection DetectOcrText(const Image& image, std::stop_token stop, int language);
} // namespace sat
