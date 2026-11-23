#pragma once
#include "capture/TextDetection.h"
#include "common/Types.h"
#include <string_view>
namespace sat {
// Detect clear untranslated prose without treating identifiers or existing Chinese as failures.
void FlagUntranslatedPassages(TranslationResult& result);
// Merge model-reported source_language codes and detected script languages into unique display names.
void PopulateDetectedLanguages(TranslationResult& result, const std::vector<TextRegion>& ocrRegions,
                               int ocrMode, std::string_view sourceHint);
} // namespace sat
