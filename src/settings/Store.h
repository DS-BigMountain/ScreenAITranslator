#pragma once
#include "common/Types.h"
namespace sat {
// Optional root is for isolated tests; the app defaults to LocalAppData.
class Store {
 std::filesystem::path root_;
public:
 explicit Store(std::filesystem::path root = {});
 const std::filesystem::path& Root() const { return root_; }
 Settings LoadSettings();
 void SaveSettings(const Settings& value);
 PersistentState LoadState();
 void SaveState(const PersistentState& state);
};
std::string ProtectSecret(const std::string& secret);
std::string UnprotectSecret(const std::string& encrypted);
void SetStartup(bool enabled, const std::wstring& executable);
void ValidateSettings(const Settings& settings);
std::string Sanitize(const std::string& text, const std::string& key = {});
}

