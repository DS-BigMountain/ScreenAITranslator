#include "settings/Store.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <limits>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void Throws(F&& f, const char* message) {
    bool threw = false;
    try { f(); } catch (const std::exception&) { threw = true; }
    Check(threw, message);
}
std::string File(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}
struct TestDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / (L"ScreenAITranslator-store-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    ~TestDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
}

void RunStoreTests() {
    using namespace sat;
    TestDirectory temp;
    Store store(temp.path);
    auto settings = store.LoadSettings();
    Check(settings.ocrMode == 0, "Default OCR uses bundled English/Japanese model");
    Check(!settings.thinkingHigh && settings.contextEnabled && settings.contextSize == 8, "Persistence and context defaults");
    Check(settings.hotkeys[0].key == 0 && settings.hotkeys[1].key == 0, "Hotkeys must start unassigned");
    Check(!std::filesystem::exists(temp.path), "Read missing config must not create data");
    const std::string secret = "test-account-secret-1234";
    settings.encryptedKey = ProtectSecret(secret);
    Check(settings.encryptedKey != secret && UnprotectSecret(settings.encryptedKey) == secret, "DPAPI round trip");
    Throws([] { UnprotectSecret("plaintext-key"); }, "Plaintext must not be accepted as protected secret");
    Check(settings.spatialOverlay,"Spatial overlay is the new default"); settings.spatialOverlay=false;settings.thinkingHigh=true;
    settings.ocrMode = 2;settings.matchTextColor=false;
    settings.prompt = "只翻译字幕。保留专有名词。";
    settings.font = L"微软雅黑";
    settings.hotkeys[0] = {MOD_CONTROL | MOD_ALT, 'T'};
    store.SaveSettings(settings);
    auto loaded = store.LoadSettings();
    Check(loaded.prompt == settings.prompt && loaded.font == settings.font && loaded.hotkeys[0].key == 'T', "UTF-8 settings round trip");
    Check(!loaded.matchTextColor,"Color preference persisted");
    Check(loaded.ocrMode == 2, "OCR mode persisted");
    Check(!loaded.spatialOverlay && loaded.thinkingHigh,"Overlay and thinking preferences persisted");
    Check(UnprotectSecret(loaded.encryptedKey) == secret, "Protected key persisted");
    Check(File(temp.path / "config.json").find(secret) == std::string::npos, "Config must not contain plaintext key");
    settings.model = "custom-vision"; store.SaveSettings(settings);
    Check(std::filesystem::exists(temp.path / "config.json.bak"), "Atomic save retains backup");
    auto bad = settings; bad.ocrMode = 4;
    Throws([&] { store.SaveSettings(bad); }, "Invalid OCR mode must fail");
    bad = settings; bad.contextSize = 100;
    Throws([&] { store.SaveSettings(bad); }, "Invalid context count must fail");
    Check(store.LoadSettings().model == "custom-vision", "Validation failure preserves config");
    bad = settings; bad.hotkeys[1] = bad.hotkeys[0];
    Throws([&] { ValidateSettings(bad); }, "Duplicate hotkeys must fail");
    bad = settings; bad.outlineWidth = std::numeric_limits<float>::quiet_NaN();
    Throws([&] { ValidateSettings(bad); }, "NaN appearance must fail");
    for (auto url : {L"http://example.com", L"https://secret@example.com", L"https://example.com?api_key=secret", L"https://example.com/#fragment", L"https://example.com\r\nAuthorization: bad"}) {
        bad = settings; bad.baseUrl = url; Throws([&] { ValidateSettings(bad); }, "Unsafe API URL must fail");
    }
    bad = settings; bad.baseUrl = L"http://127.0.0.1:45678/v1"; ValidateSettings(bad);
    PersistentState state;
    state.tab = 4; state.window = RECT{-1100, 100, -200, 800};
    store.SaveState(state);
    auto readState = store.LoadState();
    Check(readState.window->left == -1100 && readState.tab == 4, "Window state round trip");
    auto legacy = nlohmann::json::parse(File(temp.path / "state.json"));
    legacy["fixed"] = {{"monitorId", "OLD"}, {"x", .1}, {"y", .2}, {"width", .4}, {"height", .3}, {"monitorWidth", 1920}, {"monitorHeight", 1080}};
    { std::ofstream output(temp.path / "state.json", std::ios::binary); output << legacy.dump(); }
    store.SaveState(store.LoadState());
    Check(!nlohmann::json::parse(File(temp.path / "state.json")).contains("fixed"), "Legacy region ignored and removed from state writes");
    Check(!std::filesystem::exists(temp.path / "history") && !std::filesystem::exists(temp.path / "logs"), "Settings and state operations must not create history or logs");
    auto oldConfig = nlohmann::json::parse(File(temp.path / "config.json"));
    oldConfig.erase("ocrMode");oldConfig.erase("matchTextColor");
    oldConfig["history"] = true;oldConfig["persistRegion"] = true;
    { std::ofstream output(temp.path / "config.json", std::ios::binary); output << oldConfig.dump(); }
    std::filesystem::create_directory(temp.path / "history");
    std::filesystem::create_directory(temp.path / "logs");
    { std::ofstream output(temp.path / "history" / "translations.jsonl", std::ios::binary); output << "legacy history sentinel"; }
    { std::ofstream output(temp.path / "logs" / "errors.jsonl", std::ios::binary); output << "legacy error sentinel"; }
    auto migrated = store.LoadSettings();
    Check(migrated.matchTextColor,"Legacy settings enable original color matching");
    Check(migrated.ocrMode == 0, "Legacy configuration defaults to bundled automatic mode");
    store.SaveSettings(migrated);
    Check(!nlohmann::json::parse(File(temp.path / "config.json")).contains("persistRegion"), "Removed persistence option is not written back");
    Check(!nlohmann::json::parse(File(temp.path / "config.json")).contains("history"), "Legacy history flag is ignored and omitted on save");
    auto oldState = nlohmann::json::parse(File(temp.path / "state.json"));
    oldState["tab"] = 5;
    { std::ofstream output(temp.path / "state.json", std::ios::binary); output << oldState.dump(); }
    auto migratedState = store.LoadState();
    Check(migratedState.tab == 4, "Legacy removed tab migrates to the last available tab");
    store.SaveState(migratedState);
    Check(File(temp.path / "history" / "translations.jsonl") == "legacy history sentinel" && File(temp.path / "logs" / "errors.jsonl") == "legacy error sentinel", "Existing legacy data must remain untouched");
    auto invalidTab = state; invalidTab.tab = 5;
    Throws([&] { store.SaveState(invalidTab); }, "New state must not save removed tab");
    std::string diagnostic = "request " + secret + "\nAuthorization: Bearer alternate-secret\nimage=data:image/jpeg;base64," + std::string(200, 'A') + "\nBearer other-secret\nsk-test-fallback";
    auto errors = Sanitize(diagnostic, secret);
    Check(errors.find(secret) == std::string::npos && errors.find("alternate-secret") == std::string::npos && errors.find("other-secret") == std::string::npos && errors.find("sk-test-fallback") == std::string::npos && errors.find(std::string(80, 'A')) == std::string::npos, "Ephemeral errors redact secrets, headers, and images");
    Check(Sanitize(std::string(1000000, 'A')).find("AAAA") == std::string::npos, "Large diagnostics are safely omitted");
    auto backup = File(temp.path / "config.json.bak");
    { std::ofstream output(temp.path / "config.json", std::ios::binary); output << "{broken"; }
    Throws([&] { store.LoadSettings(); }, "Malformed config must report failure");
    Throws([&] { store.SaveSettings(settings); }, "Malformed config must not be silently replaced");
    Check(File(temp.path / "config.json") == "{broken" && File(temp.path / "config.json.bak") == backup, "Corrupt config and backup remain intact");
    { std::ofstream output(temp.path / "state.json", std::ios::binary); output << "[]"; }
    Throws([&] { store.LoadState(); }, "Malformed state must report failure");
    Throws([&] { store.SaveState(state); }, "Malformed state must not be silently replaced");
    std::cout << "Store tests passed (DPAPI, validation, persistence, no history/log writes, legacy preservation, corruption recovery).\n";
}



