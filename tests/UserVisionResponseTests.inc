// The user's browser result includes 52 translated passages and one right-edge overflow.
void UserVisionResponseTests() {
    std::ifstream file("test-fixtures/user/github-dashboard-ai-web-response.json");
    Require(bool(file), "user browser response fixture missing");
    const std::string source(std::istreambuf_iterator<char>(file), {});
    auto result = ParseTranslation(source);
    Require(result.segments.size() == 52, "geometry issue discarded user translation passages");
    Require(result.segments.back().box.x == 896 && result.segments.back().box.width == 104 &&
                result.segments.back().translated == "不要共享我的个人信息",
            "right-edge overflow was not clipped while preserving the translation");
    int calls = 0;
    CompatibleProvider provider(true, [&](auto&&...) {
        ++calls;
        return HttpResponse{200, Envelope(source)};
    });
    result = provider.Translate("aW1n", {}, Settings{}, "test-key", {});
    Require(calls == 1 && result.requestCount == 1 && result.segments.size() == 52,
            "completed user translation triggered another image request");
    auto variants = Json::parse(source);
    variants["segments"].back()["bbox"]["x"] = 1000;
    variants["segments"][0]["translated_text"] = "";
    variants["segments"][1]["original_text"] = "";
    variants["segments"][2].erase("bbox");
    variants["segments"].push_back({{"original_text", ""}, {"translated_text", ""}});
    result = ParseTranslation(variants.dump());
    Require(result.segments.size() == 52 && result.unpositionedSegments == 2 &&
                result.untranslatedSegments == 1 && result.ignoredSegments == 1 &&
                result.segments.back().positionUnavailable && result.segments.front().untranslated &&
                result.segments.front().translated == "Dashboard" && !result.segments[1].translated.empty(),
            "edge origins or empty model fields discarded completed user passages");
    Require(ParseTranslation("```json\n" + variants.dump() + "\n```").segments.size() == 52,
            "JSON fence caused an unnecessary rejection");
    TranslationContext context;
    TranslationResult incomplete;
    incomplete.segments = {result.segments.front(), result.segments[1]};
    context.Add(incomplete, 8);
    auto history = context.Get(true, 8);
    Require(history.empty(), "untranslated or missing-source text entered completed translation context");
    for (auto protocol : {ApiProtocol::Gemini, ApiProtocol::Anthropic}) {
        Settings options;
        options.provider = protocol == ApiProtocol::Gemini ? "gemini" : "anthropic";
        options.model = "native-test";
        int nativeCalls = 0;
        auto response = [&](const std::string& content) {
            return protocol == ApiProtocol::Gemini
                       ? Json{{"candidates",
                               Json::array({{{"content", {{"parts", Json::array({{{"text", content}}})}}}}})}}
                       : Json{{"content", Json::array({{{"type", "text"}, {"text", content}}})}};
        };
        NativeProvider native(protocol, [&](auto&&...) {
            ++nativeCalls;
            return HttpResponse{200, response(source).dump()};
        });
        auto translated = native.Translate("aW1n", {}, options, "test-key", {});
        Require(nativeCalls == 1 && translated.requestCount == 1 && translated.segments.size() == 52,
                "native API repeated image translation for a clipping issue");
        nativeCalls = 0;
        NativeProvider malformed(protocol, [&](auto&&...) {
            ++nativeCalls;
            return HttpResponse{200, response("{}").dump()};
        });
        Reject([&] { malformed.Translate("aW1n", {}, options, "test-key", {}); });
        Require(nativeCalls == 1, "native API repeated a malformed full-image generation");
    }
}
