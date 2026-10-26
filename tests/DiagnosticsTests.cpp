#include "common/Diagnostics.h"
#include <fstream>
#include <iostream>
#include <thread>
using namespace sat;
using Json = nlohmann::json;
namespace {
void Require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
std::vector<Json> ReadRows(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<Json> rows;
    std::string line;
    while (std::getline(input, line))
        rows.push_back(Json::parse(line));
    return rows;
}
} // namespace
int main() {
    try {
        auto root =
            std::filesystem::absolute("test-output") /
            ("diagnostics-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        InitializeDiagnostics(root);
        {
            DiagnosticContext context(42);
            DiagnosticSpan total("translation");
            DiagnosticEvent("privacy", {{"api_key", "SENSITIVE_KEY"},
                                        {"body", "SENSITIVE_BODY"},
                                        {"original_text", "SENSITIVE_TEXT"},
                                        {"headers", "SENSITIVE_HEADER"},
                                        {"url", "https://SENSITIVE_URL"},
                                        {"request_bytes", 123},
                                        {"missing_ids", Json::array({1, 2, 300, "SENSITIVE_ID"})}});
            {
                DiagnosticSpan request("http_headers");
                request.Fail(AppError("Requesting", "请求超时 SENSITIVE_ERROR", 12002));
            }
            total.End("error");
        }
        DiagnosticEvent("outside");
        auto rows = ReadRows(root / "diagnostic.jsonl");
        Require(rows.size() == 6, "missing diagnostic rows");
        Require(rows[1]["operation"] == 42 && rows[1]["request_bytes"] == 123 &&
                    rows[1]["missing_ids"] == Json::array({1, 2}),
                "operation context or field filtering failed");
        Require(rows[3]["stage"] == "http_headers" && rows[3]["reason"] == "timeout" &&
                    rows[3]["status"] == 12002 && rows[3].contains("duration_ms"),
                "timeout stage missing");
        Require(rows[2]["parent"] == rows[0]["span"] && rows.back()["operation"] == 0,
                "nested context not restored");
        for (const auto& row : rows)
            Require(row.dump().find("SENSITIVE") == std::string::npos, "sensitive value reached log");
        std::vector<std::jthread> workers;
        for (int worker = 0; worker < 4; ++worker)
            workers.emplace_back([worker] {
                DiagnosticContext context(100 + worker);
                for (int i = 0; i < 30; ++i)
                    DiagnosticEvent("parallel", {{"attempt", i}});
            });
        workers.clear();
        rows = ReadRows(root / "diagnostic.jsonl");
        Require(rows.size() == 126, "concurrent writes lost or corrupted records");
        auto rotation = root / "rotation";
        InitializeDiagnostics(rotation, 4096);
        for (int i = 0; i < 200; ++i)
            DiagnosticEvent("rotation", {{"attempt", i}, {"model", std::string(160, 'x')}});
        size_t files = 0;
        bool newest = false;
        for (const auto& entry : std::filesystem::directory_iterator(rotation)) {
            ++files;
            Require(entry.file_size() <= 4096, "rotation exceeded file bound");
            for (const auto& row : ReadRows(entry.path()))
                newest = newest || row.value("attempt", -1) == 199;
        }
        Require(files == 4 && newest, "rotation did not retain latest rows within four files");
        auto blocked = root / "not-a-directory";
        {
            std::ofstream out(blocked);
            out << "existing file";
        }
        InitializeDiagnostics(blocked);
        {
            DiagnosticSpan request("unwritable");
            request.End();
        }
        Require(std::filesystem::is_regular_file(blocked), "logging failure damaged existing file");
        std::cout << "Diagnostics: privacy, correlation, timeout classification, concurrency, rotation and "
                     "write failure passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
