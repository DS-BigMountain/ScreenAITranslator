#include "capture/ImageInput.h"
namespace {
void ContentTranslationTests() {
    const std::string translated = "Translated first paragraph.\n\nTranslated second paragraph.";
    for (const auto& id :
         {"openai-compatible", "deepseek", "gemini", "anthropic", "deepl", "azure-translator", "qwen-mt"}) {
        Settings settings;
        settings.provider = id;
        settings.model = id == std::string("gemini") ? "gemini-2.0-flash" : "test-model";
        auto protocol = Preset(id).protocol;
        for (const auto& language : TextTargetLanguages()) {
            ContentTranslationRequest request{"source first\n\nsource second", language.id};
            request.context = {{"previous source", "previous translation"}};
            int calls = 0;
            NativeTransport transport = [&](const Settings&, const std::string&, const std::string& data,
                                            Clock::time_point, std::stop_token, const std::wstring& path,
                                            const std::wstring&) {
                ++calls;
                auto body = Json::parse(data);
                Require(!body.contains("response_format"),
                        "text translation must not use screenshot JSON schema");
                if (protocol == ApiProtocol::DeepL) {
                    Require(body["text"][0] == request.text && body["target_lang"] == language.deepL &&
                                path == L"/translate",
                            "DeepL target or plain source changed");
                    return HttpResponse{200,
                                        Json{{"translations", Json::array({{{"text", translated}}})}}.dump()};
                }
                if (protocol == ApiProtocol::AzureTranslator) {
                    Require(body[0]["Text"] == request.text && path.ends_with(Wide(language.id)),
                            "Azure target or plain source changed");
                    return HttpResponse{
                        200, Json::array({{{"translations", Json::array({{{"text", translated}}})}}}).dump()};
                }
                if (protocol == ApiProtocol::QwenMt) {
                    Require(body["messages"][0]["content"] == request.text &&
                                body["translation_options"]["target_lang"] ==
                                    (request.target == "zh-Hans" ? "Chinese" : language.english),
                            "Qwen-MT must receive plain text and selected target");
                    return HttpResponse{200, Envelope(translated)};
                }
                std::string system;
                if (protocol == ApiProtocol::Gemini) {
                    system = body["system_instruction"]["parts"][0]["text"];
                    Require(!body.value("generationConfig", Json::object()).contains("responseMimeType"),
                            "Gemini text output was forced to JSON");
                } else if (protocol == ApiProtocol::Anthropic)
                    system = body["system"];
                else
                    system = body["messages"][0]["content"];
                Require(system.find(language.english) != system.npos &&
                            system.find("Output only") != system.npos,
                        "LLM request lost target language or translation-only instruction");
                Require(data.find("previous translation") != data.npos &&
                            data.find("source first") != data.npos,
                        "LLM request lost source or isolated context");
                if (protocol == ApiProtocol::Gemini)
                    return HttpResponse{
                        200,
                        Json{{"candidates",
                              Json::array({{{"finishReason", "STOP"},
                                            {"content",
                                             {{"parts",
                                               Json::array({{{"thought", true}, {"text", "hidden reasoning"}},
                                                            {{"text", translated}}})}}}}})}}
                            .dump()};
                if (protocol == ApiProtocol::Anthropic)
                    return HttpResponse{
                        200, Json{{"stop_reason", "end_turn"},
                                  {"content",
                                   Json::array({{{"type", "thinking"}, {"thinking", "hidden reasoning"}},
                                                {{"type", "text"}, {"text", translated}}})}}
                                 .dump()};
                return HttpResponse{200, Envelope(translated)};
            };
            Require(TranslateContentWithTransport(request, settings, "test-key", {}, transport) ==
                            translated &&
                        calls == 1,
                    "plain translation result or paragraph boundaries changed");
        }
    }
    auto image = ImportImageFile("test-fixtures/user/handshake-error-en.png", {});
    Require(!image.base64.empty() && !image.thumbnail.empty() && image.base64.size() < 8 * 1024 * 1024,
            "real screenshot import failed or exceeded upload bounds");
    for (const auto& id : {"openai-compatible", "gemini", "anthropic"}) {
        Settings s;
        s.provider = id;
        s.model = "test-image-model";
        auto protocol = Preset(id).protocol;
        ContentTranslationRequest request{"", "ja", {image, image}};
        NativeTransport transport = [&](const Settings&, const std::string&, const std::string& data,
                                        Clock::time_point, std::stop_token, const std::wstring&,
                                        const std::wstring&) {
            auto body = Json::parse(data);
            if (protocol == ApiProtocol::Gemini) {
                auto parts = body["contents"][0]["parts"];
                Require(parts.size() == 3 && parts[1]["inline_data"]["data"] == image.base64 &&
                            parts[2]["inline_data"]["mime_type"] == "image/jpeg",
                        "Gemini multiple images lost");
                return HttpResponse{
                    200,
                    Json{{"candidates",
                          Json::array({{{"content", {{"parts", Json::array({{{"text", "翻訳結果"}}})}}}}})}}
                        .dump()};
            }
            auto parts = body["messages"][protocol == ApiProtocol::Anthropic ? 0 : 1]["content"];
            Require(parts.size() == 3, "multiple images not represented in native request");
            if (protocol == ApiProtocol::Anthropic) {
                Require(parts[1]["source"]["data"] == image.base64, "Claude image bytes changed");
                return HttpResponse{
                    200, Json{{"content", Json::array({{{"type", "text"}, {"text", "翻訳結果"}}})}}.dump()};
            }
            Require(parts[1]["image_url"]["url"] == "data:image/jpeg;base64," + image.base64,
                    "compatible image request is not a JPEG data URL");
            return HttpResponse{200, Envelope("翻訳結果")};
        };
        Require(TranslateContentWithTransport(request, s, "test-key", {}, transport) == "翻訳結果",
                "image translation did not return plain target-language text");
    }
    Settings settings;
    ContentTranslationRequest request{"source", "en"};
    NativeTransport unused = [](auto&&...) -> HttpResponse {
        throw std::runtime_error("unexpected network request");
    };
    auto invalid = request;
    invalid.target = "en&injected=value";
    Reject([&] { TranslateContentWithTransport(invalid, settings, "test-key", {}, unused); }, "settings");
    invalid = request;
    invalid.text.assign(65537, 'x');
    Reject([&] { TranslateContentWithTransport(invalid, settings, "test-key", {}, unused); }, "Input");
    invalid = request;
    invalid.images = {image};
    settings.provider = "deepl";
    Reject([&] { TranslateContentWithTransport(invalid, settings, "test-key", {}, unused); }, "Input");
    settings.provider = "openai-compatible";
    std::stop_source cancelled;
    cancelled.request_stop();
    bool stopped = false;
    try {
        TranslateContentWithTransport(request, settings, "test-key", cancelled.get_token(), unused);
    } catch (const Cancelled&) {
        stopped = true;
    }
    Require(stopped, "cancelled content request reached network");
    for (const auto& body :
         {Envelope(" "), std::string("{}"),
          Json{{"choices",
                Json::array({{{"finish_reason", "length"}, {"message", {{"content", "partial"}}}}})}}
              .dump()}) {
        NativeTransport malformed = [&](auto&&...) { return HttpResponse{200, body}; };
        Reject([&] { TranslateContentWithTransport(request, settings, "test-key", {}, malformed); });
    }
    std::vector<unsigned char> badImage{1, 2, 3, 4};
    Reject([&] { ImportImage(badImage, "invalid.png", {}); }, "Input");
    badImage.resize(MaxInputImageBytes + 1);
    Reject([&] { ImportImage(badImage, "oversized.png", {}); }, "Input");
}
} // namespace
