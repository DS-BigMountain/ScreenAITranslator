#pragma once
#include "Types.h"
#include "capture/TextDetection.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace sat {
// Payload trace records the exact bytes exchanged with an AI service together with every measured
// stage in one directory per translation. Tracing is opt-in at startup; every sink stays inert
// while it is off, so the measurement path itself carries no file work.
void InitializeTrace(const std::filesystem::path& directory, bool enabled, size_t keep = 20) noexcept;
bool TraceEnabled() noexcept;
std::filesystem::path TraceRoot() noexcept;
// Removes the oldest recorded sessions and keeps the newest `keep` directories.
void PruneTraces() noexcept;

// One screen translation. Writing happens on a private thread, which keeps the recorded durations
// close to the measured ones. A nested session stays inert and shares the ambient parent.
class TraceSession {
public:
    struct Impl;

    explicit TraceSession(const char* kind, nlohmann::json fields = {}) noexcept;
    ~TraceSession();
    TraceSession(const TraceSession&) = delete;
    TraceSession& operator=(const TraceSession&) = delete;
    explicit operator bool() const noexcept;
    const std::filesystem::path& Directory() const noexcept;
    // Merges session metadata; fields can arrive while the translation runs.
    void Meta(nlohmann::json fields) noexcept;
    void Write(const char* name, std::string_view bytes) noexcept;
    void WriteText(const char* name, std::string_view text) noexcept;
    void WriteJson(const char* name, const nlohmann::json& value) noexcept;
    void WritePng(const char* name, const Image& image) noexcept;
    // Files belonging to one request attempt share a numbered directory.
    int BeginAttempt() noexcept;
    int CurrentAttempt() const noexcept;
    double OffsetMs(std::chrono::steady_clock::time_point moment) const noexcept;
    void Record(const nlohmann::json& stage) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

// Binds the ambient session of the calling thread. Worker threads take one so their stages join the
// same translation record.
class TraceContext {
    TraceSession* previous_;

public:
    explicit TraceContext(TraceSession* session) noexcept;
    ~TraceContext();
    TraceContext(const TraceContext&) = delete;
    TraceContext& operator=(const TraceContext&) = delete;
};
TraceSession* ActiveTrace() noexcept;

// Provider sinks. Every sink is a no-op outside an active session.
// `TraceRequest` records the OpenAI-compatible shape and splits prompt, schema, and regions.
void TraceRequest(const nlohmann::json& request, const std::vector<TextRegion>& regions,
                  const std::string& imageBase64, bool strict, bool textOnly) noexcept;
// `TraceRawRequest` records a protocol-specific body, which keeps native services inspectable even
// when their payload differs from the chat-completions shape.
void TraceRawRequest(const nlohmann::json& body, const std::string& imageBase64) noexcept;
void TraceResponse(int status, std::string_view body, std::string_view endpoint,
                   std::string_view method) noexcept;
// Header records keep the transport visible; the provider passes credentials already redacted.
void TraceRequestHeaders(std::string_view headers) noexcept;
void TraceResponseHeaders(std::string_view headers) noexcept;
void TraceParsed(const TranslationResult& result) noexcept;
// Diagnostics bridge: called when a measured span ends.
void TraceStage(const char* stage, unsigned long long parent, const nlohmann::json& fields,
                std::chrono::steady_clock::time_point started, const char* outcome) noexcept;
} // namespace sat
