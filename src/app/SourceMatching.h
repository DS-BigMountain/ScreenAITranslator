#pragma once
#include "capture/TextDetection.h"
#include "common/Types.h"
#include <vector>
namespace sat {
// Result of pairing model passages with locally recognized text rows.
struct SourceMatchReport {
    size_t segments{};
    // Passages that received locally measured row geometry.
    size_t adopted{};
    // Source rows those passages occupy. One passage can own several rows when the translation
    // wraps inside the original paragraph.
    size_t matchedLines{};
    size_t rejectedText{};
    size_t rejectedSpace{};
    size_t rejectedBounds{};
    // Recognized rows left without a translated passage; the overlay covers them on its own.
    std::vector<RECT> uncoveredLines;
    std::vector<int> sourceIds;
};
// Adopts locally recognized row geometry for translated passages. Detection describes screen rows,
// the model describes passages, so the pairing happens at row level: every passage lands on the rows
// the artwork actually uses instead of the approximate model rectangle. Passages the local detector
// cannot explain keep the model rectangle.
SourceMatchReport AttachSourceGeometry(TranslationResult& result, int width, int height,
                                       const std::vector<TextRegion>& regions);
} // namespace sat
