#pragma once
#include "common/Types.h"
#include "capture/TextDetection.h"
#include <nlohmann/json.hpp>
namespace sat {
TranslationResult ParseTranslation(const std::string& json, const std::vector<TextRegion>& regions = {});
nlohmann::json TranslationSchema(bool located = false);
nlohmann::json BuildRequest(const std::string& imageBase64, const std::vector<ContextItem>& context, const Settings& options, bool strict, const std::vector<TextRegion>& regions = {});
class ITranslationProvider {
public:
 virtual ~ITranslationProvider()=default;
 virtual TranslationResult Translate(const std::string& imageBase64, const std::vector<ContextItem>& context, const Settings& options, const std::string& apiKey, std::stop_token stop)=0;
 virtual TranslationResult TranslateLocated(const std::string& image, const std::vector<ContextItem>& context, const Settings& options, const std::string& key, std::stop_token stop, const std::vector<TextRegion>&) { return Translate(image,context,options,key,stop); }
};
std::unique_ptr<ITranslationProvider> CreateProvider(const std::string& provider);
// Run on the worker: these perform network I/O and honor cancellation.
std::vector<std::string> FetchModels(const Settings& options, const std::string& apiKey, std::stop_token stop);
// Uploads only an internally generated test image; never captures the desktop.
TranslationResult TestVisionApi(const Settings& options, const std::string& apiKey, std::stop_token stop);
class TranslationContext {
 std::vector<ContextItem> items_;
 std::chrono::steady_clock::time_point last_{};
public:
 void Clear();
 void Add(const TranslationResult& result, int limit);
 std::vector<ContextItem> Get(bool enabled, int limit);
};
}
