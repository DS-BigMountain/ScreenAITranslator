void RequestParameterTests() {
    Settings options;
    for (const auto* provider :
         {"deepseek", "qwen", "siliconflow", "doubao", "moonshot", "zhipu", "openai", "openai-compatible"}) {
        options.provider = provider;
        Json body = {{"temperature", .1}, {"messages", Json::array()}};
        ConfigureCompatibleRequest(body, options, options.provider == "deepseek");
        if (options.provider == "qwen" || options.provider == "siliconflow")
            Require(body["enable_thinking"] == false, "compatible vendor thinking defaults off");
        else if (options.provider == "openai" || options.provider == "openai-compatible")
            Require(body["reasoning_effort"] == "none", "generic reasoning defaults none");
        else
            Require(body["thinking"]["type"] == "disabled", "vendor thinking explicitly disabled");
    }
    options.provider = "deepseek";
    options.requestParameters =
        R"({"thinking":{"type":"enabled"},"reasoning_effort":"high","max_tokens":2048})";
    Json body = {{"temperature", .1}, {"messages", Json::array()}};
    ConfigureCompatibleRequest(body, options, true);
    Require(body["thinking"]["type"] == "enabled" && body["reasoning_effort"] == "high" &&
                body["max_tokens"] == 2048 && !body.contains("temperature"),
            "explicit thinking fields override defaults without unsupported sampling");
    options.provider = "openai-compatible";
    options.requestParameters = R"({"enable_thinking":true,"temperature":0.5})";
    body = {{"temperature", .1}};
    ConfigureCompatibleRequest(body, options, false);
    Require(body["enable_thinking"] == true && !body.contains("reasoning_effort") &&
                body["temperature"] == .5,
            "custom endpoint vendor switch replaces default and respects explicit sampling");
    options.requestParameters = R"({"reasoning_effort":null,"temperature":null})";
    body = {{"temperature", .1}};
    ConfigureCompatibleRequest(body, options, false);
    Require(!body.contains("reasoning_effort") && !body.contains("temperature"), "null removes defaults");

    for (const auto* malformed :
         {"[]", "null", "{bad}", R"({"a":1,"a":2})", R"({"a":{"x":1,"x":2}})", R"({"messages":[]})",
          R"({"stream":true})", R"({"response_format":null})", R"({"generationConfig":null})",
          R"({"generationConfig":{"responseMimeType":"text/plain"}})"})
        Reject([&] { ParseRequestParameters(malformed); }, "settings");
    Reject([&] { ParseRequestParameters(std::string(32769, ' ')); }, "settings");
    Require(ParseRequestParameters(" \n ").empty(), "empty editor uses defaults");
    Require(ParseRequestParameters(R"({"a":{"x":1},"b":{"x":2}})")["b"]["x"] == 2,
            "duplicate names in different objects remain valid");

    options = Settings{};
    options.provider = "gemini";
    options.model = "gemini-2.5-flash";
    body = NativeLlmBody(ApiProtocol::Gemini, "aW1n", {}, options, {});
    Require(body["generationConfig"]["thinkingConfig"]["thinkingBudget"] == 0,
            "Gemini thinking explicitly off");
    options.model = "gemini-3-pro-preview";
    Reject([&] { NativeLlmBody(ApiProtocol::Gemini, "aW1n", {}, options, {}); }, "settings");
    options.requestParameters =
        R"({"generationConfig":{"thinkingConfig":{"thinkingLevel":"low"},"temperature":0.2}})";
    body = NativeLlmBody(ApiProtocol::Gemini, "aW1n", {}, options, {});
    Require(body["generationConfig"]["thinkingConfig"]["thinkingLevel"] == "low" &&
                !body["generationConfig"]["thinkingConfig"].contains("thinkingBudget") &&
                body["generationConfig"]["responseMimeType"] == "application/json" &&
                body["generationConfig"]["temperature"] == .2,
            "native nested configuration overrides thinking while retaining JSON framing");
    options.requestParameters = "{}";
    options.provider = "anthropic";
    body = NativeLlmBody(ApiProtocol::Anthropic, "aW1n", {}, options, {});
    Require(body["thinking"]["type"] == "disabled", "Claude thinking explicitly off");
    options.requestParameters = R"({"thinking":{"type":"enabled","budget_tokens":2048},"max_tokens":4096})";
    body = NativeLlmBody(ApiProtocol::Anthropic, "aW1n", {}, options, {});
    Require(body["thinking"]["type"] == "enabled" && body["thinking"]["budget_tokens"] == 2048 &&
                body["max_tokens"] == 4096,
            "Claude native overrides retained");

    options.fallbackEnabled = true;
    options.fallbackRequestParameters = R"({"thinking":{"type":"disabled"}})";
    auto selected = SelectTranslationConnection(options);
    Require(selected.requestParameters == options.fallbackRequestParameters,
            "independent connection uses its own parameters");
}
