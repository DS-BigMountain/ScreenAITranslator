#pragma once
#include "Types.h"
#include <nlohmann/json.hpp>

namespace sat {
// Opt-in at application startup; tests use their own directory. No payloads or
// arbitrary exception messages enter the diagnostic schema.
void InitializeDiagnostics(const std::filesystem::path& directory,
                           size_t maxBytes = 2 * 1024 * 1024) noexcept;
void DiagnosticEvent(const char* event, const nlohmann::json& fields = {}) noexcept;
unsigned long long NewDiagnosticId() noexcept;
unsigned long long DiagnosticOperation() noexcept;
class DiagnosticContext {
    unsigned long long previous_;

public:
    explicit DiagnosticContext(unsigned long long operation) noexcept;
    ~DiagnosticContext();
    DiagnosticContext(const DiagnosticContext&) = delete;
};
class DiagnosticSpan {
    const char* stage_;
    unsigned long long id_, parent_;
    std::chrono::steady_clock::time_point start_;
    bool done_{};

public:
    explicit DiagnosticSpan(const char* stage, const nlohmann::json& fields = {}) noexcept;
    ~DiagnosticSpan();
    void End(const char* outcome = "ok", const nlohmann::json& fields = {}) noexcept;
    void Fail(const AppError& error) noexcept;
    DiagnosticSpan(const DiagnosticSpan&) = delete;
};
} // namespace sat
