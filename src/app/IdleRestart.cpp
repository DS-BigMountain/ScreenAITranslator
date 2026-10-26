#include "IdleRestart.h"
#include <shellapi.h>
#include <array>
#include <cstring>
namespace sat {
namespace {
constexpr DWORD Capacity = 2 * 1024 * 1024, Magic = 0x53415449;
struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value)
            CloseHandle(value);
    }
};
struct Header {
    DWORD magic, length;
};
HANDLE Parse(const wchar_t* text) {
    wchar_t* end{};
    auto value = _wcstoui64(text, &end, 10);
    if (!value || !end || *end)
        throw AppError("idle", "Invalid resume handle");
    auto handle = reinterpret_cast<HANDLE>(value);
    DWORD flags{};
    if (!GetHandleInformation(handle, &flags) || (flags & HANDLE_FLAG_INHERIT) == 0)
        throw AppError("idle", "Resume handle was not inherited");
    return handle;
}
} // namespace
bool StartIdleReplacement(const std::wstring& executable, HANDLE instance, const std::string& payload,
                          const std::wstring& arguments) {
    if (payload.empty() || payload.size() > Capacity - sizeof(Header))
        return false;
    Handle mapping{CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, Capacity, nullptr)};
    if (!mapping.value)
        return false;
    auto view = MapViewOfFile(mapping.value, FILE_MAP_WRITE, 0, 0, Capacity);
    if (!view)
        return false;
    auto header = static_cast<Header*>(view);
    *header = {Magic, static_cast<DWORD>(payload.size())};
    std::memcpy(header + 1, payload.data(), payload.size());
    UnmapViewOfFile(view);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle read, owner, parent, ready{CreateEventW(&sa, TRUE, FALSE, nullptr)},
        commit{CreateEventW(&sa, TRUE, FALSE, nullptr)};
    auto process = GetCurrentProcess();
    if (!ready.value || !commit.value ||
        !DuplicateHandle(process, mapping.value, process, &read.value, FILE_MAP_READ, TRUE, 0) ||
        !DuplicateHandle(process, instance, process, &owner.value, 0, TRUE, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(process, process, process, &parent.value, SYNCHRONIZE, TRUE, 0))
        return false;
    std::array<HANDLE, 5> handles{read.value, parent.value, ready.value, commit.value, owner.value};
    SIZE_T bytes{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes))
        return false;
    struct Cleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~Cleanup() {
            DeleteProcThreadAttributeList(list);
        }
    } cleanup{list};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                   sizeof(handles), nullptr, nullptr))
        return false;
    std::wstring command = L"\"" + executable + L"\" " + arguments + L" --background --idle-resume";
    for (auto handle : handles)
        command += L" " + std::to_wstring(reinterpret_cast<ULONG_PTR>(handle));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList = list;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup.StartupInfo, &child))
        return false;
    Handle childProcess{child.hProcess}, childThread{child.hThread};
    HANDLE waiting[]{ready.value, childProcess.value};
    if (WaitForMultipleObjects(2, waiting, FALSE, 3000) != WAIT_OBJECT_0)
        return false;
    // A shortcut or a second launch arriving during startup takes precedence.
    // Leave the current message loop alive to service it instead of dropping it.
    if (GetQueueStatus(QS_HOTKEY | QS_SENDMESSAGE) & 0xffff0000)
        return false;
    return SetEvent(commit.value) != FALSE;
}
IdleResume ReceiveIdleReplacement(const wchar_t* command) {
    int count{};
    auto args = CommandLineToArgvW((std::wstring(L"app ") + command).c_str(), &count);
    if (!args)
        throw AppError("idle", "Cannot parse resume command");
    struct Free {
        LPWSTR* args;
        ~Free() {
            LocalFree(args);
        }
    } free{args};
    int index = 1;
    while (index < count && std::wstring_view(args[index]) != L"--idle-resume")
        ++index;
    if (index == count)
        return {};
    if (index + 6 != count)
        throw AppError("idle", "Invalid resume command");
    Handle mapping{Parse(args[index + 1])}, parent{Parse(args[index + 2])}, ready{Parse(args[index + 3])},
        commit{Parse(args[index + 4])}, owner{Parse(args[index + 5])};
    auto view = MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, Capacity);
    if (!view)
        throw AppError("idle", "Cannot read resume state");
    struct Unmap {
        void* view;
        ~Unmap() {
            UnmapViewOfFile(view);
        }
    } unmap{view};
    auto header = static_cast<const Header*>(view);
    if (header->magic != Magic || !header->length || header->length > Capacity - sizeof(Header))
        throw AppError("idle", "Invalid resume state");
    std::string payload(reinterpret_cast<const char*>(header + 1), header->length);
    if (!SetEvent(ready.value) || WaitForSingleObject(commit.value, 5000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(parent.value, 15000) != WAIT_OBJECT_0)
        throw AppError("idle", "Idle handoff was not committed");
    SetHandleInformation(owner.value, HANDLE_FLAG_INHERIT, 0);
    auto instance = owner.value;
    owner.value = nullptr;
    return {std::move(payload), instance};
}
} // namespace sat
