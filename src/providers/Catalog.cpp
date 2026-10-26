#include "Catalog.h"
#include <algorithm>
namespace sat {
const std::vector<ProviderPreset>& ProviderPresets() {
    static const std::vector<ProviderPreset> values{
        {"deepseek", L"DeepSeek", L"https://api.deepseek.com", "deepseek-flash", ApiProtocol::DeepSeek},
        {"openai-compatible", L"自定义 OpenAI Compatible", L"https://api.openai.com/v1", "",
         ApiProtocol::Compatible},
        {"openai", L"OpenAI", L"https://api.openai.com/v1", "", ApiProtocol::Compatible},
        {"qwen", L"阿里云百炼 / 通义千问", L"https://dashscope.aliyuncs.com/compatible-mode/v1", "",
         ApiProtocol::Compatible},
        {"doubao", L"火山方舟 / 豆包", L"https://ark.cn-beijing.volces.com/api/v3", "",
         ApiProtocol::Compatible},
        {"moonshot", L"Moonshot / Kimi", L"https://api.moonshot.cn/v1", "", ApiProtocol::Compatible},
        {"zhipu", L"智谱 / GLM", L"https://open.bigmodel.cn/api/paas/v4", "", ApiProtocol::Compatible},
        {"siliconflow", L"硅基流动", L"https://api.siliconflow.cn/v1", "", ApiProtocol::Compatible},
        {"gemini", L"Google Gemini（原生 API）", L"https://generativelanguage.googleapis.com/v1beta", "",
         ApiProtocol::Gemini},
        {"anthropic", L"Anthropic Claude（原生 API）", L"https://api.anthropic.com/v1", "",
         ApiProtocol::Anthropic},
        {"deepl", L"DeepL 翻译", L"https://api-free.deepl.com/v2", "translate", ApiProtocol::DeepL},
        {"azure-translator", L"Microsoft Azure Translator", L"https://api.cognitive.microsofttranslator.com",
         "translate", ApiProtocol::AzureTranslator},
        {"qwen-mt", L"阿里云 Qwen-MT 专用翻译", L"https://dashscope.aliyuncs.com/compatible-mode/v1",
         "qwen-mt-turbo", ApiProtocol::QwenMt}};
    return values;
}
const ProviderPreset& Preset(std::string_view id) {
    const auto& values = ProviderPresets();
    auto found = std::find_if(values.begin(), values.end(), [&](const auto& p) { return id == p.id; });
    if (found == values.end())
        throw AppError("settings", "不支持的服务提供方");
    return *found;
}
bool IsTextService(std::string_view id) {
    auto p = Preset(id).protocol;
    return p == ApiProtocol::DeepL || p == ApiProtocol::AzureTranslator || p == ApiProtocol::QwenMt;
}
} // namespace sat
