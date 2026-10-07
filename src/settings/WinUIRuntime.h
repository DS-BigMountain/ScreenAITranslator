#pragma once
#include "common/Types.h"
namespace sat {
class WinUIRuntime {
 struct Impl;
 std::unique_ptr<Impl> impl_;
public:
 WinUIRuntime();
 ~WinUIRuntime();
 static bool ProcessMessage(const MSG& message);
};
}
