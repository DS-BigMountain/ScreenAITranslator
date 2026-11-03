#pragma once
#include "common/Types.h"
#include <string_view>

namespace sat {
struct TargetLanguage {
    const char* id;
    const wchar_t* label;
    const char* english;
    const char* deepL;
};
const std::vector<TargetLanguage>& TextTargetLanguages();
const TargetLanguage& FindTextTarget(std::string_view id);
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
};
std::string TranslateContent(const ContentTranslationRequest&, const Settings&, const std::string& key,
                             std::stop_token stop);
} // namespace sat
