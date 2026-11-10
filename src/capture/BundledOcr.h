#pragma once
#include "TextDetection.h"
namespace sat {
// Stable language IDs are declared in common/OcrLanguage.h.
std::vector<TextLine> RecognizeBundled(const Image& image, int mode, std::stop_token stop,
                                       bool geometryOnly = false, bool preserveGeometry = false,
                                       bool retainLowConfidenceText = false,
                                       const std::vector<RECT>* precomputedBoxes = nullptr);
// Manga pipeline: a detector and recognizer trained on artwork and vertical columns. Empty when the
// weights are absent, which lets the caller fall back to the general engine.
std::vector<TextLine> RecognizeMangaBundled(const Image& image, std::stop_token stop);
// True when the manga weights are shipped beside the application.
bool MangaOcrAvailable();
} // namespace sat
