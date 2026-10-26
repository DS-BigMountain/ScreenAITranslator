#include "RequestParameters.h"
#include <set>

namespace sat {
using Json = nlohmann::json;
namespace {
bool IsProtected(const std::string& name) {
    return name == "model" || name == "messages" || name == "contents" || name == "system" ||
           name == "system_instruction" || name == "systemInstruction" || name == "stream" ||
           name == "response_format";
}
bool HasThinkingOverride(const Json& patch) {
    return patch.contains("thinking") || patch.contains("reasoning_effort") ||
           patch.contains("enable_thinking");
}
} // namespace

Json ParseRequestParameters(const std::string& source) {
    if (source.size() > 32768)
        throw AppError("settings", "请求参数 JSON 不能超过 32 KiB。");
    if (source.find_first_not_of(" \t\r\n") == std::string::npos)
        return Json::object();
    try {
        std::vector<std::set<std::string>> keys;
        auto patch = Json::parse(source, [&](int depth, Json::parse_event_t event, Json& value) {
            if (depth > 16)
                throw AppError("settings", "请求参数 JSON 嵌套不能超过 16 层。");
            if (event == Json::parse_event_t::object_start)
                keys.emplace_back();
            else if (event == Json::parse_event_t::object_end)
                keys.pop_back();
            else if (event == Json::parse_event_t::key &&
                     !keys.back().insert(value.get<std::string>()).second)
                throw AppError("settings", "请求参数 JSON 包含重复字段。");
            return true;
        });
        if (!patch.is_object())
            throw AppError("settings", "请求参数必须是 JSON 对象，例如 {}。");
        for (const auto& item : patch.items())
            if (IsProtected(item.key()))
                throw AppError("settings", "请求参数不能覆盖 model、消息内容、stream 或响应格式。");
        if (patch.contains("generationConfig")) {
            const auto& config = patch.at("generationConfig");
            if (!config.is_object() || config.contains("responseMimeType") ||
                config.contains("responseSchema") || config.contains("responseJsonSchema"))
                throw AppError("settings", "generationConfig 必须是对象，且不能覆盖响应格式。");
        }
        return patch;
    } catch (const Json::parse_error& error) {
        throw AppError("settings", "请求参数 JSON 语法错误，字节位置：" + std::to_string(error.byte) + "。");
    } catch (const Json::exception&) {
        throw AppError("settings", "请求参数 JSON 含有超出支持范围的数值或无效内容。");
    }
}

void ApplyRequestParameters(Json& body, const std::string& source) {
    body.merge_patch(ParseRequestParameters(source));
}

void ConfigureCompatibleRequest(Json& body, const Settings& options, bool deepseek) {
    auto patch = ParseRequestParameters(options.requestParameters);
    if (deepseek || options.provider == "doubao" || options.provider == "zhipu" ||
        options.provider == "moonshot")
        body["thinking"] = {{"type", "disabled"}};
    else if (options.provider == "qwen" || options.provider == "siliconflow")
        body["enable_thinking"] = false;
    else if (!(options.provider == "openai" &&
               (options.model.starts_with("gpt-4") || options.model.starts_with("gpt-3.5"))))
        body["reasoning_effort"] = "none";

    // Explicit vendor fields replace the default thinking policy as a group.
    // This avoids sending a disabled default alongside the user's enabled flag.
    if (HasThinkingOverride(patch)) {
        body.erase("thinking");
        body.erase("reasoning_effort");
        body.erase("enable_thinking");
    }
    body.merge_patch(patch);
    const bool thinking = (body.contains("thinking") && body["thinking"].is_object() &&
                           body["thinking"].contains("type") && body["thinking"]["type"] != "disabled") ||
                          (body.contains("enable_thinking") && body["enable_thinking"] == true) ||
                          (body.contains("reasoning_effort") && body["reasoning_effort"] != "none" &&
                           !body["reasoning_effort"].is_null());
    if (thinking && !patch.contains("temperature"))
        body.erase("temperature");
}

void ConfigureNativeRequest(Json& body, const Settings& options, bool gemini) {
    if (gemini) {
        const auto model = options.model.starts_with("models/") ? options.model.substr(7) : options.model;
        auto patch = ParseRequestParameters(options.requestParameters);
        const bool override =
            patch.contains("generationConfig") && patch["generationConfig"].contains("thinkingConfig");
        if (!override && (model.starts_with("gemini-3") || model.starts_with("gemini-2.5-pro")))
            throw AppError("settings", "此 Gemini 模型不支持关闭思考；请更换模型，或在请求参数 JSON 的 "
                                       "generationConfig.thinkingConfig 中明确指定官方支持的配置。");
        if (!model.starts_with("gemini-1") && !model.starts_with("gemini-2.0"))
            body["generationConfig"]["thinkingConfig"] = {{"thinkingBudget", 0}};
        if (override)
            body["generationConfig"].erase("thinkingConfig");
        body.merge_patch(patch);
    } else {
        body["thinking"] = {{"type", "disabled"}};
        ApplyRequestParameters(body, options.requestParameters);
    }
}
} // namespace sat
