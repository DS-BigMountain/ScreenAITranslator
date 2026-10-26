#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <stop_token>
#include <stdexcept>
#include <memory>
#include <functional>
#include <filesystem>
#include <map>
#include "OcrLanguage.h"

namespace sat {
struct Box {
    int x{}, y{}, width{}, height{};
};
struct Segment {
    std::string original, translated;
    Box box;
    // Local OCR geometry is expressed in original capture pixels, independently of upload scaling.
    std::optional<RECT> sourcePixels;
    std::vector<RECT> sourceLines;
    float sourceLineHeight{};
    int sourceId{};
    bool positionUnavailable{}, untranslated{};
};
struct TranslationResult {
    std::string language;
    std::vector<Segment> segments;
    std::vector<RECT> protectedRegions;
    int requestCount{};
    // Includes geometry regions explicitly confirmed as non-text, even without a segment.
    std::vector<int> resolvedSourceIds;
    int clippedBoxes{};
    int unpositionedSegments{}, untranslatedSegments{}, ignoredSegments{};
};
struct ContextItem {
    std::string original, translated;
};
enum class CaptureBackend { Synthetic, Dxgi, GdiFallback };
struct Image {
    int width{}, height{};
    std::vector<unsigned char> bgra;
    CaptureBackend backend{CaptureBackend::Synthetic};
};
struct Monitor {
    std::wstring id;
    RECT rect{};
    UINT dpi{96};
};
struct FixedRegion {
    std::wstring monitorId;
    double x{}, y{}, width{}, height{};
    int monitorWidth{}, monitorHeight{};
};
struct Hotkey {
    UINT modifiers{}, key{};
};
struct ConnectionProfile {
    std::wstring baseUrl;
    std::string model, encryptedKey, serviceRegion;
    std::string requestParameters{"{}"};
};
struct Settings {
    bool startup{true}, tray{true}, autoHide{false};
    int autoHideSeconds{10};
    Hotkey hotkeys[4]{};
    std::string provider{"deepseek"};
    std::wstring baseUrl{L"https://api.deepseek.com"};
    std::string model{"deepseek-flash"};
    int timeoutSeconds{20};
    std::string requestParameters{"{}"};
    std::string encryptedKey;
    std::string serviceRegion;
    int proxyMode{0}; // 0 Windows system proxy, 1 direct, 2 explicit HTTP CONNECT proxy
    std::wstring proxyAddress;
    bool fallbackEnabled{false};
    std::string fallbackProvider{"openai-compatible"}, fallbackModel, encryptedFallbackKey;
    std::wstring fallbackBaseUrl{L"https://api.openai.com/v1"};
    std::string fallbackRequestParameters{"{}"};
    std::map<std::string, ConnectionProfile> connectionProfiles, fallbackProfiles;
    bool translateAll{true};
    int quality{1};
    int translationMode{TranslationAiVision};
    int ocrMode{OcrLocalAuto};
    bool contextEnabled{true};
    int contextSize{8};
    std::string prompt{
        "请准确翻译画面中的所有可见文字为简体中文。优先忠实原意，不进行文学化润色，不添加原文不存在的信息。角"
        "色名、专有名词应尽量保持一致。默认翻译所有字幕、对话、按钮和 UI 文本。"};
    std::wstring font{L"Microsoft YaHei UI"};
    bool matchTextColor{true};
    bool autoFont{true};
    float fontSize{22};
    bool spatialOverlay{true};
    COLORREF textColor{RGB(255, 255, 255)}, outlineColor{RGB(0, 0, 0)};
    float outlineWidth{1.0f}, backgroundOpacity{0.88f};
};
struct PersistentState {
    std::optional<RECT> window;
    int tab{};
};
struct AppError : std::runtime_error {
    std::string stage;
    int status{};
    AppError(std::string stage_, std::string safeMessage, int status_ = 0)
        : std::runtime_error(std::move(safeMessage)), stage(std::move(stage_)), status(status_) {}
};
struct Cancelled : std::exception {
    const char* what() const noexcept override {
        return "cancelled";
    }
};
inline void CheckStop(std::stop_token stop) {
    if (stop.stop_requested())
        throw Cancelled{};
}
inline int Width(RECT r) {
    return r.right - r.left;
}
inline int Height(RECT r) {
    return r.bottom - r.top;
}
inline bool Valid(RECT r) {
    return Width(r) > 0 && Height(r) > 0;
}
std::wstring Wide(const std::string& value);
std::string Utf8(const std::wstring& value);
void CheckHR(HRESULT hr, const char* stage);
std::string Timestamp();
} // namespace sat
