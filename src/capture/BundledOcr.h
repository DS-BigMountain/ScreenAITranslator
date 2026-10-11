#pragma once
#include "TextDetection.h"
namespace sat {
// Stable language IDs are declared in common/OcrLanguage.h.
std::vector<TextLine> RecognizeBundled(const Image& image, int mode, std::stop_token stop, bool geometryOnly = false);
}
