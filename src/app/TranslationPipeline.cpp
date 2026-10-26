#include "TranslationPipeline.h"
#include "capture/Capture.h"
#include "common/Diagnostics.h"
#include "common/Secret.h"
#include "providers/TranslationQuality.h"
#include "settings/Store.h"
#include <algorithm>

namespace sat {
namespace {
using Clock = std::chrono::steady_clock;
long long ElapsedMs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

TextDetection DetectSource(const Image& image, int mode, std::stop_token stop) {
    DiagnosticSpan ocr("ocr");
    auto detected = DetectOcrText(image, stop, mode);
    size_t emptyRegions = 0;
    std::vector<int> lowConfidence;
    for (const auto& region : detected.regions) {
        emptyRegions += region.text.empty();
        if (region.text.empty() || !(region.confidence >= .85f && region.confidence <= 1.f))
            lowConfidence.push_back(region.id);
    }
    ocr.End("ok", {{"ocr_status", detected.status},
                   {"regions", detected.regions.size()},
                   {"empty_regions", emptyRegions},
                   {"low_confidence", lowConfidence.size()},
                   {"unrecognized_lines", detected.unrecognizedLines},
                   {"low_confidence_ids", lowConfidence}});
    return detected;
}

std::string EncodeForVision(const Image& image, int quality, std::stop_token stop) {
    DiagnosticSpan encoding("image_encode");
    auto jpeg = EncodeJpeg(image, quality, stop);
    auto data = Base64(jpeg);
    encoding.End("ok", {{"jpeg_bytes", jpeg.size()}});
    return data;
}
} // namespace

PreparedTranslation PrepareTranslation(Image image, RECT region, const Monitor& monitor,
                                       const Settings& options, const std::vector<ContextItem>& history,
                                       const std::string& key, std::stop_token stop,
                                       const ProviderFactory& createProvider,
                                       const std::function<void(TranslationPhase)>& phaseChanged) {
    const auto start = Clock::now();
    DiagnosticSpan translation("translation", {{"width", image.width},
                                               {"height", image.height},
                                               {"ocr_mode", options.ocrMode},
                                               {"translation_mode", options.translationMode},
                                               {"quality", options.quality},
                                               {"timeout_s", options.timeoutSeconds},
                                               {"translate_all", options.translateAll},
                                               {"context_count", history.size()}});
    try {
        CheckStop(stop);
        PreparedTranslation prepared;
        if (!ValidTranslationMode(options.translationMode))
            throw AppError("settings", "翻译模式无效");
        prepared.textOnly = options.translationMode == TranslationOcrText;
        auto connection = SelectTranslationConnection(options);
        TextDetection detected;
        std::vector<RECT> detectedAreas;
        if (prepared.textOnly) {
            auto ocrStart = Clock::now();
            detected = DetectSource(image, options.ocrMode, stop);
            prepared.ocrMs = ElapsedMs(ocrStart);
            prepared.unrecognizedLines = detected.unrecognizedLines;
            for (const auto& source : detected.regions) {
                detectedAreas.push_back(source.rect);
                if (!source.text.empty() && !(source.confidence >= .85f && source.confidence <= 1.f))
                    ++prepared.lowConfidenceRegions;
            }
            std::erase_if(detected.regions, [](const auto& source) { return source.text.empty(); });
            if (detected.regions.empty())
                throw AppError("ocr", "本地 OCR 未获得可翻译文字，请检查原文语言或手动切换 AI 识图翻译");
        }
        DiagnosticEvent("route_selected",
                        {{"route", prepared.textOnly ? "text" : "vision"},
                         {"reason", prepared.textOnly ? "explicit_ocr_text" : "explicit_ai_vision"},
                         {"provider", connection.provider},
                         {"model", connection.model},
                         {"fallback", !prepared.textOnly && options.fallbackEnabled},
                         {"proxy_mode", connection.proxyMode}});
        Secret selectedKey(!prepared.textOnly && options.fallbackEnabled
                               ? UnprotectSecret(options.encryptedFallbackKey)
                               : key);
        std::string data;
        if (!prepared.textOnly)
            data = EncodeForVision(image, options.quality, stop);
        CheckStop(stop);

        phaseChanged(TranslationPhase::Requesting);
        auto provider = createProvider(connection.provider);
        auto apiStart = Clock::now();
        DiagnosticSpan api("api");
        try {
            prepared.result = provider->TranslateLocated(data, history, connection, selectedKey.value, stop,
                                                         detected.regions);
            FlagUntranslatedPassages(prepared.result);
            api.End("ok", {{"request_count", prepared.result.requestCount},
                           {"segments", prepared.result.segments.size()}});
        } catch (const AppError& error) {
            api.Fail(error);
            throw;
        } catch (const Cancelled&) {
            api.End("cancelled");
            throw;
        }
        prepared.apiMs = ElapsedMs(apiStart);
        if (prepared.textOnly)
            prepared.result.protectedRegions = std::move(detectedAreas);
        data.clear();
        data.shrink_to_fit();
        CheckStop(stop);
        if (prepared.result.segments.empty()) {
            prepared.totalMs = ElapsedMs(start);
            translation.End("no_text");
            return prepared;
        }

        phaseChanged(TranslationPhase::Layout);
        DiagnosticSpan layout("layout");
        prepared.blocks = BuildOverlay(prepared.result, std::move(image), region, monitor, options);
        size_t suppressed = 0;
        for (const auto& block : prepared.blocks)
            for (const auto& item : block.positioned)
                suppressed += item.suppressed;
        layout.End("ok", {{"suppressed", suppressed}});
        CheckStop(stop);
        prepared.totalMs = ElapsedMs(start);
        translation.End();
        return prepared;
    } catch (const Cancelled&) {
        translation.End("cancelled");
        throw;
    } catch (const AppError& error) {
        translation.Fail(error);
        throw;
    } catch (...) {
        translation.End("error");
        throw;
    }
}
} // namespace sat
