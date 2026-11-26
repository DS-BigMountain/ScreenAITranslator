#pragma once
#include "common/Types.h"
namespace sat {
struct TextLine {
    RECT rect{};
    std::string text;
    float confidence{};
    OrientedBox obb{};
};
struct TextRegion {
    int id{};
    RECT rect{};
    std::string text;
    std::vector<RECT> lines;
    float lineHeight{};
    Box imageBox{};
    float confidence{};
    OrientedBox obb{};
};
struct TextDetection {
    std::vector<TextRegion> regions;
    std::string status;
    size_t unrecognizedLines{};
    std::vector<std::string> detectedLanguages;
    std::vector<RECT> uncoveredLines;
};
std::vector<std::string> DetectScriptLanguages(const std::vector<TextRegion>& regions, int ocrHint = OcrLocalAuto,
                                               std::string_view sourceHint = "auto");
std::vector<TextRegion> GroupTextLines(std::vector<TextLine> lines, const Image& image);
TextDetection DetectTextGeometry(const Image& image, std::stop_token stop = {});
TextDetection DetectText(const Image& image, std::stop_token stop = {}, int mode = 0);
// Production path: one detector pass, retaining geometry if recognition fails.
TextDetection DetectTextForTranslation(const Image& image, std::stop_token stop = {}, int mode = 0);
// Pure OCR: preserve recognized text regardless of confidence; never request AI.
TextDetection DetectOcrText(const Image& image, std::stop_token stop, int language);
} // namespace sat
