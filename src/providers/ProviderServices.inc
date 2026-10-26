// Included inside namespace sat, sharing the validated WinHTTP transport and parser.
namespace {
using NativeTransport =
    std::function<HttpResponse(const Settings&, const std::string&, const std::string&, Clock::time_point,
                               std::stop_token, const std::wstring&, const std::wstring&)>;
HttpResponse NativeHttp(const Settings& s, const std::string& key, const std::string& body,
                        Clock::time_point deadline, std::stop_token stop, const std::wstring& path,
                        const std::wstring& headers) {
    return SendHttpRequest(s, key, body, deadline, stop, L"POST", path.c_str(), headers);
}
std::wstring ServiceHeaders(ApiProtocol protocol, const Settings& s, const std::string& key) {
    ValidateApiKey(key);
    if (protocol == ApiProtocol::Gemini)
        return L"x-goog-api-key: " + Wide(key) + L"\r\n";
    if (protocol == ApiProtocol::Anthropic)
        return L"x-api-key: " + Wide(key) + L"\r\nanthropic-version: 2023-06-01\r\n";
    if (protocol == ApiProtocol::DeepL)
        return L"Authorization: DeepL-Auth-Key " + Wide(key) + L"\r\n";
    if (protocol == ApiProtocol::AzureTranslator) {
        if (!std::all_of(s.serviceRegion.begin(), s.serviceRegion.end(),
                         [](unsigned char c) { return std::isalnum(c) || c == '-'; }))
            throw AppError("settings", "Azure 区域格式无效");
        return L"Ocp-Apim-Subscription-Key: " + Wide(key) + L"\r\n" +
               (s.serviceRegion.empty()
                    ? std::wstring{}
                    : L"Ocp-Apim-Subscription-Region: " + Wide(s.serviceRegion) + L"\r\n");
    }
    return L"Authorization: Bearer " + Wide(key) + L"\r\n";
}
void RetryDelay(int seconds, Clock::time_point deadline, std::stop_token stop) {
    auto delay = std::chrono::seconds(std::max(1, seconds));
    if (Clock::now() + delay >= deadline)
        throw AppError("Requesting", "服务暂时繁忙，已达到本次请求时限");
    std::mutex mutex;
    std::condition_variable_any cv;
    std::unique_lock lock(mutex);
    cv.wait_for(lock, stop, delay, [] { return false; });
    CheckStop(stop);
}
std::string ModelPath(const std::string& model) {
    auto name = model.starts_with("models/") ? model.substr(7) : model;
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_' || c == '.';
        }))
        throw AppError("settings", "原生 API 模型名格式无效");
    return name;
}
Json NativeLlmBody(ApiProtocol protocol, const std::string& image, const std::vector<ContextItem>& context,
                   const Settings& s, const std::vector<TextRegion>& regions,
                   const std::vector<int>& repairIds = {}) {
    auto compatible = BuildRequest(image, context, s, false, regions, repairIds);
    auto system = compatible["messages"][0]["content"].get<std::string>();
    auto content = compatible["messages"][1]["content"];
    bool textOnly = content.is_string();
    std::string text = textOnly ? content.get<std::string>() : content[0]["text"].get<std::string>();
    if (protocol == ApiProtocol::Gemini) {
        Json parts = Json::array({{{"text", text}}});
        if (!textOnly)
            parts.push_back({{"inline_data", {{"mime_type", "image/jpeg"}, {"data", image}}}});
        Json config = {{"responseMimeType", "application/json"}};
        Json body = {{"system_instruction", {{"parts", Json::array({{{"text", system}}})}}},
                     {"contents", Json::array({{{"role", "user"}, {"parts", parts}}})},
                     {"generationConfig", config}};
        ConfigureNativeRequest(body, s, true);
        return body;
    }
    Json parts = Json::array();
    if (!textOnly)
        parts.push_back({{"type", "image"},
                         {"source", {{"type", "base64"}, {"media_type", "image/jpeg"}, {"data", image}}}});
    parts.push_back({{"type", "text"}, {"text", text}});
    Json body = {{"model", s.model},
                 {"max_tokens", 16384},
                 {"system", system},
                 {"messages", Json::array({{{"role", "user"}, {"content", parts}}})}};
    ConfigureNativeRequest(body, s, false);
    return body;
}
std::string NativeContent(ApiProtocol protocol, const Json& root) {
    try {
        if (protocol == ApiProtocol::Gemini) {
            const auto& candidate = root.at("candidates").at(0);
            if (candidate.value("finishReason", std::string("STOP")) != "STOP")
                throw AppError("Parsing", "模型响应未完整结束");
            std::string text;
            for (const auto& part : candidate.at("content").at("parts"))
                if (part.contains("text") && !part.value("thought", false))
                    text += part.at("text").get<std::string>();
            return text;
        }
        if (protocol == ApiProtocol::Anthropic) {
            if (root.value("stop_reason", std::string("end_turn")) != "end_turn")
                throw AppError("Parsing", "模型响应未完整结束");
            std::string text;
            for (const auto& part : root.at("content"))
                if (part.value("type", std::string{}) == "text")
                    text += part.at("text").get<std::string>();
            return text;
        }
        const auto& choice = root.at("choices").at(0);
        if (choice.value("finish_reason", std::string("stop")) != "stop")
            throw AppError("Parsing", "翻译响应未完整结束");
        return choice.at("message").at("content").get<std::string>();
    } catch (const Json::exception&) {
        FormatError();
    }
}
class NativeProvider final : public ITranslationProvider {
    ApiProtocol protocol_;
    NativeTransport transport_;

public:
    explicit NativeProvider(ApiProtocol protocol, NativeTransport transport = NativeHttp)
        : protocol_(protocol), transport_(std::move(transport)) {}
    TranslationResult Translate(const std::string& image, const std::vector<ContextItem>& context,
                                const Settings& s, const std::string& key, std::stop_token stop) override {
        return TranslateLocated(image, context, s, key, stop, {});
    }
    TranslationResult TranslateLocated(const std::string& image, const std::vector<ContextItem>& context,
                                       const Settings& s, const std::string& key, std::stop_token stop,
                                       const std::vector<TextRegion>& regions) override {
        CheckStop(stop);
        if (s.translationMode == TranslationOcrText && (!image.empty() || !HasRecognizedText(regions)))
            throw AppError("Preprocessing", "OCR 文字翻译仅接收已识别文字，不接收图片");
        ValidateApiKey(key);
        const auto deadline = Clock::now() + std::chrono::seconds(std::clamp(s.timeoutSeconds, 1, 300));
        const auto headers = ServiceHeaders(protocol_, s, key);
        std::atomic<int> attempts{};
        auto post = [&](const Json& body, const std::wstring& path, std::stop_token token) {
            for (int retry = 0;; ++retry) {
                CheckStop(token);
                if (Clock::now() >= deadline)
                    throw AppError("Requesting", "请求超时");
                auto number = ++attempts;
                DiagnosticSpan attempt(
                    "api_attempt", {{"attempt", number},
                                    {"provider", s.provider},
                                    {"model", s.model},
                                    {"remaining_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                         deadline - Clock::now())
                                                         .count()}});
                auto serialized = body.dump();
                HttpResponse response;
                try {
                    response = transport_(s, key, serialized, deadline, token, path, headers);
                    attempt.End("ok", {{"status", response.status},
                                       {"request_bytes", serialized.size()},
                                       {"response_bytes", response.body.size()}});
                } catch (const AppError& e) {
                    attempt.Fail(e);
                    throw;
                } catch (const Cancelled&) {
                    attempt.End("cancelled");
                    throw;
                }
                CheckStop(token);
                if (Clock::now() >= deadline)
                    throw AppError("Requesting", "请求超时");
                if (retry == 0 &&
                    (response.status == 429 || response.status == 502 || response.status == 503 ||
                     response.status == 504) &&
                    response.retryAfterSeconds <= 3) {
                    DiagnosticEvent("retry", {{"reason", "transient_http_error"},
                                              {"attempt", number},
                                              {"status", response.status}});
                    DiagnosticSpan delay("retry_delay");
                    RetryDelay(response.retryAfterSeconds, deadline, token);
                    delay.End();
                    continue;
                }
                if (response.status < 200 || response.status >= 300)
                    HttpError(response, !IsTextService(s.provider));
                try {
                    return Json::parse(response.body);
                } catch (const Json::exception&) {
                    FormatError();
                }
            }
        };
        if (protocol_ == ApiProtocol::Gemini || protocol_ == ApiProtocol::Anthropic) {
            const auto path = protocol_ == ApiProtocol::Gemini
                                  ? L"/models/" + Wide(ModelPath(s.model)) + L":generateContent"
                                  : L"/messages";
            CoverageRepair coverage;
            for (int retry = 0;; ++retry) {
                try {
                    auto body = NativeLlmBody(protocol_, image, context, s, regions, coverage.missing);
                    auto result = ParseTranslation(NativeContent(protocol_, post(body, path, stop)), regions,
                                                   s.translationMode == TranslationOcrText,
                                                   regions.empty() && s.translationMode == TranslationAiVision
                                                       ? UploadedImageCoordinates(image)
                                                       : VisionCoordinates{});
                    result = coverage.Accept(std::move(result), regions, s.translateAll);
                    result.requestCount = attempts;
                    return result;
                } catch (const AppError& error) {
                    if (error.stage != "Parsing" || regions.empty() || retry >= 1)
                        throw;
                    DiagnosticEvent(
                        "retry",
                        {{"reason", coverage.missing.empty() ? "invalid_response" : "coverage_repair"},
                         {"missing_ids", coverage.missing},
                         {"remaining_ms",
                          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now())
                              .count()}});
                }
            }
        }
        if (s.translationMode != TranslationOcrText || !HasRecognizedText(regions))
            throw AppError("Vision", "专用翻译服务仅用于 OCR 文字翻译模式");
        TranslationResult result;
        result.language = "auto";
        result.segments.resize(regions.size());
        for (size_t i = 0; i < regions.size(); ++i) {
            const auto& r = regions[i];
            auto& segment = result.segments[i];
            segment.original = r.text;
            segment.sourceId = r.id;
            segment.sourcePixels = r.rect;
            segment.sourceLines = r.lines;
            segment.sourceLineHeight = r.lineHeight;
            result.protectedRegions.push_back(r.rect);
        }
        if (protocol_ == ApiProtocol::QwenMt) {
            // Qwen-MT accepts one plain source passage, not a fabricated JSON protocol.
            // Three bounded workers preserve source IDs and share one overall deadline.
            std::atomic<size_t> next{};
            std::stop_source cancellation;
            std::stop_callback external(stop, [&] { cancellation.request_stop(); });
            std::vector<std::future<void>> workers;
            auto diagnosticOperation = DiagnosticOperation();
            for (size_t worker = 0; worker < std::min<size_t>(3, regions.size()); ++worker)
                workers.push_back(std::async(std::launch::async, [&, diagnosticOperation] {
                    DiagnosticContext context(diagnosticOperation);
                    try {
                        for (;;) {
                            auto i = next.fetch_add(1);
                            if (i >= regions.size())
                                return;
                            CheckStop(cancellation.get_token());
                            Json body = {
                                {"model", s.model},
                                {"messages", Json::array({{{"role", "user"}, {"content", regions[i].text}}})},
                                {"translation_options",
                                 {{"source_lang", "auto"}, {"target_lang", "Chinese"}}}};
                            result.segments[i].translated =
                                ReadText(Json(NativeContent(protocol_, post(body, L"/chat/completions",
                                                                            cancellation.get_token()))),
                                         MaxText);
                        }
                    } catch (...) {
                        cancellation.request_stop();
                        throw;
                    }
                }));
            std::exception_ptr failure;
            for (auto& worker : workers) {
                try {
                    worker.get();
                } catch (const Cancelled&) {
                    if (!failure)
                        failure = std::current_exception();
                } catch (...) {
                    failure = std::current_exception();
                }
            }
            if (failure)
                std::rethrow_exception(failure);
        } else {
            // Both translation APIs preserve array order. Limit batch size and bytes so
            // long screenshots stay within service limits without losing region mapping.
            for (size_t begin = 0; begin < regions.size();) {
                size_t end = begin, bytes = 0;
                Json texts = Json::array();
                while (end < regions.size() && end - begin < 50 && bytes + regions[end].text.size() < 48000) {
                    bytes += regions[end].text.size();
                    texts.push_back(protocol_ == ApiProtocol::DeepL ? Json(regions[end].text)
                                                                    : Json{{"Text", regions[end].text}});
                    ++end;
                }
                if (end == begin)
                    throw AppError("Requesting", "单个文字区域超出翻译服务限制");
                Json body =
                    protocol_ == ApiProtocol::DeepL
                        ? Json{{"text", texts}, {"target_lang", "ZH-HANS"}, {"split_sentences", "nonewlines"}}
                        : texts;
                if (protocol_ == ApiProtocol::DeepL && s.contextEnabled && !context.empty()) {
                    std::string previous;
                    for (const auto& item : context) {
                        if (previous.size() + item.original.size() > 16000)
                            break;
                        previous += item.original + '\n';
                    }
                    body["context"] = previous;
                }
                auto root = post(body,
                                 protocol_ == ApiProtocol::DeepL ? L"/translate"
                                                                 : L"/translate?api-version=3.0&to=zh-Hans",
                                 stop);
                try {
                    const auto& items = protocol_ == ApiProtocol::DeepL ? root.at("translations") : root;
                    if (!items.is_array() || items.size() != end - begin)
                        throw AppError("Parsing", "翻译服务返回的段落数量不完整");
                    for (size_t i = 0; i < items.size(); ++i)
                        result.segments[begin + i].translated = ReadText(
                            protocol_ == ApiProtocol::DeepL ? items[i].at("text")
                                                            : items[i].at("translations").at(0).at("text"),
                            MaxText);
                } catch (const Json::exception&) {
                    FormatError();
                }
                begin = end;
            }
        }
        CheckStop(stop);
        result.requestCount = attempts;
        return result;
    }
};
} // namespace
