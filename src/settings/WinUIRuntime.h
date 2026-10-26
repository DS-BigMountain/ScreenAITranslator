#pragma once
#include "common/Types.h"
namespace sat {
class WinUIRuntime {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    explicit WinUIRuntime(bool deferInitialization = false);
    ~WinUIRuntime();
    static void EnsureInitialized();
    static bool Initialized();
    static bool ProcessMessage(const MSG& message);
};
} // namespace sat
