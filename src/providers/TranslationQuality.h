#pragma once
#include "common/Types.h"
namespace sat {
// Detect clear untranslated prose without treating identifiers or existing Chinese as failures.
void FlagUntranslatedPassages(TranslationResult& result);
} // namespace sat
