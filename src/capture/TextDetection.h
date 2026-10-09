#pragma once
#include "common/Types.h"
namespace sat {
struct TextLine { RECT rect{}; std::string text; };
struct TextRegion { int id{}; RECT rect{}; std::string text; std::vector<RECT> lines; float lineHeight{}; Box imageBox{}; };
struct TextDetection { std::vector<TextRegion> regions; std::string status; };
std::vector<TextRegion> GroupTextLines(std::vector<TextLine> lines, const Image& image);
TextDetection DetectTextGeometry(const Image& image, std::stop_token stop = {});
TextDetection DetectText(const Image& image, std::stop_token stop = {}, int mode = 0);
}
