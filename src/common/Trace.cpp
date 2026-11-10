#include "common/Trace.h"
#include "capture/Capture.h"
#include "common/Diagnostics.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace sat {
namespace {
std::mutex stateMutex;
std::filesystem::path traceRoot;
bool traceEnabled{};
size_t traceKeep{20};
std::atomic<unsigned> traceSequence{};
thread_local TraceSession* ambientSession{};

std::string LocalStamp(bool compact) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char text[48]{};
    if (compact)
        sprintf_s(text, "%04u%02u%02u-%02u%02u%02u-%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond, t.wMilliseconds);
    else
        sprintf_s(text, "%04u-%02u-%02u %02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond, t.wMilliseconds);
    return text;
}
bool StartsWithDigit(const std::string& text) {
    return !text.empty() && std::isdigit(static_cast<unsigned char>(text.front()));
}
std::vector<unsigned char> DecodeBase64(std::string_view text) {
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };
    std::vector<unsigned char> result;
    result.reserve(text.size() / 4 * 3);
    unsigned buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=')
            break;
        const int value = digit(c);
        if (value < 0)
            continue;
        buffer = (buffer << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
        }
    }
    return result;
}
bool LooksLikeImageData(const std::string& text) {
    if (text.size() <= 2048)
        return false;
    const size_t limit = std::min<size_t>(text.size(), 4096);
    for (size_t i = 0; i < limit; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (!std::isalnum(c) && c != '+' && c != '/' && c != '=')
            return false;
    }
    return true;
}
// Replaces the embedded image payload with a marker so the JSON record stays readable. The bytes
// themselves travel to upload.jpg, which keeps the record exact and the request file small.
void OmitImageData(nlohmann::json& node, std::string& imageBase64) {
    if (node.is_object()) {
        for (auto& item : node.items())
            OmitImageData(item.value(), imageBase64);
    } else if (node.is_array()) {
        for (auto& item : node)
            OmitImageData(item, imageBase64);
    } else if (node.is_string()) {
        auto* text = node.get_ptr<std::string*>();
        if (!text)
            return;
        static constexpr std::string_view prefix = "data:image/jpeg;base64,";
        if (text->starts_with(prefix)) {
            imageBase64 = text->substr(prefix.size());
            *text = "<image omitted: see upload.jpg>";
        } else if (LooksLikeImageData(*text)) {
            imageBase64 = *text;
            *text = "<image omitted: see upload.jpg>";
        }
    }
}
std::string ExtractContent(std::string_view body) {
    try {
        auto root = nlohmann::json::parse(body);
        if (root.contains("choices") && root["choices"].is_array() && !root["choices"].empty()) {
            const auto& choice = root["choices"][0];
            if (choice.contains("message") && choice["message"].is_object() &&
                choice["message"].contains("content") && choice["message"]["content"].is_string())
                return choice["message"]["content"].get<std::string>();
        }
        if (root.contains("candidates") && root["candidates"].is_array() && !root["candidates"].empty()) {
            std::string text;
            const auto& candidate = root["candidates"][0];
            if (candidate.contains("content") && candidate["content"].contains("parts"))
                for (const auto& part : candidate["content"]["parts"])
                    if (part.contains("text") && part["text"].is_string())
                        text += part["text"].get<std::string>();
            if (!text.empty())
                return text;
        }
        if (root.contains("content") && root["content"].is_array()) {
            std::string text;
            for (const auto& part : root["content"])
                if (part.is_object() && part.value("type", std::string{}) == "text" && part.contains("text"))
                    text += part["text"].get<std::string>();
            if (!text.empty())
                return text;
        }
        if (root.contains("translations") && root["translations"].is_array() && !root["translations"].empty()) {
            const auto& first = root["translations"][0];
            if (first.contains("text") && first["text"].is_string())
                return first["text"].get<std::string>();
        }
    } catch (...) {
    }
    return {};
}
std::string ShortValue(const nlohmann::json& value) {
    try {
        if (value.is_string()) {
            auto text = value.get<std::string>();
            if (text.size() > 48)
                text = text.substr(0, 48) + "…";
            return text;
        }
        if (value.is_array()) {
            auto text = value.dump();
            return text.size() > 48 ? text.substr(0, 48) + "…" : text;
        }
        if (value.is_object())
            return "{}";
        return value.dump();
    } catch (...) {
        return "?";
    }
}
std::string Detail(const nlohmann::json& record) {
    static constexpr std::string_view skip[]{"stage", "span", "parent", "outcome", "duration_ms",
                                             "offset_ms", "thread"};
    std::string detail;
    for (auto it = record.begin(); it != record.end(); ++it) {
        const auto& key = it.key();
        if (std::find(std::begin(skip), std::end(skip), key) != std::end(skip))
            continue;
        if (!detail.empty())
            detail += " · ";
        detail += key + "=" + ShortValue(it.value());
    }
    return detail;
}
std::string Number(double value) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << value;
    return text.str();
}
} // namespace

struct TraceSession::Impl {
    std::filesystem::path directory;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    nlohmann::json meta = nlohmann::json::object();
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> tasks;
    std::vector<nlohmann::json> stages;
    std::thread writer;
    bool closing{};
    int attempts{};
    int current{};

    void Push(std::function<void()> task) noexcept {
        {
            std::lock_guard lock(mutex);
            if (closing)
                return;
            tasks.push_back(std::move(task));
        }
        wake.notify_one();
    }
    static void WriteFile(const std::filesystem::path& path, std::string_view bytes) {
        std::error_code code;
        std::filesystem::create_directories(path.parent_path(), code);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!bytes.empty())
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    void Run() noexcept {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return closing || !tasks.empty(); });
                if (tasks.empty())
                    return;
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            try {
                task();
            } catch (...) {
            }
        }
    }
    std::vector<nlohmann::json> StageSnapshot() {
        std::lock_guard lock(mutex);
        return stages;
    }
    std::string RenderTiming(const std::vector<nlohmann::json>& records) const {
        const double elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::vector<const nlohmann::json*> rows;
        rows.reserve(records.size());
        for (const auto& record : records)
            rows.push_back(&record);
        std::sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) {
            return a->value("offset_ms", 0.0) < b->value("offset_ms", 0.0);
        });
        std::map<int, int> depths;
        std::ostringstream text;
        text << "# 请求明细 · 阶段耗时\n\n";
        if (meta.contains("started") && meta["started"].is_string())
            text << "- 开始时间：" << meta["started"].get<std::string>() << "（本地）\n";
        text << "- 进程：" << meta.value("pid", 0) << " · 操作 " << meta.value("operation", 0ull) << "\n";
        if (meta.contains("summary") && meta["summary"].is_object()) {
            static constexpr std::pair<std::string_view, std::string_view> labels[]{
                {"total_ms", "翻译总计"},  {"ocr_ms", "本地识别"},   {"api_ms", "接口请求"},
                {"segments", "译文段"},    {"request_count", "请求次数"}};
            for (const auto& [key, label] : labels) {
                if (!meta["summary"].contains(key))
                    continue;
                const auto& value = meta["summary"][key];
                text << "- " << label << "：";
                if (std::string_view(key).ends_with("_ms"))
                    text << (value.is_number_integer() ? std::to_string(value.get<long long>())
                                                      : Number(value.get<double>()))
                         << " ms\n";
                else
                    text << ShortValue(value) << "\n";
            }
        }
        text << "- 记录总时长：" << Number(elapsed) << " ms\n";
        // Local recognition and the service request travel together; naming the overlap explains why
        // the total stays below their sum.
        const nlohmann::json *ocrStage = nullptr, *apiStage = nullptr;
        for (const auto* record : rows) {
            const auto stage = record->value("stage", std::string{});
            if (stage == "ocr")
                ocrStage = record;
            else if (stage == "api")
                apiStage = record;
        }
        if (ocrStage && apiStage) {
            const double startOcr = ocrStage->value("offset_ms", 0.0);
            const double endOcr = startOcr + ocrStage->value("duration_ms", 0.0);
            const double startApi = apiStage->value("offset_ms", 0.0);
            const double endApi = startApi + apiStage->value("duration_ms", 0.0);
            const double overlap = std::max(0.0, std::min(endOcr, endApi) - std::max(startOcr, startApi));
            if (overlap >= 30)
                text << "- 并行重叠：" << Number(overlap)
                     << " ms（本地识别与接口请求同时进行）\n";
        }
        text << "\n| 阶段 | 起始 | 耗时 | 占比 | 结果 | 明细 |\n| --- | ---: | ---: | ---: | --- | --- |\n";
        for (const auto* record : rows) {
            const int span = record->value("span", 0);
            const int parent = record->value("parent", 0);
            int depth = 0;
            if (parent) {
                const auto found = depths.find(parent);
                depth = found == depths.end() ? 1 : found->second + 1;
            }
            depths[span] = depth;
            std::string name;
            for (int i = 0; i < depth; ++i)
                name += "⤷ ";
            name += record->value("stage", std::string{});
            const double duration = record->value("duration_ms", 0.0);
            text << "| " << name << " | " << Number(record->value("offset_ms", 0.0)) << " | "
                 << Number(duration) << " | "
                 << Number(elapsed > 0 ? duration * 100.0 / elapsed : 0.0) << "% | "
                 << record->value("outcome", std::string{}) << " | " << Detail(*record) << " |\n";
        }
        return text.str();
    }
    void WriteSummary() noexcept {
        try {
            const auto records = StageSnapshot();
            nlohmann::json document = meta;
            document["duration_ms"] =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            document["stages"] = records;
            nlohmann::json files = nlohmann::json::array();
            std::error_code code;
            for (std::filesystem::directory_iterator it(directory, code), end; !code && it != end;
                 it.increment(code)) {
                if (!it->is_regular_file()) {
                    for (std::filesystem::directory_iterator inner(it->path(), code), innerEnd;
                         !code && inner != innerEnd; inner.increment(code))
                        if (inner->is_regular_file()) {
                            const auto bytes = std::filesystem::file_size(inner->path(), code);
                            files.push_back({{"name", it->path().filename().string() + "/" +
                                                  inner->path().filename().string()},
                                             {"bytes", code ? 0 : bytes}});
                        }
                    continue;
                }
                const auto bytes = std::filesystem::file_size(it->path(), code);
                files.push_back({{"name", it->path().filename().string()}, {"bytes", code ? 0 : bytes}});
            }
            document["files"] = files;
            WriteFile(directory / "session.json", document.dump(2));
            WriteFile(directory / "timing.json",
                      nlohmann::json{{"duration_ms", document["duration_ms"]}, {"stages", records}}.dump(2));
            WriteFile(directory / "timing.md", RenderTiming(records));
        } catch (...) {
        }
    }
};

bool TraceEnabled() noexcept {
    std::lock_guard lock(stateMutex);
    return traceEnabled;
}
std::filesystem::path TraceRoot() noexcept {
    std::lock_guard lock(stateMutex);
    return traceRoot;
}
void PruneTraces() noexcept {
    try {
        std::filesystem::path root;
        size_t keep = 20;
        {
            std::lock_guard lock(stateMutex);
            root = traceRoot;
            keep = traceKeep;
        }
        if (root.empty())
            return;
        std::vector<std::string> names;
        std::error_code code;
        for (std::filesystem::directory_iterator it(root, code), end; !code && it != end; it.increment(code))
            if (it->is_directory()) {
                auto name = it->path().filename().string();
                if (StartsWithDigit(name))
                    names.push_back(std::move(name));
            }
        std::sort(names.begin(), names.end());
        while (names.size() > keep) {
            std::filesystem::remove_all(root / names.front(), code);
            names.erase(names.begin());
        }
    } catch (...) {
    }
}
void InitializeTrace(const std::filesystem::path& directory, bool enabled, size_t keep) noexcept {
    try {
        std::lock_guard lock(stateMutex);
        traceRoot = directory.empty() ? std::filesystem::path{} : std::filesystem::absolute(directory);
        traceEnabled = enabled && !traceRoot.empty();
        traceKeep = std::clamp<size_t>(keep, 1, 500);
        if (traceEnabled)
            std::filesystem::create_directories(traceRoot);
    } catch (...) {
    }
    PruneTraces();
}
TraceSession::TraceSession(const char* kind, nlohmann::json fields) noexcept {
    try {
        if (!TraceEnabled() || ambientSession)
            return;
        std::filesystem::path root;
        {
            std::lock_guard lock(stateMutex);
            root = traceRoot;
        }
        if (root.empty())
            return;
        auto session = std::make_unique<Impl>();
        session->meta = fields.is_object() ? std::move(fields) : nlohmann::json::object();
        session->meta["kind"] = kind ? kind : "";
        session->meta["operation"] = DiagnosticOperation();
        session->meta["started"] = LocalStamp(false);
        session->meta["version"] = SAT_VERSION;
        session->meta["pid"] = GetCurrentProcessId();
        std::error_code code;
        // A fixed-width sequence keeps directory ordering stable when two translations start in the
        // same millisecond.
        char suffix[24]{};
        sprintf_s(suffix, "-%03u", ++traceSequence);
        session->directory = root / (LocalStamp(true) + suffix + "_op" + std::to_string(DiagnosticOperation()));
        std::filesystem::create_directories(session->directory, code);
        if (code)
            return;
        session->writer = std::thread([raw = session.get()] { raw->Run(); });
        impl_ = std::move(session);
        ambientSession = this;
    } catch (...) {
        impl_.reset();
    }
}
TraceSession::~TraceSession() {
    if (!impl_)
        return;
    if (ambientSession == this)
        ambientSession = nullptr;
    auto* session = impl_.get();
    session->Push([session] { session->WriteSummary(); });
    {
        std::lock_guard lock(session->mutex);
        session->closing = true;
    }
    session->wake.notify_all();
    if (session->writer.joinable())
        session->writer.join();
    PruneTraces();
}
TraceSession::operator bool() const noexcept {
    return impl_ != nullptr;
}
const std::filesystem::path& TraceSession::Directory() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->directory : empty;
}
void TraceSession::Meta(nlohmann::json fields) noexcept {
    if (!impl_ || !fields.is_object())
        return;
    try {
        for (auto it = fields.begin(); it != fields.end(); ++it)
            impl_->meta[it.key()] = it.value();
    } catch (...) {
    }
}
void TraceSession::Write(const char* name, std::string_view bytes) noexcept {
    if (!impl_ || !name)
        return;
    try {
        const auto path = impl_->directory / name;
        std::string data(bytes);
        impl_->Push([path, data = std::move(data)] { Impl::WriteFile(path, data); });
    } catch (...) {
    }
}
void TraceSession::WriteText(const char* name, std::string_view text) noexcept {
    Write(name, text);
}
void TraceSession::WriteJson(const char* name, const nlohmann::json& value) noexcept {
    if (!impl_)
        return;
    try {
        Write(name, value.dump(2));
    } catch (...) {
    }
}
void TraceSession::WritePng(const char* name, const Image& image) noexcept {
    if (!impl_ || !name)
        return;
    try {
        const auto path = impl_->directory / name;
        auto copy = image;
        impl_->Push([path, copy = std::move(copy)] {
            try {
                const auto png = EncodePng(copy, {});
                const std::string_view bytes(reinterpret_cast<const char*>(png.data()), png.size());
                Impl::WriteFile(path, bytes);
            } catch (...) {
            }
        });
    } catch (...) {
    }
}
int TraceSession::BeginAttempt() noexcept {
    if (!impl_)
        return 0;
    std::lock_guard lock(impl_->mutex);
    impl_->current = ++impl_->attempts;
    return impl_->current;
}
int TraceSession::CurrentAttempt() const noexcept {
    if (!impl_)
        return 0;
    std::lock_guard lock(impl_->mutex);
    return impl_->current;
}
double TraceSession::OffsetMs(std::chrono::steady_clock::time_point moment) const noexcept {
    if (!impl_)
        return 0;
    return std::chrono::duration<double, std::milli>(moment - impl_->start).count();
}
void TraceSession::Record(const nlohmann::json& stage) noexcept {
    if (!impl_)
        return;
    try {
        std::lock_guard lock(impl_->mutex);
        impl_->stages.push_back(stage);
    } catch (...) {
    }
}
TraceContext::TraceContext(TraceSession* session) noexcept : previous_(ambientSession) {
    ambientSession = session;
}
TraceContext::~TraceContext() {
    ambientSession = previous_;
}
TraceSession* ActiveTrace() noexcept {
    return ambientSession;
}

void TraceRequest(const nlohmann::json& request, const std::vector<TextRegion>& regions,
                  const std::string& imageBase64, bool strict, bool textOnly) noexcept {
    auto* session = ActiveTrace();
    if (!session)
        return;
    try {
        const int attempt = session->BeginAttempt();
        if (!attempt)
            return;
        const std::string folder = std::to_string(attempt) + "/";
        auto record = request;
        std::string embedded;
        OmitImageData(record, embedded);
        session->Write((folder + "request.json").c_str(), record.dump(2));
        if (request.contains("messages") && request["messages"].is_array() &&
            request["messages"].size() >= 2) {
            const auto& messages = request["messages"];
            if (messages[0].is_object() && messages[0].contains("content"))
                session->WriteText((folder + "request.system.txt").c_str(),
                                   messages[0]["content"].is_string()
                                       ? messages[0]["content"].get<std::string>()
                                       : messages[0]["content"].dump(2));
            if (messages[1].is_object() && messages[1].contains("content")) {
                const auto& content = messages[1]["content"];
                std::string user;
                if (content.is_string())
                    user = content.get<std::string>();
                else if (content.is_array())
                    for (const auto& part : content)
                        if (part.is_object() && part.value("type", std::string{}) == "text" &&
                            part.contains("text") && part["text"].is_string())
                            user += part["text"].get<std::string>();
                if (!user.empty())
                    session->WriteText((folder + "request.user.txt").c_str(), user);
            }
        }
        if (request.contains("response_format") && request["response_format"].is_object()) {
            const auto& format = request["response_format"];
            if (format.contains("json_schema") && format["json_schema"].is_object())
                session->WriteJson((folder + "request.schema.json").c_str(),
                                   format["json_schema"].value("schema", nlohmann::json::object()));
            session->WriteJson((folder + "request.format.json").c_str(),
                               {{"format", format.value("type", std::string{})},
                                {"strict", strict},
                                {"text_only", textOnly}});
        }
        if (!regions.empty()) {
            auto listed = nlohmann::json::array();
            for (const auto& region : regions)
                listed.push_back({{"source_id", region.id},
                                  {"confidence", region.confidence},
                                  {"original_text", region.text},
                                  {"pixels",
                                   {{"x", region.rect.left},
                                    {"y", region.rect.top},
                                    {"width", Width(region.rect)},
                                    {"height", Height(region.rect)}}},
                                  {"line_height", region.lineHeight},
                                  {"lines", region.lines.size()}});
            session->WriteJson((folder + "request.regions.json").c_str(), listed);
        }
        const std::string& image = embedded.empty() ? imageBase64 : embedded;
        if (!image.empty() && attempt == 1) {
            const auto bytes = DecodeBase64(image);
            if (!bytes.empty())
                session->Write("upload.jpg",
                               std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        }
    } catch (...) {
    }
}
void TraceRawRequest(const nlohmann::json& body, const std::string& imageBase64) noexcept {
    auto* session = ActiveTrace();
    if (!session)
        return;
    try {
        const int attempt = session->BeginAttempt();
        if (!attempt)
            return;
        const std::string folder = std::to_string(attempt) + "/";
        auto record = body;
        std::string embedded;
        OmitImageData(record, embedded);
        session->Write((folder + "request.json").c_str(), record.dump(2));
        const std::string& image = embedded.empty() ? imageBase64 : embedded;
        if (!image.empty() && attempt == 1) {
            const auto bytes = DecodeBase64(image);
            if (!bytes.empty())
                session->Write("upload.jpg",
                               std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        }
    } catch (...) {
    }
}
void TraceResponse(int status, std::string_view body, std::string_view endpoint,
                   std::string_view method) noexcept {
    auto* session = ActiveTrace();
    if (!session)
        return;
    try {
        const int attempt = session->CurrentAttempt();
        const std::string folder = attempt ? std::to_string(attempt) + "/" : std::string{};
        session->Write((folder + "response.json").c_str(), body);
        session->WriteJson((folder + "response.meta.json").c_str(),
                           {{"status", status},
                            {"method", std::string(method)},
                            {"endpoint", std::string(endpoint)},
                            {"bytes", body.size()},
                            {"received", LocalStamp(false)}});
        const auto content = ExtractContent(body);
        if (!content.empty())
            session->WriteText((folder + "response.content.txt").c_str(), content);
    } catch (...) {
    }
}
void TraceRequestHeaders(std::string_view headers) noexcept {
    auto* session = ActiveTrace();
    if (!session || headers.empty())
        return;
    try {
        const int attempt = session->CurrentAttempt();
        const std::string folder = attempt ? std::to_string(attempt) + "/" : std::string{};
        session->WriteText((folder + "request.headers.txt").c_str(), headers);
    } catch (...) {
    }
}
void TraceResponseHeaders(std::string_view headers) noexcept {
    auto* session = ActiveTrace();
    if (!session || headers.empty())
        return;
    try {
        const int attempt = session->CurrentAttempt();
        const std::string folder = attempt ? std::to_string(attempt) + "/" : std::string{};
        session->WriteText((folder + "response.headers.txt").c_str(), headers);
    } catch (...) {
    }
}
void TraceParsed(const TranslationResult& result) noexcept {
    auto* session = ActiveTrace();
    if (!session)
        return;
    try {
        nlohmann::json segments = nlohmann::json::array();
        for (const auto& segment : result.segments) {
            nlohmann::json item{{"source_id", segment.sourceId},
                               {"original_text", segment.original},
                               {"translated_text", segment.translated},
                               {"box", {{"x", segment.box.x},
                                        {"y", segment.box.y},
                                        {"width", segment.box.width},
                                        {"height", segment.box.height}}},
                               {"untranslated", segment.untranslated}};
            if (segment.sourcePixels)
                item["source_pixels"] = {{"x", segment.sourcePixels->left},
                                         {"y", segment.sourcePixels->top},
                                         {"width", Width(*segment.sourcePixels)},
                                         {"height", Height(*segment.sourcePixels)}};
            segments.push_back(std::move(item));
        }
        session->WriteJson("parsed.json", {{"language", result.language},
                                           {"requests", result.requestCount},
                                           {"clipped_boxes", result.clippedBoxes},
                                           {"resolved_source_ids", result.resolvedSourceIds},
                                           {"segments", segments}});
    } catch (...) {
    }
}
void TraceStage(const char* stage, unsigned long long parent, const nlohmann::json& fields,
                std::chrono::steady_clock::time_point started, const char* outcome) noexcept {
    auto* session = ActiveTrace();
    if (!session)
        return;
    try {
        auto record = fields.is_object() ? fields : nlohmann::json::object();
        record["stage"] = stage;
        record["parent"] = parent;
        record["outcome"] = outcome;
        record["offset_ms"] = session->OffsetMs(started);
        record["thread"] = GetCurrentThreadId();
        session->Record(record);
    } catch (...) {
    }
}
} // namespace sat
