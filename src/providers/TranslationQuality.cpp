#include "TranslationQuality.h"
#include "common/Platform.h"
#include "common/Diagnostics.h"
#include <algorithm>
#include <cctype>
#include <string_view>

namespace sat {
namespace {
bool HasHan(const std::wstring& text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t ch) { return ch >= 0x3400 && ch <= 0x9fff; });
}
bool LooksLikeEnglishProse(const std::string& text) {
    int words = 0, common = 0;
    std::string word;
    constexpr std::string_view vocabulary[]{"the",  "this",   "that",   "please",     "during",     "with",
                                            "from", "cannot", "failed", "connection", "terminated", "error",
                                            "not",  "found",  "your",   "will",       "is",         "are"};
    auto flush = [&] {
        if (word.size() >= 2) {
            ++words;
            common += std::find(std::begin(vocabulary), std::end(vocabulary), word) != std::end(vocabulary);
        }
        word.clear();
    };
    for (unsigned char ch : text) {
        if (ch < 128 && std::isalpha(ch))
            word += static_cast<char>(std::tolower(ch));
        else
            flush();
    }
    flush();
    return words >= 4 && common >= 1;
}
} // namespace
void FlagUntranslatedPassages(TranslationResult& result) {
    for (auto& segment : result.segments) {
        if (segment.untranslated || segment.original.empty())
            continue;
        const auto original = Wide(segment.original), translated = Wide(segment.translated);
        const auto foreignScript = std::count_if(original.begin(), original.end(), [](wchar_t ch) {
            return (ch >= 0x3040 && ch <= 0x30ff) || (ch >= 0xac00 && ch <= 0xd7af);
        });
        const bool unchangedForeign = foreignScript >= 4 && segment.original == segment.translated;
        const bool englishOnly = !HasHan(translated) && LooksLikeEnglishProse(segment.original);
        if (unchangedForeign || englishOnly) {
            segment.untranslated = true;
            ++result.untranslatedSegments;
        }
    }
    DiagnosticEvent("translation_quality", {{"untranslated_segments", result.untranslatedSegments}});
}
} // namespace sat
