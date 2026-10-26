#pragma once
#include "common/Types.h"
#include <string_view>
namespace sat {
enum class ApiProtocol { Compatible, DeepSeek, Gemini, Anthropic, DeepL, AzureTranslator, QwenMt };
struct ProviderPreset {
    const char* id;
    const wchar_t* name;
    const wchar_t* url;
    const char* model;
    ApiProtocol protocol;
};
const std::vector<ProviderPreset>& ProviderPresets();
const ProviderPreset& Preset(std::string_view id);
bool IsTextService(std::string_view id);
} // namespace sat
