#pragma once
#include "common/Types.h"
#include "capture/TextDetection.h"
#include "providers/Catalog.h"
#include "providers/VisionResponse.h"
#include <nlohmann/json.hpp>
namespace sat {
bool CanTranslateText(const std::vector<TextRegion>& regions);
bool HasRecognizedText(const std::vector<TextRegion>& regions);
Settings SelectTranslationConnection(const Settings& options);
TranslationResult ParseTranslation(const std::string& json, const std::vector<TextRegion>& regions = {},
                                   bool textOnly = false, VisionCoordinates coordinates = {});
nlohmann::json TranslationSchema(bool located = false, bool textOnly = false,
                                 VisionCoordinates coordinates = {});
nlohmann::json BuildRequest(const std::string& imageBase64, const std::vector<ContextItem>& context,
                            const Settings& options, bool strict, const std::vector<TextRegion>& regions = {},
                            const std::vector<int>& repairIds = {});
class ITranslationProvider {
public:
    virtual ~ITranslationProvider() = default;
    virtual TranslationResult Translate(const std::string& imageBase64,
                                        const std::vector<ContextItem>& context, const Settings& options,
                                        const std::string& apiKey, std::stop_token stop) = 0;
    virtual TranslationResult TranslateLocated(const std::string& image,
                                               const std::vector<ContextItem>& context,
                                               const Settings& options, const std::string& key,
                                               std::stop_token stop, const std::vector<TextRegion>&) {
        return Translate(image, context, options, key, stop);
    }
};
std::unique_ptr<ITranslationProvider> CreateProvider(const std::string& provider);
// Run on the worker: these perform network I/O and honor cancellation.
std::vector<std::string> FetchModels(const Settings& options, const std::string& apiKey,
                                     std::stop_token stop);
// Uploads only an internally generated test image; never captures the desktop.
TranslationResult TestVisionApi(const Settings& options, const std::string& apiKey, std::stop_token stop);
TranslationResult TestTextApi(const Settings& options, const std::string& apiKey, std::stop_token stop);
class TranslationContext {
    std::vector<ContextItem> items_;
    ULONGLONG expires_{};

public:
    void Clear();
    void Add(const TranslationResult& result, int limit);
    std::vector<ContextItem> Get(bool enabled, int limit);
    ULONGLONG ExpiresAt() const {
        return expires_;
    }
    void Restore(std::vector<ContextItem> items, ULONGLONG expires);
};
} // namespace sat
