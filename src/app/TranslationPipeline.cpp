#include "TranslationPipeline.h"
#include "app/SourceMatching.h"
#include "capture/Capture.h"
#include "common/Diagnostics.h"
#include "common/Secret.h"
#include "common/Trace.h"
#include "providers/Catalog.h"
#include "providers/TranslationQuality.h"
#include "settings/Store.h"
#include <algorithm>

namespace sat {
namespace {
using Clock = std::chrono::steady_clock;
long long ElapsedMs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

// Records what local recognition saw, including the confidence that decides placement adoption.
void TraceDetection(const TextDetection& detected) noexcept {
    auto* trace = ActiveTrace();
    if (!trace)
        return;
    try {
        nlohmann::json regions = nlohmann::json::array();
        for (const auto& region : detected.regions) {
            nlohmann::json lines = nlohmann::json::array();
            for (const auto& line : region.lines)
                lines.push_back({{"x", line.left},
                                 {"y", line.top},
                                 {"width", Width(line)},
                                 {"height", Height(line)}});
            regions.push_back({{"id", region.id},
                               {"text", region.text},
                               {"confidence", region.confidence},
                               {"rect",
                                {{"x", region.rect.left},
                                 {"y", region.rect.top},
                                 {"width", Width(region.rect)},
                                 {"height", Height(region.rect)}}},
                               {"line_height", region.lineHeight},
                               {"lines", lines}});
        }
        trace->WriteJson("ocr.json", {{"status", detected.status},
                                      {"unrecognized_lines", detected.unrecognizedLines},
                                      {"regions", regions}});
    } catch (...) {
    }
}

// Legacy configurations may carry a display-oriented value; the local detector only accepts the
// three recognized language presets.
int LocalOcrLanguage(int mode) {
    return mode == OcrEnglish || mode == OcrJapanese || mode == OcrKorean ? mode : OcrLocalAuto;
}

TextDetection DetectSource(const Image& image, int mode, std::stop_token stop) {
    DiagnosticSpan ocr("ocr");
    auto detected = DetectOcrText(image, stop, LocalOcrLanguage(mode));
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
    TraceDetection(detected);
    return detected;
}

std::string EncodeForVision(const Image& image, int quality, std::stop_token stop) {
    DiagnosticSpan encoding("image_encode");
    auto jpeg = EncodeJpeg(image, quality, stop);
    auto data = Base64(jpeg);
    encoding.End("ok", {{"jpeg_bytes", jpeg.size()}});
    return data;
}

// Counts the passages whose recognition stayed below the confidence gate. The count feeds the
// status line only; placement relies on the geometry itself.
void CountLowConfidence(const TextDetection& detected, PreparedTranslation& prepared) {
    prepared.unrecognizedLines = detected.unrecognizedLines;
    for (const auto& region : detected.regions)
        if (!region.text.empty() && !(region.confidence >= .85f && region.confidence <= 1.f))
            ++prepared.lowConfidenceRegions;
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
                                               {"provider", options.provider},
                                               {"quality", options.quality},
                                               {"timeout_s", options.timeoutSeconds},
                                               {"translate_all", options.translateAll},
                                               {"context_count", history.size()}});
    TraceSession* trace = ActiveTrace();
    if (trace) {
        trace->WritePng("capture.png", image);
        const char* backend = image.backend == CaptureBackend::Dxgi        ? "dxgi"
                              : image.backend == CaptureBackend::GdiFallback ? "gdi"
                                                                             : "synthetic";
        nlohmann::json meta;
        meta["capture"] = {{"width", image.width}, {"height", image.height}, {"backend", backend}};
        meta["region"] = {
            {"x", region.left}, {"y", region.top}, {"width", Width(region)}, {"height", Height(region)}};
        meta["monitor"] = {
            {"width", Width(monitor.rect)}, {"height", Height(monitor.rect)}, {"dpi", monitor.dpi}};
        meta["prompt_length"] = options.prompt.size();
        meta["font"] = Utf8(options.font);
        meta["auto_font"] = options.autoFont;
        meta["font_size"] = options.fontSize;
        meta["match_text_color"] = options.matchTextColor;
        trace->Meta(std::move(meta));
    }
    // Every measured stage lands in the trace, which lets one directory pair the exact request with
    // the time each step took.
    auto summarize = [](const PreparedTranslation& prepared) {
        auto* active = ActiveTrace();
        if (!active)
            return;
        active->Meta({{"summary",
                       {{"ocr_ms", prepared.ocrMs},
                        {"api_ms", prepared.apiMs},
                        {"total_ms", prepared.totalMs},
                        {"segments", prepared.result.segments.size()},
                        {"request_count", prepared.result.requestCount}}}});
    };
    try {
        CheckStop(stop);
        PreparedTranslation prepared;
        // One screen translation entry: the provider capability decides whether the captured image
        // travels to the service, and the local detector always supplies the source geometry.
        prepared.textOnly = EffectiveTranslationMode(options) == TranslationOcrText;
        Settings routed = options;
        routed.translationMode = prepared.textOnly ? TranslationOcrText : TranslationAiVision;
        auto connection = SelectTranslationConnection(routed);
        DiagnosticEvent("route_selected",
                        {{"route", prepared.textOnly ? "text" : "vision"},
                         {"reason", "recognition_first"},
                         {"provider", connection.provider},
                         {"model", connection.model},
                         {"proxy_mode", connection.proxyMode}});

        TextDetection detected;
        std::vector<RECT> detectedAreas;
        // Recognition supplies the passages for both routes. A text service consumes the recognized
        // text directly; an image model receives the same passages through the source_regions
        // contract, so every recognized row comes back translated with the row geometry the detector
        // already measured. This is the reference ordering: recognize first, translate second, place
        // from the recognized rows. It replaces the earlier arrangement where the vision model had to
        // find the text on its own and recognition only refined the rectangles afterwards.
        auto ocrStart = Clock::now();
        phaseChanged(TranslationPhase::Recognizing);
        detected = DetectSource(image, routed.ocrMode, stop);
        prepared.ocrMs = ElapsedMs(ocrStart);
        CountLowConfidence(detected, prepared);
        for (const auto& source : detected.regions)
            detectedAreas.push_back(source.rect);
        std::erase_if(detected.regions, [](const auto& source) { return source.text.empty(); });
        std::vector<TextRegion> sourceRegions = detected.regions;
        // Nothing recognized at all is different from a doubtful recognition: the local pass found no
        // usable passage, so the capture itself goes to the model. A text service has no way to read
        // the capture, so it keeps the recognition error instead of failing on a capability it never had.
        if (sourceRegions.empty()) {
            if (IsTextService(options.provider))
                throw AppError("ocr", "本地 OCR 未获得可翻译文字，请检查原文语言设置");
            prepared.textOnly = false;
            routed.translationMode = TranslationAiVision;
            connection = SelectTranslationConnection(routed);
            DiagnosticEvent("route_fallback",
                            {{"reason", "local_ocr_empty"},
                             {"provider", connection.provider},
                             {"model", connection.model}});
        }
        Secret selectedKey(key);
        std::string data;
        if (!prepared.textOnly)
            // The upload bound is fixed: the resolution selector is gone, and a long edge of 1600 px keeps
            // a full-screen selection inside one request while small selections upload at their own size.
            data = EncodeForVision(image, 1, stop);
        CheckStop(stop);

        phaseChanged(TranslationPhase::Requesting);
        auto provider = createProvider(connection.provider);
        auto apiStart = Clock::now();
        DiagnosticSpan api("api");
        try {
            prepared.result =
                provider->TranslateLocated(data, history, connection, selectedKey.value, stop, sourceRegions);
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
            summarize(prepared);
            translation.End("no_text");
            return prepared;
        }
        // The local detector describes the same passages more precisely than the model once both
        // agree on the region and its text; every other segment keeps the model rectangle.
        if (!prepared.textOnly) {
            DiagnosticSpan matching("geometry_match");
            auto report = AttachSourceGeometry(prepared.result, image.width, image.height, detected.regions);
            prepared.result.uncoveredLines = std::move(report.uncoveredLines);
            matching.End("ok", {{"segments", report.segments},
                                {"adopted", report.adopted},
                                {"rows", report.matchedLines},
                                {"rejected_text", report.rejectedText},
                                {"rejected_space", report.rejectedSpace},
                                {"rejected_bounds", report.rejectedBounds}});
        }

        // Placement carries the source pixels that decided font size and color; recording it here
        // keeps the trace aligned with what the overlay actually received.
        TraceParsed(prepared.result);
        phaseChanged(TranslationPhase::Layout);
        DiagnosticSpan layout("layout");
        prepared.blocks = BuildOverlay(prepared.result, std::move(image), region, monitor, routed);
        size_t suppressed = 0;
        for (const auto& block : prepared.blocks)
            for (const auto& item : block.positioned)
                suppressed += item.suppressed;
        layout.End("ok", {{"suppressed", suppressed}});
        CheckStop(stop);
        prepared.totalMs = ElapsedMs(start);
        summarize(prepared);
        translation.End();
        return prepared;
    } catch (const Cancelled&) {
        translation.End("cancelled");
        throw;
    } catch (const AppError& error) {
        if (auto* active = ActiveTrace())
            active->Meta({{"error", {{"stage", error.stage}, {"message", error.what()}}}});
        translation.Fail(error);
        throw;
    } catch (...) {
        translation.End("error");
        throw;
    }
}
} // namespace sat
