#pragma once
#include "common/Types.h"
namespace sat {
struct IdleResume {
 std::string payload;
 HANDLE instance{};
};
// Anonymous, explicitly inherited handles keep session text off disk and the command line.
bool StartIdleReplacement(const std::wstring& executable,HANDLE instance,const std::string& payload,const std::wstring& arguments=L"");
// A resumed child waits until the old process has fully exited before registering hotkeys.
IdleResume ReceiveIdleReplacement(const wchar_t* command);
}
