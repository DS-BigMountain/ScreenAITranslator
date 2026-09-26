#include "Store.h"
#include <nlohmann/json.hpp>
#include <shlobj.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <regex>

namespace sat {
namespace {
using Json = nlohmann::json;
std::mutex storeMutex;
std::atomic<unsigned long> temporaryId{};
constexpr size_t ConfigLimit = 1024 * 1024;
[[noreturn]] void Failure(const char* message) { throw AppError("storage", message, static_cast<int>(GetLastError())); }
void Require(bool condition, const char* message) { if (!condition) throw AppError("settings", message); }
std::string Read(const std::filesystem::path& file, size_t limit) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) { if (ec) Failure("Cannot inspect local data file"); return {}; }
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size > limit) throw AppError("storage", "Local data file is inaccessible or too large; original file retained");
    std::ifstream input(file, std::ios::binary);
    if (!input) Failure("Cannot open local data file");
    std::string data(static_cast<size_t>(size), '\0');
    if (!data.empty() && !input.read(data.data(), static_cast<std::streamsize>(data.size()))) Failure("Cannot read local data file");
    return data;
}
Json ReadJson(const std::filesystem::path& file) {
    if (!std::filesystem::exists(file)) return Json::object();
    try {
        auto json = Json::parse(Read(file, ConfigLimit));
        if (!json.is_object() || json.value("version", 1) != 1) throw std::runtime_error("schema");
        return json;
    } catch (const AppError&) { throw; }
    catch (...) { throw AppError("storage", "Invalid local JSON or unsupported version; original file and backup retained"); }
}
void AtomicWrite(const std::filesystem::path& file, const std::string& data, bool backup) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) throw AppError("storage", "Cannot create local data directory");
    auto temp = file;
    temp += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(++temporaryId);
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) Failure("Cannot create temporary local data file");
    DWORD written = 0;
    const bool okay = data.size() <= MAXDWORD && WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size() && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!okay) { DeleteFileW(temp.c_str()); Failure("Cannot flush local data; original file retained"); }
    BOOL replaced;
    if (std::filesystem::exists(file)) {
        auto bak = file; bak += L".bak";
        replaced = ReplaceFileW(file.c_str(), temp.c_str(), backup ? bak.c_str() : nullptr, 0, nullptr, nullptr);
    } else replaced = MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_WRITE_THROUGH);
    if (!replaced) { DeleteFileW(temp.c_str()); Failure("Cannot commit local data; original file retained"); }
}
bool Plain(const std::string& value, size_t limit) {
    return !value.empty() && value.size() <= limit && std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
void ValidateState(const PersistentState& state) {
    Require(state.tab >= 0 && state.tab <= 4, "Invalid settings tab");
    if (state.window) {
        const auto& r = *state.window;
        Require(r.right > r.left && r.bottom > r.top && static_cast<long long>(r.right) - r.left <= 32768 && static_cast<long long>(r.bottom) - r.top <= 32768, "Invalid window position");
    }

}
}

Store::Store(std::filesystem::path root) : root_(std::move(root)) {
    if (root_.empty()) {
        PWSTR path = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path))) Failure("Cannot resolve LocalAppData");
        root_ = std::filesystem::path(path) / L"ScreenAITranslator";
        CoTaskMemFree(path);
    }
}

std::string ProtectSecret(const std::string& secret) {
    if (secret.empty()) return {};
    Require(secret.size() <= 16384 && secret.find_first_of("\r\n") == std::string::npos && secret.find('\0') == std::string::npos, "Invalid API key");
    DATA_BLOB input{static_cast<DWORD>(secret.size()), reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))}, output{};
    if (!CryptProtectData(&input, L"ScreenAITranslator API key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) Failure("Cannot protect API key for current Windows user");
    DWORD length = 0;
    bool okay = CryptBinaryToStringA(output.pbData, output.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length) != FALSE;
    std::string encoded(length, '\0');
    if (okay) okay = CryptBinaryToStringA(output.pbData, output.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded.data(), &length) != FALSE;
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData);
    if (!okay) Failure("Cannot encode protected API key");
    encoded.resize(length);
    while (!encoded.empty() && encoded.back() == '\0') encoded.pop_back();
    return encoded;
}

std::string UnprotectSecret(const std::string& encrypted) {
    if (encrypted.empty()) return {};
    Require(encrypted.size() <= 32768, "Invalid encrypted API key");
    DWORD length = 0;
    if (!CryptStringToBinaryA(encrypted.data(), static_cast<DWORD>(encrypted.size()), CRYPT_STRING_BASE64, nullptr, &length, nullptr, nullptr)) Failure("Invalid encrypted API key");
    std::vector<BYTE> bytes(length);
    if (!CryptStringToBinaryA(encrypted.data(), static_cast<DWORD>(encrypted.size()), CRYPT_STRING_BASE64, bytes.data(), &length, nullptr, nullptr)) Failure("Invalid encrypted API key");
    DATA_BLOB input{length, bytes.data()}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) Failure("API key cannot be decrypted by this Windows user; enter it again");
    std::string secret(reinterpret_cast<char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData);
    return secret;
}

void ValidateSettings(const Settings& s) {
    Require(s.provider == "deepseek" || s.provider == "openai-compatible", "Unsupported provider");
    Require(Plain(s.model, 256), "Model must be nonempty and contain no control characters");
    Require(s.baseUrl.size() <= 2048 && s.baseUrl.find_first_of(L"\r\n\t ?#\\") == std::wstring::npos && s.baseUrl.find(L'\0') == std::wstring::npos, "Invalid API Base URL");
    URL_COMPONENTS url{}; url.dwStructSize = sizeof(url); url.dwHostNameLength = url.dwUserNameLength = url.dwPasswordLength = url.dwUrlPathLength = url.dwExtraInfoLength = static_cast<DWORD>(-1);
    Require(WinHttpCrackUrl(s.baseUrl.c_str(), static_cast<DWORD>(s.baseUrl.size()), 0, &url) != FALSE && url.dwHostNameLength > 0 && !url.dwUserNameLength && !url.dwPasswordLength && !url.dwExtraInfoLength, "API Base URL must contain a host and no credentials, query, or fragment");
    std::wstring host(url.lpszHostName, url.dwHostNameLength);
    std::transform(host.begin(), host.end(), host.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    Require(url.nScheme == INTERNET_SCHEME_HTTPS || (url.nScheme == INTERNET_SCHEME_HTTP && (host == L"localhost" || host == L"127.0.0.1" || host == L"::1" || host == L"[::1]")), "HTTPS is required except for loopback test servers");
    Require(s.timeoutSeconds >= 1 && s.timeoutSeconds <= 300, "Timeout must be 1 to 300 seconds");
    Require(s.autoHideSeconds >= 1 && s.autoHideSeconds <= 3600, "Auto-hide time must be 1 to 3600 seconds");
    Require(s.quality >= 0 && s.quality <= 2 && s.contextSize >= 5 && s.contextSize <= 10, "Invalid quality or context count (5 to 10)");
    Require(s.prompt.size() <= 32768 && s.prompt.find('\0') == std::string::npos, "Prompt exceeds limit or contains invalid characters");
    Require(!s.font.empty() && s.font.size() <= 128, "Invalid font name");
    Require(std::isfinite(s.fontSize) && s.fontSize >= 8 && s.fontSize <= 144 && std::isfinite(s.outlineWidth) && s.outlineWidth >= 0 && s.outlineWidth <= 10 && std::isfinite(s.backgroundOpacity) && s.backgroundOpacity >= 0.1f && s.backgroundOpacity <= 1, "Invalid appearance settings");
    for (int i = 0; i < 4; ++i) {
        const auto& h = s.hotkeys[i];
        Require(h.key <= 255 && !(h.modifiers & ~(MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN)) && (h.key || !h.modifiers), "Invalid hotkey");
        for (int j = 0; j < i; ++j) Require(!h.key || h.key != s.hotkeys[j].key || h.modifiers != s.hotkeys[j].modifiers, "Hotkeys must be unique");
    }
    if (!s.encryptedKey.empty()) { auto secret = UnprotectSecret(s.encryptedKey); SecureZeroMemory(secret.data(), secret.size()); }
}

Settings Store::LoadSettings() {
    std::lock_guard lock(storeMutex);
    const auto j = ReadJson(root_ / L"config.json");
    Settings s;
    try {
        for (auto field : {"startup", "tray", "autoHide", "contextEnabled", "autoFont", "spatialOverlay", "thinkingHigh"})
            Require(!j.contains(field) || j.at(field).is_boolean(), "Invalid boolean setting");
        for (auto field : {"autoHideSeconds", "timeoutSeconds", "quality", "contextSize", "textColor", "outlineColor"})
            Require(!j.contains(field) || (j.at(field).is_number_integer() && j.at(field).get<long long>() >= 0 && j.at(field).get<long long>() <= INT_MAX), "Invalid integer setting");
#define READ_FIELD(field) s.field = j.value(#field, s.field)
        READ_FIELD(startup); READ_FIELD(tray); READ_FIELD(autoHide); READ_FIELD(autoHideSeconds);
        READ_FIELD(provider); READ_FIELD(model); READ_FIELD(timeoutSeconds); READ_FIELD(thinkingHigh); READ_FIELD(encryptedKey); READ_FIELD(quality);
        READ_FIELD(contextEnabled); READ_FIELD(contextSize); READ_FIELD(prompt); READ_FIELD(autoFont); READ_FIELD(spatialOverlay); READ_FIELD(fontSize);
        READ_FIELD(textColor); READ_FIELD(outlineColor); READ_FIELD(outlineWidth); READ_FIELD(backgroundOpacity);
#undef READ_FIELD
        s.baseUrl = Wide(j.value("baseUrl", Utf8(s.baseUrl))); s.font = Wide(j.value("font", Utf8(s.font)));
        if (j.contains("hotkeys")) {
            const auto& keys = j.at("hotkeys");
            Require(keys.is_array() && keys.size() == 4, "Invalid saved hotkeys");
            for (int i = 0; i < 4; ++i) {
                for (auto field : {"modifiers", "key"}) Require(keys.at(i).at(field).is_number_integer() && keys.at(i).at(field).get<long long>() >= 0 && keys.at(i).at(field).get<long long>() <= 255, "Invalid saved hotkey value");
                s.hotkeys[i] = { keys.at(i).at("modifiers").get<UINT>(), keys.at(i).at("key").get<UINT>() };
            }
        }
        ValidateSettings(s);
    } catch (const AppError&) { throw; }
    catch (...) { throw AppError("storage", "Invalid settings values; original file retained"); }
    return s;
}

void Store::SaveSettings(const Settings& s) {
    ValidateSettings(s);
    Json j{{"version", 1}};
#define WRITE_FIELD(field) j[#field] = s.field
    WRITE_FIELD(startup); WRITE_FIELD(tray); WRITE_FIELD(autoHide); WRITE_FIELD(autoHideSeconds);
    WRITE_FIELD(provider); WRITE_FIELD(model); WRITE_FIELD(timeoutSeconds); WRITE_FIELD(thinkingHigh); WRITE_FIELD(encryptedKey); WRITE_FIELD(quality);
    WRITE_FIELD(contextEnabled); WRITE_FIELD(contextSize); WRITE_FIELD(prompt); WRITE_FIELD(autoFont); WRITE_FIELD(spatialOverlay); WRITE_FIELD(fontSize);
    WRITE_FIELD(textColor); WRITE_FIELD(outlineColor); WRITE_FIELD(outlineWidth); WRITE_FIELD(backgroundOpacity);
#undef WRITE_FIELD
    j["baseUrl"] = Utf8(s.baseUrl); j["font"] = Utf8(s.font); j["hotkeys"] = Json::array();
    for (const auto& h : s.hotkeys) j["hotkeys"].push_back({{"modifiers", h.modifiers}, {"key", h.key}});
    std::lock_guard lock(storeMutex);
    (void)ReadJson(root_ / L"config.json"); // Never overwrite malformed input with defaults.
    AtomicWrite(root_ / L"config.json", j.dump(2), true);
}

PersistentState Store::LoadState() {
    std::lock_guard lock(storeMutex);
    const auto j = ReadJson(root_ / L"state.json"); PersistentState state;
    try {
        state.tab = j.value("tab", 0);
        if (state.tab == 5) state.tab = 4; // Migrate the removed history/log page to Appearance.
        if (j.contains("window") && !j["window"].is_null()) {
            const auto& w = j.at("window");
            state.window = RECT{w.at("left").get<LONG>(), w.at("top").get<LONG>(), w.at("right").get<LONG>(), w.at("bottom").get<LONG>()};
        }
        // Legacy fixed selections are deliberately ignored: regions are session-only.
        ValidateState(state);
    } catch (const AppError&) { throw; }
    catch (...) { throw AppError("storage", "Invalid saved state; original file retained"); }
    return state;
}
void Store::SaveState(const PersistentState& state) {
    ValidateState(state);
    Json j{{"version", 1}, {"tab", state.tab}, {"window", nullptr}};
    if (state.window) { auto r = *state.window; j["window"] = {{"left", r.left}, {"top", r.top}, {"right", r.right}, {"bottom", r.bottom}}; }
    std::lock_guard lock(storeMutex);
    (void)ReadJson(root_ / L"state.json");
    AtomicWrite(root_ / L"state.json", j.dump(2), true);
}

std::string Sanitize(const std::string& text, const std::string& key) {
    if (text.size() > 16384) return "[Oversized diagnostic text omitted]";
    std::string out = text;
    if (!key.empty()) { size_t at = 0; while ((at = out.find(key, at)) != std::string::npos) { out.replace(at, key.size(), "[REDACTED]"); at += 10; } }
    static const std::regex authorization(R"(authorization[\s"']*[:=][^\r\n]*)", std::regex::icase);
    static const std::regex bearer(R"(bearer\s+[^\s,;"}]+)", std::regex::icase);
    static const std::regex secret(R"(sk-[A-Za-z0-9_\-]+)", std::regex::icase);
    static const std::regex image(R"(data:image/[^;\s]+;base64,[A-Za-z0-9+/=\r\n]+)", std::regex::icase);
    static const std::regex base64(R"([A-Za-z0-9+/]{80,}={0,2})");
    out = std::regex_replace(out, authorization, "Authorization: [REDACTED]");
    out = std::regex_replace(out, bearer, "Bearer [REDACTED]");
    out = std::regex_replace(out, secret, "[REDACTED]");
    out = std::regex_replace(out, image, "[IMAGE REDACTED]");
    out = std::regex_replace(out, base64, "[BASE64 REDACTED]");
    if (out.size() > 16384) out = "[Oversized diagnostic text omitted]";
    return out;
}

void SetStartup(bool enabled, const std::wstring& executable) {
    Require(!enabled || (!executable.empty() && std::filesystem::path(executable).is_absolute() && executable.find_first_of(L"\"\r\n") == std::wstring::npos), "Startup executable must be an absolute path");
    HKEY registry = nullptr;
    const auto opened = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &registry, nullptr);
    if (opened != ERROR_SUCCESS) throw AppError("startup", "Cannot open current-user startup settings", opened);
    LSTATUS status;
    if (enabled) {
        const auto command = L"\"" + executable + L"\" --background";
        status = RegSetValueExW(registry, L"ScreenAITranslator", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else status = RegDeleteValueW(registry, L"ScreenAITranslator");
    RegCloseKey(registry);
    if (status != ERROR_SUCCESS && !(status == ERROR_FILE_NOT_FOUND && !enabled)) throw AppError("startup", "Cannot update current-user startup settings", status);
}
}


