#pragma once
#include "common/Types.h"
#include <nlohmann/json.hpp>

namespace sat {
// Optional JSON Merge Patch applied to the native request body after defaults.
// Null removes a default field; message content and response framing remain owned by the app.
nlohmann::json ParseRequestParameters(const std::string& source);
void ApplyRequestParameters(nlohmann::json& body, const std::string& source);
void ConfigureCompatibleRequest(nlohmann::json& body, const Settings& options, bool deepseek);
void ConfigureNativeRequest(nlohmann::json& body, const Settings& options, bool gemini);
} // namespace sat
