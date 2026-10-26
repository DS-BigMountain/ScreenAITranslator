// Protocol regressions use the production parser and provider retry paths.
void CoverageTests() {
    std::vector<TextRegion> regions{
        {1, {10, 10, 150, 30}, "", {{10, 10, 150, 30}}, 20, {50, 50, 700, 100}, 0},
        {2, {10, 80, 150, 100}, "", {{10, 80, 150, 100}}, 20, {50, 400, 700, 100}, 0}};
    Json first = {
        {"source_language", "en"},
        {"segments",
         Json::array({{{"source_id", 1}, {"original_text", "First"}, {"translated_text", "第一段"}}})}};
    Json repair = {
        {"source_language", "en"},
        {"segments",
         Json::array({{{"source_id", 2}, {"original_text", "Second"}, {"translated_text", "第二段"}}})}};
    Settings settings;
    settings.provider = "qwen";
    auto userData = [](const Json& request) {
        auto content = request.at("messages").at(1).at("content");
        return Json::parse(content.is_string() ? content.get<std::string>()
                                               : content.at(0).at("text").get<std::string>());
    };
    int calls = 0;
    CompatibleProvider provider(
        false, [&](const Settings&, const std::string&, const std::string& body, auto, auto) {
            auto data = userData(Json::parse(body));
            ++calls;
            Require(data["source_regions"].size() == 2, "repair must retain neighboring passages as context");
            if (calls == 1)
                Require(!data.contains("requested_source_ids"), "normal request unexpectedly entered repair");
            else
                Require(data["requested_source_ids"] == Json::array({2}),
                        "repair regenerates already completed passages");
            return HttpResponse{200, Envelope((calls == 1 ? first : repair).dump())};
        });
    auto result = provider.TranslateLocated("aW1n", {}, settings, "test-key", {}, regions);
    Require(calls == 2 && result.requestCount == 2 && result.segments.size() == 2 &&
                result.segments[0].translated == "第一段" && result.segments[1].sourceId == 2,
            "geometry repair lost completed text or source ordering");
    int failedCalls = 0;
    CompatibleProvider missing(false, [&](auto&&...) {
        ++failedCalls;
        return HttpResponse{200, Envelope(first.dump())};
    });
    Reject([&] { missing.TranslateLocated("aW1n", {}, settings, "test-key", {}, regions); });
    Require(failedCalls == 2, "persistent omissions must fail after one repair");
    auto nontext = first;
    nontext["segments"].push_back({{"source_id", 2}, {"original_text", ""}, {"translated_text", ""}});
    int normalCalls = 0;
    CompatibleProvider complete(false, [&](auto&&...) {
        ++normalCalls;
        return HttpResponse{200, Envelope(nontext.dump())};
    });
    result = complete.TranslateLocated("aW1n", {}, settings, "test-key", {}, regions);
    Require(normalCalls == 1 && result.segments.size() == 1 && result.resolvedSourceIds.size() == 2,
            "explicit non-text confirmation must not cause extra requests");
    settings.translateAll = false;
    failedCalls = 0;
    Require(missing.TranslateLocated("aW1n", {}, settings, "test-key", {}, regions).segments.size() == 1 &&
                failedCalls == 1,
            "selective mode must remain available");
    settings.translateAll = true;
    auto mixed = regions;
    mixed[0].text = "First";
    mixed[0].confidence = .99f;
    auto compact = first;
    compact["segments"][0].erase("original_text");
    compact["segments"].push_back(repair["segments"][0]);
    auto compactResult = ParseTranslation(compact.dump(), mixed);
    Require(compactResult.segments[0].original == "First" && compactResult.segments[1].original == "Second",
            "compact vision output lost recognized or corrected originals");
    auto mixedRequest = userData(BuildRequest("aW1n", {}, settings, false, mixed));
    Require(mixedRequest["source_regions"][0]["reliable_ocr"] == true &&
                mixedRequest["source_regions"][1]["reliable_ocr"] == false,
            "mixed vision request lacks reliability routing");
    Reject([&] { ParseTranslation(compact.dump(), regions); });
    auto corrected = compact;
    corrected["segments"][0]["original_text"] = "Corrected first";
    Require(ParseTranslation(corrected.dump(), mixed).segments[0].original == "Corrected first",
            "compact vision protocol forbids correcting high-confidence OCR");
    for (auto protocol : {ApiProtocol::Gemini, ApiProtocol::Anthropic}) {
        settings.provider = protocol == ApiProtocol::Gemini ? "gemini" : "anthropic";
        settings.model = "test-model";
        int nativeCalls = 0;
        NativeProvider native(protocol, [&](const Settings&, const std::string&, const std::string& body,
                                            auto, auto, auto, auto) {
            ++nativeCalls;
            auto request = Json::parse(body);
            auto text = protocol == ApiProtocol::Gemini ? request["contents"][0]["parts"][0]["text"]
                                                        : request["messages"][0]["content"].back()["text"];
            if (nativeCalls == 2)
                Require(Json::parse(text.get<std::string>())["requested_source_ids"] == Json::array({2}),
                        "native repair missing targeted IDs");
            auto content = (nativeCalls == 1 ? first : repair).dump();
            Json root;
            if (protocol == ApiProtocol::Gemini)
                root = {{"candidates",
                         Json::array({{{"finishReason", "STOP"},
                                       {"content", {{"parts", Json::array({{{"text", content}}})}}}}})}};
            else
                root = {{"stop_reason", "end_turn"},
                        {"content", Json::array({{{"type", "text"}, {"text", content}}})}};
            return HttpResponse{200, root.dump()};
        });
        result = native.TranslateLocated("aW1n", {}, settings, "test-key", {}, regions);
        Require(nativeCalls == 2 && result.segments.size() == 2 && result.requestCount == 2,
                "native geometry omission accepted");
    }
    auto discovered = nontext;
    discovered["segments"].push_back({{"source_id", 0},
                                      {"original_text", "Missed caption"},
                                      {"translated_text", "漏检图注"},
                                      {"bbox", {{"x", 50}, {"y", 250}, {"width", 700}, {"height", 80}}}});
    result = ParseTranslation(discovered.dump(), regions);
    Require(result.segments.size() == 2 && result.segments[1].sourceId == 0 &&
                !result.segments[1].sourcePixels && result.segments[1].box.y == 250,
            "vision cannot add a locally missed passage");
    auto invalid = discovered;
    invalid["segments"].back()["bbox"]["width"] = 1000;
    Require(ParseTranslation(invalid.dump(), regions).segments.back().box.width == 950,
            "new passage outside the right edge lost its completed text");
    invalid = discovered;
    invalid["segments"].back()["source_id"] = 3;
    Reject([&] { ParseTranslation(invalid.dump(), regions); });
    for (auto& r : regions) {
        r.text = "Source";
        r.confidence = .99f;
    }
    Reject([&] { ParseTranslation(discovered.dump(), regions, true); });
    for (const char* finish : {"length", "content_filter", "tool_calls"}) {
        auto envelope = Json::parse(Envelope(ValidResult()));
        envelope["choices"][0]["finish_reason"] = finish;
        Reject([&] { ParseEnvelope(envelope.dump()); });
    }
    settings.provider = "qwen";
    int truncatedCalls = 0;
    CompatibleProvider truncated(false, [&](auto&&...) {
        ++truncatedCalls;
        auto response = Json::parse(Envelope(ValidResult()));
        response["choices"][0]["finish_reason"] = "length";
        return HttpResponse{200, response.dump()};
    });
    Reject([&] { truncated.Translate("aW1n", {}, settings, "test-key", {}); });
    Require(truncatedCalls == 1, "truncated direct vision response must not repeat image generation");
}
