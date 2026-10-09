#pragma once
#include "TextDetection.h"
namespace sat {
// mode: 0 = shared English/Japanese model, 1 = English, 2 = Japanese, 3 = direct AI.
std::vector<TextLine> RecognizeBundled(const Image& image, int mode, std::stop_token stop, bool geometryOnly = false);
}
