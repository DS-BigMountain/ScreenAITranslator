#include "Diagnostics.h"
#include <atomic>
#include <algorithm>
#include <mutex>
#include <fstream>
#include <array>
#include <cstdio>
#include <string_view>

namespace sat {
namespace {
std::mutex logMutex;
std::filesystem::path logDirectory;
size_t fileLimit = 2 * 1024 * 1024;
std::atomic<unsigned long long> sequence{};
thread_local unsigned long long operationId{}, spanId{};
// Closed field schema prevents accidental payload/header/config dumps.
nlohmann::json SafeFields(const nlohmann::json& fields) {
    nlohmann::json safe = nlohmann::json::object();
    if (!fields.is_object())
        return safe;
    static constexpr std::string_view strings[]{"stage", "outcome",    "reason",      "provider", "model",
                                                "route", "ocr_status", "error_stage", "phase"};
    static constexpr std::string_view numbers[]{"span",
                                                "parent",
                                                "duration_ms",
                                                "width",
                                                "height",
                                                "ocr_mode",
                                                "quality",
                                                "regions",
                                                "empty_regions",
                                                "low_confidence",
                                                "segments",
                                                "suppressed",
                                                "request_count",
                                                "attempt",
                                                "remaining_ms",
                                                "timeout_s",
                                                "request_bytes",
                                                "response_bytes",
                                                "jpeg_bytes",
                                                "status",
                                                "winhttp_error",
                                                "context_count",
                                                "proxy_mode",
                                                "retained_segments",
                                                "boxes",
                                                "recognized_lines",
                                                "unrecognized_lines",
                                                "translation_mode",
                                                "clipped_boxes",
                                                "unpositioned_segments",
                                                "untranslated_segments",
                                                "ignored_segments",
                                                "json_byte"};
    static constexpr std::string_view booleans[]{"thinking", "translate_all", "fallback", "strict_schema",
                                                 "resumed"};
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        const auto& key = it.key();
        const auto& value = it.value();
        if (std::find(std::begin(strings), std::end(strings), key) != std::end(strings) && value.is_string())
            safe[key] = value.get<std::string>().substr(0, 160);
        else if (std::find(std::begin(numbers), std::end(numbers), key) != std::end(numbers) &&
                 value.is_number())
            safe[key] = value;
        else if (std::find(std::begin(booleans), std::end(booleans), key) != std::end(booleans) &&
                 value.is_boolean())
            safe[key] = value;
        else if ((key == "missing_ids" || key == "low_confidence_ids") && value.is_array()) {
            safe[key] = nlohmann::json::array();
            for (const auto& id : value)
                if (id.is_number_integer() && id >= 1 && id <= 256 && safe[key].size() < 256)
                    safe[key].push_back(id);
        }
    }
    return safe;
}
const char* ErrorReason(const AppError& error) noexcept {
    std::string_view text(error.what());
    if (text.find("超时") != text.npos || text.find("时限") != text.npos ||
        text.find("time limit") != text.npos)
        return "timeout";
    if (text.find("未完整结束") != text.npos)
        return "incomplete_response";
    if (text.find("缺失") != text.npos)
        return "missing_regions";
    if (error.stage == "Parsing")
        return "invalid_response";
    if (error.stage == "Authentication")
        return "authentication_failed";
    if (error.stage == "Requesting")
        return "request_failed";
    return "stage_failed";
}
} // namespace
void InitializeDiagnostics(const std::filesystem::path& directory, size_t maxBytes) noexcept {
    try {
        std::lock_guard lock(logMutex);
        logDirectory.clear();
        std::filesystem::create_directories(directory);
        logDirectory = std::filesystem::absolute(directory);
        fileLimit = std::clamp<size_t>(maxBytes, 4096, 2 * 1024 * 1024);
    } catch (...) {
    }
}
void DiagnosticEvent(const char* event, const nlohmann::json& fields) noexcept {
    try {
        std::lock_guard lock(logMutex);
        if (logDirectory.empty())
            return;
        auto row = SafeFields(fields);
        SYSTEMTIME now{};
        GetSystemTime(&now);
        char timestamp[40]{};
        sprintf_s(timestamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        row["time_utc"] = timestamp;
        row["event"] = event;
        row["version"] = SAT_VERSION;
        row["pid"] = GetCurrentProcessId();
        row["thread"] = GetCurrentThreadId();
        row["operation"] = operationId;
        if (!row.contains("span"))
            row["span"] = spanId;
        auto line = row.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + '\n';
        // Coordinate the brief overlap during idle process replacement. Never block
        // a translation indefinitely if another process owns the writer.
        HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\ScreenAITranslator.Diagnostics");
        if (!mutex)
            return;
        auto wait = WaitForSingleObject(mutex, 50);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(mutex);
            return;
        }
        try {
            auto current = logDirectory / L"diagnostic.jsonl";
            if (std::filesystem::exists(current) &&
                std::filesystem::file_size(current) + line.size() > fileLimit) {
                std::filesystem::remove(logDirectory / L"diagnostic.3.jsonl");
                for (int i = 2; i >= 0; --i) {
                    auto from =
                        i ? logDirectory / (L"diagnostic." + std::to_wstring(i) + L".jsonl") : current;
                    auto to = logDirectory / (L"diagnostic." + std::to_wstring(i + 1) + L".jsonl");
                    if (std::filesystem::exists(from))
                        std::filesystem::rename(from, to);
                }
            }
            std::ofstream output(current, std::ios::binary | std::ios::app);
            output << line;
        } catch (...) {
        }
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    } catch (...) {
    }
}
unsigned long long NewDiagnosticId() noexcept {
    return ++sequence;
}
unsigned long long DiagnosticOperation() noexcept {
    return operationId;
}
DiagnosticContext::DiagnosticContext(unsigned long long operation) noexcept : previous_(operationId) {
    operationId = operation;
}
DiagnosticContext::~DiagnosticContext() {
    operationId = previous_;
}
DiagnosticSpan::DiagnosticSpan(const char* stage, const nlohmann::json& fields) noexcept
    : stage_(stage), id_(NewDiagnosticId()), parent_(spanId), start_(std::chrono::steady_clock::now()) {
    spanId = id_;
    try {
        auto data = fields.is_object() ? fields : nlohmann::json::object();
        data["stage"] = stage_;
        data["parent"] = parent_;
        DiagnosticEvent("stage_begin", data);
    } catch (...) {
    }
}
DiagnosticSpan::~DiagnosticSpan() {
    if (!done_)
        End("interrupted");
}
void DiagnosticSpan::End(const char* outcome, const nlohmann::json& fields) noexcept {
    if (done_)
        return;
    done_ = true;
    try {
        auto data = fields.is_object() ? fields : nlohmann::json::object();
        data["stage"] = stage_;
        data["span"] = id_;
        data["parent"] = parent_;
        data["outcome"] = outcome;
        data["duration_ms"] =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
        DiagnosticEvent("stage_end", data);
    } catch (...) {
    }
    if (spanId == id_)
        spanId = parent_;
}
void DiagnosticSpan::Fail(const AppError& error) noexcept {
    try {
        End("error",
            {{"error_stage", error.stage}, {"status", error.status}, {"reason", ErrorReason(error)}});
    } catch (...) {
        End("error");
    }
}
} // namespace sat
