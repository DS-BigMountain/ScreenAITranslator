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
size_t traceKeep{30};
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
        size_t keep = 30;
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
namespace {
uint32_t Crc32Update(uint32_t crc, const uint8_t* data, size_t len) noexcept {
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
        for (int b = 0; b < 8; ++b)
            c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}
void WriteLe16(std::ostream& out, uint16_t v) {
    const char buf[2]{char(v & 0xFF), char((v >> 8) & 0xFF)};
    out.write(buf, 2);
}
void WriteLe32(std::ostream& out, uint32_t v) {
    const char buf[4]{char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char((v >> 24) & 0xFF)};
    out.write(buf, 4);
}
void CurrentDosDateTime(uint16_t& dosTime, uint16_t& dosDate) noexcept {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const uint16_t y = st.wYear >= 1980 ? uint16_t(st.wYear - 1980) : 0;
    dosDate = static_cast<uint16_t>((y << 9) | (st.wMonth << 5) | st.wDay);
    dosTime = static_cast<uint16_t>((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
}
std::vector<std::filesystem::path> RecentTraceDirectories(const std::filesystem::path& logsRoot,
                                                          size_t maxSessions) {
    std::vector<std::filesystem::path> dirs;
    std::error_code code;
    const auto traceDir = logsRoot / L"trace";
    if (!std::filesystem::is_directory(traceDir, code))
        return dirs;
    for (std::filesystem::directory_iterator it(traceDir, code), end; !code && it != end; it.increment(code)) {
        if (!it->is_directory(code))
            continue;
        auto name = it->path().filename().string();
        if (StartsWithDigit(name))
            dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
        return a.filename().u8string() > b.filename().u8string();
    });
    if (dirs.size() > maxSessions)
        dirs.resize(maxSessions);
    return dirs;
}
} // namespace
bool HasExportableTraces(const std::filesystem::path& logsRoot) noexcept {
    try {
        std::error_code code;
        if (!RecentTraceDirectories(logsRoot, 1).empty())
            return true;
        return std::filesystem::is_regular_file(logsRoot / L"diagnostic.jsonl", code);
    } catch (...) {
        return false;
    }
}
TraceExportResult ExportRecentTracesArchive(const std::filesystem::path& logsRoot,
                                            const std::filesystem::path& zipPath, size_t maxSessions) {
    if (zipPath.empty())
        throw AppError("logs", "请选择有效的日志保存路径。");
    const size_t limit = std::clamp<size_t>(maxSessions, 1, 200);
    auto sessions = RecentTraceDirectories(logsRoot, limit);
    std::error_code code;
    const auto diagFile = logsRoot / L"diagnostic.jsonl";
    const bool hasDiag = std::filesystem::is_regular_file(diagFile, code);
    if (sessions.empty() && !hasDiag)
        throw AppError("logs", "当前暂无可导出的翻译日志，请先开启“记录日志”并进行翻译。");

    struct ZipEntry {
        std::string name;
        std::filesystem::path diskPath;
        std::string memoryBytes;
        uint32_t crc{};
        uint32_t size{};
        uint32_t localOffset{};
    };
    std::vector<ZipEntry> entries;
    auto pushFile = [&](std::string archiveName, const std::filesystem::path& path) {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(path, ec);
        if (ec || sz > 256ull * 1024 * 1024)
            return;
        ZipEntry e;
        e.name = std::move(archiveName);
        e.diskPath = path;
        e.size = static_cast<uint32_t>(sz);
        entries.push_back(std::move(e));
    };

    nlohmann::json exportedList = nlohmann::json::array();
    for (const auto& dir : sessions) {
        const auto sessionNameU8 = dir.filename().u8string();
        std::string sessionName(sessionNameU8.begin(), sessionNameU8.end());
        exportedList.push_back(sessionName);
        for (std::filesystem::recursive_directory_iterator it(dir, code), end; !code && it != end;
             it.increment(code)) {
            if (!it->is_regular_file(code))
                continue;
            const auto rel = std::filesystem::relative(it->path(), dir, code);
            if (code)
                continue;
            const auto relU8 = rel.generic_u8string();
            std::string relStr(relU8.begin(), relU8.end());
            pushFile("trace/" + sessionName + "/" + relStr, it->path());
        }
    }
    if (hasDiag)
        pushFile("diagnostic.jsonl", diagFile);

    {
        nlohmann::json summary{{"exported_at", LocalStamp(false)},
                               {"max_sessions", limit},
                               {"exported_sessions", sessions.size()},
                               {"sessions", exportedList}};
        ZipEntry meta;
        meta.name = "export_summary.json";
        meta.memoryBytes = summary.dump(2);
        meta.size = static_cast<uint32_t>(meta.memoryBytes.size());
        entries.push_back(std::move(meta));
    }

    if (entries.size() <= 1 && !hasDiag)
        throw AppError("logs", "当前暂无可导出的翻译日志文件。");

    std::filesystem::create_directories(zipPath.parent_path(), code);
    auto tempZip = zipPath;
    tempZip += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());

    uint16_t dosTime{}, dosDate{};
    CurrentDosDateTime(dosTime, dosDate);

    {
        std::ofstream out(tempZip, std::ios::binary | std::ios::trunc);
        if (!out)
            throw AppError("logs", "无法创建目标日志压缩包，请检查保存目录权限。");

        constexpr size_t ChunkSize = 64 * 1024;
        std::vector<char> buffer(ChunkSize);
        for (auto& e : entries) {
            uint32_t crc = 0;
            std::string dataCopy;
            if (!e.diskPath.empty()) {
                std::ifstream in(e.diskPath, std::ios::binary);
                if (!in)
                    continue;
                while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
                    const size_t n = static_cast<size_t>(in.gcount());
                    crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(buffer.data()), n);
                }
            } else {
                crc = Crc32Update(0, reinterpret_cast<const uint8_t*>(e.memoryBytes.data()),
                                  e.memoryBytes.size());
            }
            e.crc = crc;
            e.localOffset = static_cast<uint32_t>(out.tellp());
            constexpr uint16_t utf8Flags = 0x0800;
            WriteLe32(out, 0x04034B50u);
            WriteLe16(out, 20);
            WriteLe16(out, utf8Flags);
            WriteLe16(out, 0);
            WriteLe16(out, dosTime);
            WriteLe16(out, dosDate);
            WriteLe32(out, e.crc);
            WriteLe32(out, e.size);
            WriteLe32(out, e.size);
            WriteLe16(out, static_cast<uint16_t>(e.name.size()));
            WriteLe16(out, 0);
            out.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
            if (!e.diskPath.empty()) {
                std::ifstream in(e.diskPath, std::ios::binary);
                while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0)
                    out.write(buffer.data(), in.gcount());
            } else if (!e.memoryBytes.empty()) {
                out.write(e.memoryBytes.data(), static_cast<std::streamsize>(e.memoryBytes.size()));
            }
        }

        const uint32_t cdOffset = static_cast<uint32_t>(out.tellp());
        uint16_t cdCount = 0;
        for (const auto& e : entries) {
            constexpr uint16_t utf8Flags = 0x0800;
            WriteLe32(out, 0x02014B50u);
            WriteLe16(out, 20);
            WriteLe16(out, 20);
            WriteLe16(out, utf8Flags);
            WriteLe16(out, 0);
            WriteLe16(out, dosTime);
            WriteLe16(out, dosDate);
            WriteLe32(out, e.crc);
            WriteLe32(out, e.size);
            WriteLe32(out, e.size);
            WriteLe16(out, static_cast<uint16_t>(e.name.size()));
            WriteLe16(out, 0);
            WriteLe16(out, 0);
            WriteLe16(out, 0);
            WriteLe16(out, 0);
            WriteLe32(out, 0);
            WriteLe32(out, e.localOffset);
            out.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
            ++cdCount;
        }
        const uint32_t cdEnd = static_cast<uint32_t>(out.tellp());
        WriteLe32(out, 0x06054B50u);
        WriteLe16(out, 0);
        WriteLe16(out, 0);
        WriteLe16(out, cdCount);
        WriteLe16(out, cdCount);
        WriteLe32(out, cdEnd - cdOffset);
        WriteLe32(out, cdOffset);
        WriteLe16(out, 0);
        out.flush();
        if (!out) {
            std::filesystem::remove(tempZip, code);
            throw AppError("logs", "写入日志压缩包失败，请检查磁盘空间或权限。");
        }
    }

    if (!MoveFileExW(tempZip.c_str(), zipPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tempZip, code);
        throw AppError("logs", "无法保存日志压缩包到指定位置。");
    }
    return {sessions.size(), entries.size()};
}
} // namespace sat
