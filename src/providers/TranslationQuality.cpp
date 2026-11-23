#include "TranslationQuality.h"
#include "common/Platform.h"
#include "common/Diagnostics.h"
#include "providers/ContentTranslation.h"
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
        // An empty answer leaves the source passage in place instead of blanking the surface.
        if (segment.translated.empty()) {
            segment.untranslated = true;
            ++result.untranslatedSegments;
            continue;
        }
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
namespace {
bool IsSpecificLatinDisplayName(std::string_view display) {
    return display != "简体中文" && display != "繁体中文" && display != "日语" && display != "韩语" &&
           display != "俄语" && display != "乌克兰语" && display != "保加利亚语" &&
           display != "塞尔维亚语" && display != "白俄罗斯语" && display != "阿拉伯语" &&
           display != "波斯语" && display != "乌尔都语" && display != "希伯来语" && display != "泰语" &&
           display != "希腊语" && display != "印地语" && display != "泰米尔语" && display != "泰卢固语" &&
           display != "孟加拉语" && display != "马拉地语" && display != "旁遮普语" &&
           display != "古吉拉特语" && display != "卡纳达语" && display != "马拉雅拉姆语" &&
           display != "僧伽罗语" && display != "尼泊尔语" && display != "缅甸语" && display != "高棉语" &&
           display != "老挝语" && display != "格鲁吉亚语" && display != "亚美尼亚语" &&
           display != "意第绪语" && display != "阿姆哈拉语" && display != "哈萨克语" &&
           display != "吉尔吉斯语" && display != "蒙古语";
}
} // namespace
void PopulateDetectedLanguages(TranslationResult& result, const std::vector<TextRegion>& ocrRegions,
                               int ocrMode, std::string_view sourceHint) {
    result.detectedLanguages.clear();
    auto addName = [&](std::string name) {
        if (name.empty())
            return;
        if (std::find(result.detectedLanguages.begin(), result.detectedLanguages.end(), name) ==
            result.detectedLanguages.end())
            result.detectedLanguages.push_back(std::move(name));
    };
    auto addCodeList = [&](std::string_view raw) {
        size_t start = 0;
        while (start < raw.size()) {
            const size_t sep = raw.find_first_of(",;/|+", start);
            const auto part =
                sep == std::string_view::npos ? raw.substr(start) : raw.substr(start, sep - start);
            addName(LanguageDisplayName(part));
            if (sep == std::string_view::npos)
                break;
            start = sep + 1;
        }
    };
    addCodeList(result.language);
    auto appendScriptCodes = [&](const std::vector<std::string>& codes) {
        const bool alreadyHasLatin =
            std::any_of(result.detectedLanguages.begin(), result.detectedLanguages.end(),
                        [](const std::string& name) { return IsSpecificLatinDisplayName(name); });
        for (const auto& code : codes) {
            if (code == "en" && alreadyHasLatin)
                continue;
            addName(LanguageDisplayName(code));
        }
    };
    if (!ocrRegions.empty())
        appendScriptCodes(DetectScriptLanguages(ocrRegions, ocrMode, sourceHint));
    if (!result.segments.empty()) {
        std::vector<TextRegion> segmentRegions;
        segmentRegions.reserve(result.segments.size());
        for (const auto& seg : result.segments) {
            if (!seg.original.empty()) {
                TextRegion tr;
                tr.text = seg.original;
                segmentRegions.push_back(std::move(tr));
            }
        }
        if (!segmentRegions.empty())
            appendScriptCodes(DetectScriptLanguages(segmentRegions, ocrMode, sourceHint));
    }
    if ((result.language.empty() || result.language == "unknown") && !result.detectedLanguages.empty())
        result.language = result.detectedLanguages.front();
}
} // namespace sat
