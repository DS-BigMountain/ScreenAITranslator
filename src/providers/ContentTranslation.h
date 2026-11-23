#pragma once
#include "common/Types.h"
#include <string_view>

namespace sat {
struct TargetLanguage {
    const char* id;
    const wchar_t* label;
    const char* english;
    const char* deepL;
    char group;
};
const std::vector<TargetLanguage>& TextTargetLanguages();
const TargetLanguage& FindTextTarget(std::string_view id);
// Normalizes an arbitrary language code or name (e.g. "en", "English", "zh-CN") to a Chinese display label.
std::string LanguageDisplayName(std::string_view codeOrName);
struct TranslationAttachment {
    std::string name;
    std::string base64;
    std::string thumbnail;
};
struct ContentTranslationRequest {
    std::string text;
    std::string target{"zh-Hans"};
    std::vector<TranslationAttachment> images;
    std::vector<ContextItem> context;
    std::string source{"auto"};
};
std::string TranslateContent(const ContentTranslationRequest&, const Settings&, const std::string& key,
                             std::stop_token stop);
} // namespace sat
