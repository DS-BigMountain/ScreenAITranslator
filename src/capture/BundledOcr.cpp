#include "BundledOcr.h"
#include "common/Diagnostics.h"
#include "common/TextMetrics.h"
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace sat {
namespace {
std::filesystem::path AppDirectory() {
    std::wstring path(32768, L'\0');
    const auto n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!n || n >= path.size())
        throw AppError("ocr", "Cannot locate application directory");
    path.resize(n);
    return std::filesystem::path(path).parent_path();
}
// Preferred model first; the directory scan covers staged builds that ship one variant only.
struct ModelCandidate {
    const wchar_t* name;
    const char* variant;
};
struct ModelFile {
    std::filesystem::path path;
    const char* variant;
};
std::optional<ModelFile> FindModel(const std::filesystem::path& root,
                                   std::initializer_list<ModelCandidate> candidates) {
    std::error_code error;
    for (const auto& candidate : candidates) {
        const auto path = root / candidate.name;
        if (std::filesystem::is_regular_file(path, error))
            return ModelFile{path, candidate.variant};
    }
    return std::nullopt;
}
ModelFile PickModel(const std::filesystem::path& root, std::initializer_list<ModelCandidate> candidates) {
    if (auto found = FindModel(root, candidates))
        return *found;
    throw AppError("ocr", "Bundled OCR model unavailable");
}
std::initializer_list<ModelCandidate> RecognizerCandidates(int mode) {
    static constexpr std::initializer_list<ModelCandidate> kEnglish{
        {L"models/rec-en-server.onnx", "server"}, {L"models/rec-en.onnx", "mobile"}};
    static constexpr std::initializer_list<ModelCandidate> kKorean{
        {L"models/rec-ko-server.onnx", "server"}, {L"models/rec-ko.onnx", "mobile"}};
    static constexpr std::initializer_list<ModelCandidate> kJapanese{
        {L"models/rec-ja-server.onnx", "server"}, {L"models/rec-ja.onnx", "mobile"}};
    static constexpr std::initializer_list<ModelCandidate> kLatin{
        {L"models/rec-latin.onnx", "mobile"},
        {L"models/rec-ja-server.onnx", "server"},
        {L"models/rec-en.onnx", "mobile"}};
    static constexpr std::initializer_list<ModelCandidate> kRussian{{L"models/rec-ru.onnx", "mobile"}};
    static constexpr std::initializer_list<ModelCandidate> kThai{{L"models/rec-th.onnx", "mobile"}};
    static constexpr std::initializer_list<ModelCandidate> kArabic{{L"models/rec-ar.onnx", "mobile"}};
    static constexpr std::initializer_list<ModelCandidate> kGreek{
        {L"models/rec-el.onnx", "mobile"}, {L"models/rec-ja-server.onnx", "server"}};
    static constexpr std::initializer_list<ModelCandidate> kDevanagari{{L"models/rec-hi.onnx", "mobile"}};
    switch (mode) {
    case OcrEnglish:
        return kEnglish;
    case OcrKorean:
        return kKorean;
    case OcrChinese:
    case OcrJapanese:
        return kJapanese;
    case OcrLatin:
        return kLatin;
    case OcrRussian:
        return kRussian;
    case OcrThai:
        return kThai;
    case OcrArabic:
        return kArabic;
    case OcrGreek:
        return kGreek;
    case OcrDevanagari:
        return kDevanagari;
    default:
        throw AppError("ocr", "Invalid OCR mode");
    }
}
bool HasDedicatedRecognizer(const std::filesystem::path& root, int mode) {
    std::error_code error;
    const auto candidates = RecognizerCandidates(mode);
    if (candidates.size() == 0)
        return false;
    return std::filesystem::is_regular_file(root / candidates.begin()->name, error);
}
struct Model {
    Ort::Session session{nullptr};
    std::string input, output;
    std::vector<std::string> alphabet;
    // Declared input geometry. A zero entry marks a dynamic axis.
    std::array<int64_t, 4> inputShape{};
    Model(Ort::Env& env, Ort::SessionOptions& options, const std::filesystem::path& path, bool recognition,
          const std::filesystem::path& dictPath = {})
        : session(env, path.c_str(), options) {
        Ort::AllocatorWithDefaultOptions allocator;
        input = session.GetInputNameAllocated(0, allocator).get();
        output = session.GetOutputNameAllocated(0, allocator).get();
        const auto shape = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        for (size_t i = 0; i < shape.size() && i < inputShape.size(); ++i)
            inputShape[i] = shape[i];
        if (recognition) {
            // Paddle exports carry the dictionary as metadata; the manga conversion ships it beside
            // the weights instead, so both sources feed the same alphabet layout.
            auto embedded =
                session.GetModelMetadata().LookupCustomMetadataMapAllocated("character", allocator);
            std::string external;
            if (!embedded && !dictPath.empty()) {
                std::ifstream file(dictPath, std::ios::binary);
                if (!file)
                    throw AppError("ocr", "Recognition dictionary unavailable");
                external.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            }
            if (!embedded && external.empty())
                throw AppError("ocr", "Missing recognition dictionary");
            alphabet.push_back("");
            std::istringstream stream(embedded ? std::string(embedded.get()) : external);
            std::string line;
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                alphabet.push_back(line);
            }
            alphabet.push_back(" ");
        }
    }
    // Recognition height follows the exported graph; the PP-OCRv5 and PP-OCRv6
    // RapidOCR conversions both declare 48. A pinned width disables the aspect-ratio path.
    int InputHeight(int fallback) const { return inputShape[2] > 0 ? int(inputShape[2]) : fallback; }
    int InputWidth() const { return inputShape[3] > 0 ? int(inputShape[3]) : 0; }
    Ort::Value Run(std::vector<float>& pixels, int width, int height, Ort::RunOptions& options) {
        std::array<int64_t, 4> shape{1, 3, height, width};
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor =
            Ort::Value::CreateTensor<float>(memory, pixels.data(), pixels.size(), shape.data(), shape.size());
        const char* ins[]{input.c_str()};
        const char* outs[]{output.c_str()};
        auto values = session.Run(options, ins, &tensor, 1, outs, 1);
        return std::move(values[0]);
    }
};
struct Engine {
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::SessionOptions> options;
    std::unique_ptr<Model> detector, recognizer, classifier;
    int recognizerMode{-1};
    std::string detectorVariant, recognizerVariant;
    // Text-line orientation accepts its exported shape; RapidOCR conversions use 80x160
    // while older PaddleOCR classifiers export 48x192.
    int orientationHeight{80}, orientationWidth{160};
    bool orientationShapeKnown{false}, orientationChecked{false};
    Engine() {
        const auto root = AppDirectory();
        // Retain one DLL reference for ORT's process-wide callbacks, not one per capture.
        static const HMODULE module =
            LoadLibraryExW((root / L"onnxruntime.dll").c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            throw AppError("ocr", "Bundled OCR runtime unavailable");
        auto getApi =
            reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(GetProcAddress(module, "OrtGetApiBase"));
        const auto api = getApi ? getApi()->GetApi(ORT_API_VERSION) : nullptr;
        if (!api)
            throw AppError("ocr", "Bundled OCR runtime incompatible");
        Ort::InitApi(api);
        env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "ScreenAITranslator");
        options = std::make_unique<Ort::SessionOptions>();
        options->SetIntraOpNumThreads(4);
        options->SetInterOpNumThreads(1);
        options->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // Dynamic screenshot sizes otherwise grow a per-session arena to the largest
        // inference and keep those pages committed after every translation.
        options->DisableMemPattern();
        options->DisableCpuMemArena();
        auto picked = PickModel(root, {{L"models/det-server.onnx", "server"}, {L"models/det.onnx", "mobile"}});
        detectorVariant = picked.variant;
        DiagnosticEvent("ocr_detector_selected", {{"model", detectorVariant}});
        detector = std::make_unique<Model>(*env, *options, picked.path, false);
    }
    Model& Recognizer(int mode) {
        // Reuse the much smaller line-recognition workspace only within this request.
        // It is destroyed with the recognizer before returning to the cloud pipeline.
        options->EnableCpuMemArena();
        if (recognizerMode != mode) {
            recognizer.reset();
            recognizerVariant.clear();
        }
        if (!recognizer) {
            const auto root = AppDirectory();
            const ModelFile picked = PickModel(root, RecognizerCandidates(mode));
            recognizer = std::make_unique<Model>(*env, *options, picked.path, true);
            recognizerVariant = picked.variant;
            DiagnosticEvent("ocr_recognizer_selected",
                            {{"model", recognizerVariant}, {"ocr_mode", mode}});
        }
        recognizerMode = mode;
        return *recognizer;
    }
    // Missing or unusable orientation weights degrade to horizontal-only recognition.
    Model* Classifier() {
        if (!orientationChecked) {
            orientationChecked = true;
            std::error_code error;
            const auto path = AppDirectory() / L"models/cls.onnx";
            if (std::filesystem::is_regular_file(path, error)) {
                try {
                    options->EnableCpuMemArena();
                    auto model = std::make_unique<Model>(*env, *options, path, false);
                    orientationHeight = model->inputShape[2] > 0 ? int(model->inputShape[2]) : 80;
                    orientationWidth = model->inputShape[3] > 0 ? int(model->inputShape[3]) : 160;
                    orientationShapeKnown = model->inputShape[2] > 0 && model->inputShape[3] > 0;
                    classifier = std::move(model);
                    DiagnosticEvent("ocr_classifier_selected", {{"model", "textline-ori"}});
                } catch (const std::exception&) {
                    DiagnosticEvent("ocr_fallback", {{"reason", "classifier_load_failed"}});
                }
            }
        }
        return classifier.get();
    }
    // Manga pipeline: a dedicated tiny detector plus a recognizer trained on vertical columns. Both
    // load lazily, and a missing set of weights degrades to the general engine instead of failing.
    std::unique_ptr<Model> mangaDetector, mangaRecognizer;
    bool mangaChecked{false};
    void LoadManga() {
        if (mangaChecked)
            return;
        mangaChecked = true;
        std::error_code error;
        const auto root = AppDirectory();
        const auto detectorPath = root / L"models/manga_det.onnx";
        const auto recognizerPath = root / L"models/manga_rec.onnx";
        const auto dictPath = root / L"models/ppocrv6_dict.txt";
        if (!std::filesystem::is_regular_file(detectorPath, error) ||
            !std::filesystem::is_regular_file(recognizerPath, error) ||
            !std::filesystem::is_regular_file(dictPath, error)) {
            DiagnosticEvent("ocr_fallback", {{"reason", "manga_models_missing"}});
            return;
        }
        try {
            options->EnableCpuMemArena();
            mangaDetector = std::make_unique<Model>(*env, *options, detectorPath, false);
            mangaRecognizer = std::make_unique<Model>(*env, *options, recognizerPath, true, dictPath);
            DiagnosticEvent("ocr_manga_selected",
                            {{"detector", "manga_det"}, {"recognizer", "manga_rec"}});
        } catch (const std::exception&) {
            mangaDetector.reset();
            mangaRecognizer.reset();
            DiagnosticEvent("ocr_fallback", {{"reason", "manga_load_failed"}});
        }
    }
    Model* MangaDetector() {
        LoadManga();
        return mangaDetector.get();
    }
    Model* MangaRecognizer() {
        LoadManga();
        return mangaRecognizer.get();
    }
};
// Bilinear BGR input follows the model's Paddle/RapidOCR preprocessing convention.
// rotate180 samples the crop upside down so a 180-degree line reaches the recognizer upright.
// vertical samples a column of upright characters: each character block keeps its own orientation
// and the blocks run left to right, which is the line the recognizer was trained on.
std::vector<float> Tensor(const Image& image, RECT crop, int width, int height, int contentWidth,
                          bool detection, bool rotate180 = false, bool vertical = false,
                          bool quarter = false, int blocks = 0, bool turnLeft = false) {
    std::vector<float> result(size_t(width) * height * 3, 0.f);
    constexpr float mean[]{.485f, .456f, .406f}, stddev[]{.229f, .224f, .225f};
    // A column carries upright characters stacked vertically. The block count is what turns the
    // column into a readable line, and a wrong count shifts every following character.
    const int chars = vertical
                          ? std::max(1, blocks > 0 ? blocks
                                                   : int(std::lround(double(Height(crop)) /
                                                                     std::max(1, Width(crop)))))
                          : 1;
    const double charSpan = double(Height(crop)) / chars;
    const double unitSpan = double(contentWidth) / chars;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < contentWidth; ++x) {
            float py, px;
            if (turnLeft) {
                // Counter-clockwise quarter turn: the crop's top-right corner becomes the target's
                // top-left, so a column read top to bottom arrives left to right.
                px = crop.right - 1 - (y + .5f) * Width(crop) / height;
                py = crop.top + (x + .5f) * Height(crop) / contentWidth - .5f;
            } else if (quarter) {
                // A line rotated a quarter: the target row axis follows the crop column axis.
                const float along = (x + .5f) * Height(crop) / contentWidth - .5f;
                px = crop.left + (y + .5f) * Width(crop) / height - .5f;
                py = float(crop.bottom - 1) - along;
            } else if (vertical) {
                const int block = std::min(chars - 1, int(x / unitSpan));
                const double within = (x - block * unitSpan) / unitSpan;
                px = crop.left + float(within) * Width(crop) - .5f;
                py = crop.top + float(block * charSpan) + (y + .5f) * float(charSpan) / height - .5f;
            } else {
                py = crop.top + (y + .5f) * Height(crop) / height - .5f;
                px = crop.left + (x + .5f) * Width(crop) / contentWidth - .5f;
            }
            py = std::clamp(py, float(crop.top), float(crop.bottom - 1));
            px = std::clamp(px, float(crop.left), float(crop.right - 1));
            float py2 = rotate180 ? float(crop.bottom - 1) - py : py;
            float px2 = rotate180 ? float(crop.right - 1) - px : px;
            int y0 = int(py2), y1 = std::min(y0 + 1, int(crop.bottom - 1));
            float fy = py2 - y0;
            int x0 = int(px2), x1 = std::min(x0 + 1, int(crop.right - 1));
            float fx = px2 - x0;
            for (int c = 0; c < 3; ++c) {
                auto sample = [&](int sampleX, int sampleY) {
                    return float(image.bgra[(size_t(sampleY) * image.width + sampleX) * 4 + c]);
                };
                float pixel = ((sample(x0, y0) * (1 - fx) + sample(x1, y0) * fx) * (1 - fy) +
                               (sample(x0, y1) * (1 - fx) + sample(x1, y1) * fx) * fy) /
                              255.f;
                result[(size_t(c) * height + y) * width + x] =
                    detection ? (pixel - mean[c]) / stddev[c] : pixel * 2 - 1;
            }
        }
    }
    return result;
}
std::vector<float> OrientedTensor(const Image& image, const OrientedBox& obb, int width, int height,
                                  int contentWidth, bool turnLeft = false) {
    std::vector<float> result(size_t(width) * height * 3, 0.f);
    constexpr double kPi = 3.14159265358979323846;
    const float rad = float(double(obb.angleDeg) * kPi / 180.0);
    const float cosA = std::cos(rad), sinA = std::sin(rad);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < contentWidth; ++x) {
            float lx{}, ly{};
            if (turnLeft) {
                lx = obb.width * .5f - (y + .5f) * obb.width / float(height);
                ly = -obb.height * .5f + (x + .5f) * obb.height / float(contentWidth);
            } else {
                lx = -obb.width * .5f + (x + .5f) * obb.width / float(contentWidth);
                ly = -obb.height * .5f + (y + .5f) * obb.height / float(height);
            }
            const float px = std::clamp(obb.centerX + lx * cosA - ly * sinA, 0.f, float(image.width - 1));
            const float py = std::clamp(obb.centerY + lx * sinA + ly * cosA, 0.f, float(image.height - 1));
            const int y0 = int(py), y1 = std::min(y0 + 1, image.height - 1);
            const float fy = py - y0;
            const int x0 = int(px), x1 = std::min(x0 + 1, image.width - 1);
            const float fx = px - x0;
            for (int c = 0; c < 3; ++c) {
                auto sample = [&](int sampleX, int sampleY) {
                    return float(image.bgra[(size_t(sampleY) * image.width + sampleX) * 4 + c]);
                };
                const float pixel = ((sample(x0, y0) * (1 - fx) + sample(x1, y0) * fx) * (1 - fy) +
                                     (sample(x0, y1) * (1 - fx) + sample(x1, y1) * fx) * fy) /
                                    255.f;
                result[(size_t(c) * height + y) * width + x] = pixel * 2 - 1;
            }
        }
    }
    return result;
}
// DB post-processing parameters. The general detector keeps its original bias toward strict
// contours; the manga detector pairs a low binarisation threshold with a soft average score and a
// smaller expansion, which is what keeps thin vertical strokes and kana apart on artwork.
struct DbParams {
    float threshold{.3f};
    float minAverage{.48f};
    float unclip{1.6f};
    size_t minPixels{4};
};
struct DetectedBox {
    RECT rect{};
    OrientedBox obb{};
};
std::vector<DetectedBox> BoxesWithObb(const Ort::Value& output, const Image& image, DbParams params = {}) {
    auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() != 4 || shape[0] != 1 || shape[1] != 1 || shape[2] <= 0 || shape[3] <= 0 ||
        shape[2] * shape[3] > 4096 * 4096)
        throw AppError("ocr", "Invalid detector output");
    int h = int(shape[2]), w = int(shape[3]);
    const float* map = output.GetTensorData<float>();
    std::vector<unsigned char> seen(size_t(w) * h);
    std::vector<int> pending;
    std::vector<RECT> boxes;
    std::vector<RECT> rawBoxes;
    std::vector<OrientedBox> obbs;
    const double sx = double(image.width) / double(w);
    const double sy = double(image.height) / double(h);
    constexpr double kPi = 3.14159265358979323846;
    for (int p = 0; p < w * h; ++p) {
        if (seen[p] || map[p] < params.threshold)
            continue;
        pending.clear();
        pending.push_back(p);
        seen[p] = 1;
        int l = p % w, r = l, t = p / w, b = t;
        double score = 0;
        for (size_t i = 0; i < pending.size(); ++i) {
            int at = pending[i], x = at % w, y = at / w;
            l = std::min(l, x);
            r = std::max(r, x);
            t = std::min(t, y);
            b = std::max(b, y);
            score += map[at];
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || nx >= w || ny < 0 || ny >= h)
                        continue;
                    int next = ny * w + nx;
                    if (!seen[next] && map[next] >= params.threshold) {
                        seen[next] = 1;
                        pending.push_back(next);
                    }
                }
        }
        if (pending.size() < params.minPixels || score / pending.size() < params.minAverage || b - t < 2 || r - l < 2)
            continue;
        if (boxes.size() >= 512)
            throw AppError("ocr", "Too many text regions");
        // DB's shrunken text mask is expanded along both axes for horizontal text.
        float pad = float(pending.size()) * params.unclip / (2 * ((r - l + 1) + (b - t + 1)));
        RECT rect{LONG(std::floor((l - pad) * image.width / w)),
                  LONG(std::floor((t - pad) * image.height / h)),
                  LONG(std::ceil((r + 1 + pad) * image.width / w)),
                  LONG(std::ceil((b + 1 + pad) * image.height / h))};
        rect.left = std::max(0L, rect.left);
        rect.top = std::max(0L, rect.top);
        rect.right = std::min<LONG>(image.width, rect.right);
        rect.bottom = std::min<LONG>(image.height, rect.bottom);
        if (Width(rect) < 3 || Height(rect) < 5)
            continue;
        RECT raw{std::clamp<LONG>(LONG(std::floor(double(l) * image.width / w)), rect.left, rect.right),
                 std::clamp<LONG>(LONG(std::floor(double(t) * image.height / h)), rect.top, rect.bottom),
                 std::clamp<LONG>(LONG(std::ceil(double(r + 1) * image.width / w)), rect.left, rect.right),
                 std::clamp<LONG>(LONG(std::ceil(double(b + 1) * image.height / h)), rect.top, rect.bottom)};
        OrientedBox obb;
        if (pending.size() >= 6) {
            double mx = 0.0, my = 0.0;
            for (int idx : pending) {
                mx += ((idx % w) + 0.5) * sx;
                my += ((idx / w) + 0.5) * sy;
            }
            mx /= double(pending.size());
            my /= double(pending.size());
            double cxx = 0.0, cyy = 0.0, cxy = 0.0;
            for (int idx : pending) {
                const double dx = ((idx % w) + 0.5) * sx - mx;
                const double dy = ((idx / w) + 0.5) * sy - my;
                cxx += dx * dx;
                cyy += dy * dy;
                cxy += dx * dy;
            }
            const double theta = 0.5 * std::atan2(2.0 * cxy, cxx - cyy);
            const double ux = std::cos(theta), uy = std::sin(theta);
            const double vx = -uy, vy = ux;
            double minU = 1e9, maxU = -1e9, minV = 1e9, maxV = -1e9;
            const double halfCellU = 0.5 * (std::abs(ux) * sx + std::abs(uy) * sy);
            const double halfCellV = 0.5 * (std::abs(vx) * sx + std::abs(vy) * sy);
            for (int idx : pending) {
                const double dx = ((idx % w) + 0.5) * sx - mx;
                const double dy = ((idx / w) + 0.5) * sy - my;
                const double pu = dx * ux + dy * uy;
                const double pv = dx * vx + dy * vy;
                minU = std::min(minU, pu - halfCellU);
                maxU = std::max(maxU, pu + halfCellU);
                minV = std::min(minV, pv - halfCellV);
                maxV = std::max(maxV, pv + halfCellV);
            }
            const double padImg = double(pad) * 0.5 * (sx + sy);
            minU -= padImg;
            maxU += padImg;
            minV -= padImg;
            maxV += padImg;
            const double lenU = maxU - minU, lenV = maxV - minV;
            const double midU = 0.5 * (minU + maxU), midV = 0.5 * (minV + maxV);
            const double cx = mx + midU * ux + midV * vx;
            const double cy = my + midU * uy + midV * vy;
            const double deg = theta * 180.0 / kPi;
            const double rectArea = double(Width(rect)) * double(Height(rect));
            if (lenU >= lenV * 1.45 && lenU * lenV < rectArea * 0.90) {
                if (std::abs(deg) <= 55.0) {
                    if (std::abs(deg) >= 6.0)
                        obb = {float(cx), float(cy), float(lenU), float(lenV), float(deg)};
                } else {
                    const double beta = deg > 0.0 ? deg - 90.0 : deg + 90.0;
                    if (std::abs(beta) >= 6.0)
                        obb = {float(cx), float(cy), float(lenV), float(lenU), float(beta)};
                }
            }
        }
        // A tall box is either a vertical column or a rotated line. Both reach recognition; the
        // recognizer decides which reading the crop supports.
        boxes.push_back(rect);
        rawBoxes.push_back(raw);
        obbs.push_back(obb);
    }
    // Recognition expansion can touch adjacent baselines or neighbouring UI pills on the same row.
    // Split the shared margin at their midpoint so a crop cannot import strokes from its neighbour.
    const auto expanded = boxes;
    for (size_t i = 0; i < boxes.size(); ++i)
        for (size_t j = 0; j < boxes.size(); ++j) {
            if (i == j)
                continue;
            auto a = expanded[i], b = expanded[j];
            int overlapX = std::min(a.right, b.right) - std::max(a.left, b.left);
            int overlapY = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
            int ca = (a.top + a.bottom) / 2, cb = (b.top + b.bottom) / 2, deltaY = cb - ca;
            if (overlapX >= std::min(Width(a), Width(b)) * .5f &&
                std::min(Height(a), Height(b)) >= std::max(Height(a), Height(b)) * .45f &&
                std::abs(deltaY) >= std::min(Height(a), Height(b)) * .5f) {
                if (a.bottom > b.top && b.bottom > a.top) {
                    int split = (ca + cb) / 2;
                    if (deltaY > 0)
                        boxes[i].bottom = std::min<LONG>(boxes[i].bottom, split);
                    else
                        boxes[i].top = std::max<LONG>(boxes[i].top, split);
                }
            }
            // Same-row horizontal neighbours whose unexpanded kernels were disjoint must not cross after
            // unclip padding.
            if (overlapY >= std::min(Height(a), Height(b)) * .45f &&
                std::abs(deltaY) <= std::min(Height(a), Height(b)) * .55f) {
                if (rawBoxes[i].right <= rawBoxes[j].left && a.right > b.left - 1) {
                    const LONG splitX = (rawBoxes[i].right + rawBoxes[j].left) / 2;
                    if (splitX > boxes[i].left + 3)
                        boxes[i].right = std::min<LONG>(boxes[i].right, splitX);
                } else if (rawBoxes[j].right <= rawBoxes[i].left && b.right > a.left - 1) {
                    const LONG splitX = (rawBoxes[j].right + rawBoxes[i].left + 1) / 2;
                    if (splitX < boxes[i].right - 3)
                        boxes[i].left = std::max<LONG>(boxes[i].left, splitX);
                }
            }
        }
    // Peel enclosing pill/button frames, split across segmented-control boundaries or curved pill
    // borders, and detach geometric colour swatches or micro-chevrons before recognition.
    auto key12 = [](const unsigned char* p) {
        return (int(p[2] >> 4) << 8) | (int(p[1] >> 4) << 4) | int(p[0] >> 4);
    };
    auto modeRgb = [&](RECT area, int& outR, int& outG, int& outB) -> bool {
        area.left = std::clamp<LONG>(area.left, 0, image.width);
        area.top = std::clamp<LONG>(area.top, 0, image.height);
        area.right = std::clamp<LONG>(area.right, area.left, image.width);
        area.bottom = std::clamp<LONG>(area.bottom, area.top, image.height);
        if (Width(area) <= 0 || Height(area) <= 0)
            return false;
        std::array<size_t, 4096> hist{};
        size_t total = 0;
        for (int y = area.top; y < area.bottom; ++y)
            for (int x = area.left; x < area.right; ++x) {
                ++hist[key12(&image.bgra[(size_t(y) * image.width + x) * 4])];
                ++total;
            }
        if (!total)
            return false;
        const int m = int(std::max_element(hist.begin(), hist.end()) - hist.begin());
        const int mr = ((m >> 8) & 15) * 16 + 8;
        const int mg = ((m >> 4) & 15) * 16 + 8;
        const int mb = (m & 15) * 16 + 8;
        long long sr = 0, sg = 0, sb = 0, cnt = 0;
        for (int y = area.top; y < area.bottom; ++y)
            for (int x = area.left; x < area.right; ++x) {
                const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                if (std::abs(int(p[2]) - mr) + std::abs(int(p[1]) - mg) + std::abs(int(p[0]) - mb) < 42) {
                    sr += p[2];
                    sg += p[1];
                    sb += p[0];
                    ++cnt;
                }
            }
        if (cnt > 0) {
            outR = int(sr / cnt);
            outG = int(sg / cnt);
            outB = int(sb / cnt);
        } else {
            outR = mr;
            outG = mg;
            outB = mb;
        }
        return true;
    };
    auto peelCapsuleFrame = [&](RECT rect) -> RECT {
        const int bw = Width(rect), bh = Height(rect);
        if (bw < 22 || bh < 12 || bh > 84 || bw <= bh ||
            image.bgra.size() != size_t(image.width) * image.height * 4)
            return rect;
        int br = 255, bg = 255, bb = 255;
        if (!modeRgb(rect, br, bg, bb))
            return rect;
        const int insetX = std::clamp(bh / 3, 3, std::max(3, bw / 4));
        const int cx0 = rect.left + insetX, cx1 = rect.right - insetX;
        if (cx1 - cx0 < 8)
            return rect;
        const int cSpan = cx1 - cx0;
        // Measure text-core row ink in a deeper horizontal inset so rounded pill corner arcs
        // never bridge the whitespace between the top/bottom rails and the actual text band.
        const int coreInsetX = std::clamp(int(std::lround(bh * .58f)), insetX, std::max(insetX, bw / 3));
        const int coreX0 = rect.left + coreInsetX, coreX1 = rect.right - coreInsetX;
        std::vector<int> rowInk(size_t(bh), 0);
        std::vector<unsigned char> rowRail(size_t(bh), 0);
        for (int y = 0; y < bh; ++y) {
            const int sy = rect.top + y;
            int ink = 0, smoothRun = 0, maxSmoothRun = 0;
            for (int sx = cx0; sx < cx1; ++sx) {
                const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                const int d = std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb);
                if (d >= 44 && sx >= coreX0 && sx < coreX1)
                    ++ink;
                // A horizontal capsule border is a horizontally smooth non-background stroke near
                // the top/bottom rim; text rows oscillate rapidly between glyph stems and counters.
                bool smoothBorder = d >= 18;
                if (smoothBorder && sx > cx0) {
                    const auto* pl = &image.bgra[(size_t(sy) * image.width + sx - 1) * 4];
                    const int dh = std::abs(int(p[2]) - int(pl[2])) + std::abs(int(p[1]) - int(pl[1])) +
                                   std::abs(int(p[0]) - int(pl[0]));
                    if (dh > 24)
                        smoothBorder = false;
                }
                if (smoothBorder) {
                    ++smoothRun;
                    maxSmoothRun = std::max(maxSmoothRun, smoothRun);
                } else {
                    smoothRun = 0;
                }
            }
            const bool inRailZone = (y <= std::max(2, bh / 3)) || (y >= bh - 1 - std::max(2, bh / 3));
            if (inRailZone && maxSmoothRun >= std::max(10, int(cSpan * .45f)))
                rowRail[size_t(y)] = 1;
            rowInk[size_t(y)] = rowRail[size_t(y)] ? 0 : ink;
        }
        const int midY = bh / 2;
        int bestRunTop = -1, bestRunBottom = -1, bestDist = bh;
        for (int y = 0; y < bh;) {
            if (rowInk[size_t(y)] < 2) {
                ++y;
                continue;
            }
            const int r0 = y;
            int gap = 0;
            while (y < bh) {
                if (rowInk[size_t(y)] >= 2) {
                    gap = 0;
                    ++y;
                } else if (gap == 0 && y + 1 < bh && rowInk[size_t(y + 1)] >= 2) {
                    gap = 1;
                    ++y;
                } else {
                    break;
                }
            }
            const int r1 = (y > r0 && rowInk[size_t(y - 1)] < 2) ? y - 1 : y;
            if (r1 - r0 >= 4) {
                const int dist =
                    (midY >= r0 && midY <= r1) ? 0 : std::min(std::abs(midY - r0), std::abs(midY - r1));
                if (dist < bestDist || (dist == bestDist && (r1 - r0) > (bestRunBottom - bestRunTop))) {
                    bestDist = dist;
                    bestRunTop = r0;
                    bestRunBottom = r1;
                }
            }
        }
        if (bestRunTop < 0)
            return rect;
        const int coreTop = bestRunTop;
        const int coreBottom = bestRunBottom;
        LONG newTop = rect.top, newBottom = rect.bottom;
        int topRailBottom = -1;
        for (int y = 0; y < coreTop; ++y)
            if (rowRail[size_t(y)])
                topRailBottom = y;
        if (topRailBottom >= 0 && coreTop - topRailBottom - 1 >= 1)
            newTop = rect.top + std::min(coreTop, topRailBottom + 2);
        int bottomRailTop = -1;
        for (int y = bh - 1; y >= coreBottom; --y)
            if (rowRail[size_t(y)])
                bottomRailTop = y;
        if (bottomRailTop >= 0 && bottomRailTop - coreBottom >= 1)
            newBottom = rect.top + std::max(coreBottom, bottomRailTop - 1);
        // Only peel curved/vertical side rims when an enclosing capsule rail is present and the side
        // stroke extends into BOTH upper and lower corner zones (never peeling trailing '!' or 'y').
        LONG newLeft = rect.left, newRight = rect.right;
        if (topRailBottom >= 0 || bottomRailTop >= 0) {
            const int absCoreTop = rect.top + coreTop;
            const int absCoreBottom = rect.top + coreBottom;
            const int cornerTop0 =
                topRailBottom >= 0 ? rect.top + topRailBottom + 1 : std::max(int(rect.top), absCoreTop - 4);
            const int cornerBot1 =
                bottomRailTop >= 0 ? rect.top + bottomRailTop : std::min(int(rect.bottom), absCoreBottom + 4);
            std::vector<int> bandInk(size_t(bw), 0);
            std::vector<int> outerTop(size_t(bw), 0), outerBot(size_t(bw), 0);
            for (int x = 0; x < bw; ++x) {
                const int sx = rect.left + x;
                for (int sy = absCoreTop; sy < absCoreBottom; ++sy) {
                    const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                    if (std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb) >= 36)
                        ++bandInk[size_t(x)];
                }
                for (int sy = cornerTop0; sy < absCoreTop; ++sy) {
                    const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                    if (std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb) >= 28)
                        ++outerTop[size_t(x)];
                }
                for (int sy = absCoreBottom; sy < cornerBot1; ++sy) {
                    const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                    if (std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb) >= 28)
                        ++outerBot[size_t(x)];
                }
            }
            const int maxRimZone = std::min(12, bw / 4);
            {
                int x = 0;
                while (x < maxRimZone && bandInk[size_t(x)] == 0)
                    ++x;
                const int rimStart = x;
                while (x < maxRimZone && bandInk[size_t(x)] > 0)
                    ++x;
                const int rimEnd = x;
                int topHits = 0, botHits = 0;
                const int cornerEnd = std::min(bw, rimEnd + 4);
                for (int cx = rimStart; cx < cornerEnd; ++cx) {
                    topHits += outerTop[size_t(cx)];
                    botHits += outerBot[size_t(cx)];
                }
                int cleanCols = 0;
                while (x < bw && bandInk[size_t(x)] == 0) {
                    ++cleanCols;
                    ++x;
                }
                if (rimEnd > rimStart && rimEnd - rimStart <= 5 && cleanCols >= 2 && x < bw - 6 &&
                    topHits >= 1 && botHits >= 1) {
                    newLeft = rect.left + rimEnd + std::min(3, cleanCols - 1);
                }
            }
            {
                int x = bw - 1;
                while (x >= bw - maxRimZone && bandInk[size_t(x)] == 0)
                    --x;
                const int rimEnd = x;
                while (x >= bw - maxRimZone && bandInk[size_t(x)] > 0)
                    --x;
                const int rimStart = x;
                int topHits = 0, botHits = 0;
                const int cornerStart = std::max(0, rimStart - 3);
                for (int cx = cornerStart; cx <= rimEnd; ++cx) {
                    topHits += outerTop[size_t(cx)];
                    botHits += outerBot[size_t(cx)];
                }
                int cleanCols = 0;
                while (x >= 0 && bandInk[size_t(x)] == 0) {
                    ++cleanCols;
                    --x;
                }
                if (rimEnd > rimStart && rimEnd - rimStart <= 5 && cleanCols >= 2 && x >= 6 &&
                    topHits >= 1 && botHits >= 1) {
                    newRight = rect.left + rimStart + 1 - std::min(3, cleanCols - 1);
                }
            }
        }
        if (newRight - newLeft >= 8 && newBottom - newTop >= 6)
            return {newLeft, newTop, newRight, newBottom};
        return rect;
    };
    auto trimGeometricAffixes = [&](RECT rect) -> RECT {
        const LONG origLeft = rect.left;
        const int bw = Width(rect), bh = Height(rect);
        if (bw < 24 || bh < 10 || bh > 72 || bw <= bh ||
            image.bgra.size() != size_t(image.width) * image.height * 4)
            return rect;
        int br = 255, bg = 255, bb = 255;
        if (!modeRgb(rect, br, bg, bb))
            return rect;
        std::vector<int> colInk(size_t(bw), 0);
        std::vector<int> colTop(size_t(bw), bh), colBot(size_t(bw), 0);
        for (int x = 0; x < bw; ++x) {
            const int sx = rect.left + x;
            for (int y = 0; y < bh; ++y) {
                const auto* p = &image.bgra[(size_t(rect.top + y) * image.width + sx) * 4];
                if (std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb) >= 48) {
                    ++colInk[size_t(x)];
                    colTop[size_t(x)] = std::min(colTop[size_t(x)], y);
                    colBot[size_t(x)] = std::max(colBot[size_t(x)], y + 1);
                }
            }
        }
        struct Span {
            int x0{}, x1{}, top{}, bot{}, areaInk{};
        };
        std::vector<Span> spans;
        for (int x = 0; x < bw;) {
            if (colInk[size_t(x)] == 0) {
                ++x;
                continue;
            }
            Span s{x, x, bh, 0, 0};
            int gap = 0;
            while (x < bw) {
                if (colInk[size_t(x)] > 0) {
                    s.top = std::min(s.top, colTop[size_t(x)]);
                    s.bot = std::max(s.bot, colBot[size_t(x)]);
                    s.areaInk += colInk[size_t(x)];
                    gap = 0;
                    ++x;
                } else if (gap == 0 && x + 1 < bw && colInk[size_t(x + 1)] > 0) {
                    gap = 1;
                    ++x;
                } else {
                    break;
                }
            }
            s.x1 = (x > s.x0 && colInk[size_t(x - 1)] == 0) ? x - 1 : x;
            if (s.x1 > s.x0 && s.bot > s.top)
                spans.push_back(s);
        }
        if (spans.size() < 2)
            return rect;
        // Detach leading solid colour swatches (e.g. [black-square] #0A0A0C).
        if (spans.size() >= 2) {
            const auto& first = spans.front();
            const int sw = first.x1 - first.x0, sh = first.bot - first.top;
            const int gap = spans[1].x0 - first.x1;
            const int restW = spans.back().x1 - spans[1].x0;
            const float fill = float(first.areaInk) / float(std::max(1, sw * sh));
            if (sw >= 8 && sw <= 28 && sh >= 8 && sh <= 28 && float(sw) >= sh * .65f &&
                float(sw) <= sh * 1.55f && gap >= 4 && restW >= 14 && fill >= .72f) {
                rect.left = origLeft + std::max(first.x1 + 2, spans[1].x0 - 2);
                spans.erase(spans.begin());
            }
        }
        // Detach trailing solid swatches or vertically-centred micro-chevrons / arrows (e.g. dropdown v).
        if (spans.size() >= 2) {
            const auto& last = spans.back();
            const auto& prev = spans[spans.size() - 2];
            const int lw = last.x1 - last.x0, lh = last.bot - last.top;
            const int gap = last.x0 - prev.x1;
            const int leadW = prev.x1 - spans.front().x0;
            int mainTop = bh, mainBot = 0;
            for (size_t k = 0; k + 1 < spans.size(); ++k) {
                mainTop = std::min(mainTop, spans[k].top);
                mainBot = std::max(mainBot, spans[k].bot);
            }
            const int mainH = std::max(6, mainBot - mainTop);
            const float fill = float(last.areaInk) / float(std::max(1, lw * lh));
            const bool solidSwatch = lw >= 8 && lw <= 28 && lh >= 8 && lh <= 28 && float(lw) >= lh * .65f &&
                                     float(lw) <= lh * 1.55f && gap >= 4 && fill >= .72f;
            const bool microChevron = lw >= 3 && lw <= std::max(12, int(mainH * .95f)) && lh >= 2 &&
                                      lh <= int(mainH * .58f) &&
                                      gap >= std::max(5, int(std::lround(mainH * .45f))) &&
                                      last.top >= mainTop + 2 && last.bot <= mainBot - 1;
            if (leadW >= 12 && (solidSwatch || microChevron)) {
                const LONG newRight = origLeft + std::min(last.x0 - 2, prev.x1 + 3);
                if (newRight - rect.left >= 8)
                    rect.right = std::min(rect.right, newRight);
            }
        }
        return rect;
    };
    std::vector<DetectedBox> splitBoxes;
    splitBoxes.reserve(boxes.size());
    for (size_t idx = 0; idx < boxes.size(); ++idx) {
        const RECT rawRect = boxes[idx];
        const OrientedBox rawObb = obbs[idx];
        // Tilted lines must not be split or capsule-peeled by axis-aligned row/column scans.
        if (rawObb.valid()) {
            splitBoxes.push_back({rawRect, rawObb});
            continue;
        }
        const RECT rect = peelCapsuleFrame(rawRect);
        const int bw = Width(rect), bh = Height(rect);
        if (bh < 10 || bh > 80 || bw <= bh * 3 / 2 ||
            image.bgra.size() != size_t(image.width) * image.height * 4) {
            splitBoxes.push_back({trimGeometricAffixes(rect), {}});
            continue;
        }
        // Per-tile local backdrop keeps segmented controls (white active pill next to light-grey track)
        // from treating one half's surface as ink or masking the divider between them.
        constexpr int kTileW = 36;
        const int numTiles = std::max(1, (bw + kTileW - 1) / kTileW);
        struct Rgb {
            int r{255}, g{255}, b{255};
        };
        std::vector<Rgb> tileBg;
        tileBg.resize(size_t(numTiles));
        int globalR = 255, globalG = 255, globalB = 255;
        modeRgb(rect, globalR, globalG, globalB);
        for (int t = 0; t < numTiles; ++t) {
            const LONG tx0 = rect.left + t * kTileW;
            const LONG tx1 = std::min<LONG>(rect.right, tx0 + kTileW);
            if (!modeRgb({tx0, rect.top, tx1, rect.bottom}, tileBg[size_t(t)].r, tileBg[size_t(t)].g,
                         tileBg[size_t(t)].b))
                tileBg[size_t(t)] = {globalR, globalG, globalB};
        }
        const int vInset = bh >= 24 ? std::max(2, bh / 5) : std::max(1, bh / 8);
        const int y0 = std::min<int>(rect.bottom - 1, rect.top + vInset);
        const int y1 = std::max<int>(y0 + 1, rect.bottom - vInset);
        std::vector<int> colInk(size_t(bw), 0);
        std::vector<int> colTopY(size_t(bw), y1), colBotY(size_t(bw), y0);
        std::vector<unsigned char> colBorder(size_t(bw), 0);
        int inkMinY = y1, inkMaxY = y0;
        for (int x = 0; x < bw; ++x) {
            const int sx = rect.left + x;
            const auto& tb = tileBg[size_t(std::min(numTiles - 1, x / kTileW))];
            int ink = 0, minDiff = 999, maxDiff = 0;
            int colFirstY = y1, colLastY = y0;
            for (int sy = y0; sy < y1; ++sy) {
                const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                const int d = std::abs(int(p[2]) - tb.r) + std::abs(int(p[1]) - tb.g) + std::abs(int(p[0]) - tb.b);
                if (d >= 52) {
                    ++ink;
                    colFirstY = std::min(colFirstY, sy);
                    colLastY = std::max(colLastY, sy + 1);
                }
                minDiff = std::min(minDiff, d);
                maxDiff = std::max(maxDiff, d);
            }
            const bool uniformVerticalLine = (y1 - y0 >= 8) && (maxDiff - minDiff <= 24) && maxDiff < 160;
            if (uniformVerticalLine && minDiff >= 18)
                colBorder[size_t(x)] = 1;
            colInk[size_t(x)] = uniformVerticalLine ? 0 : ink;
            colTopY[size_t(x)] = colFirstY;
            colBotY[size_t(x)] = colLastY;
            if (colInk[size_t(x)] >= 2 && colInk[size_t(x)] < (y1 - y0) * 9 / 10) {
                inkMinY = std::min(inkMinY, colFirstY);
                inkMaxY = std::max(inkMaxY, colLastY);
            }
        }
        auto edgeHasInk = [&](int sx, int sy, const Rgb& ref) {
            if (sy < 0 || sy >= image.height)
                return false;
            const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
            const int d = std::abs(int(p[2]) - ref.r) + std::abs(int(p[1]) - ref.g) + std::abs(int(p[0]) - ref.b);
            return d >= 40;
        };
        const int minBorderRows = std::max(6, ((y1 - y0) * 4) / 5);
        for (int x = 3; x + 4 < bw; ++x) {
            if (colInk[size_t(x)] > 0 && colInk[size_t(x - 1)] == 0 && colInk[size_t(x - 2)] == 0 &&
                colInk[size_t(x - 3)] == 0) {
                int run = 1;
                while (x + run < bw && colInk[size_t(x + run)] > 0 && run <= 3)
                    ++run;
                if (run <= 2 && x + run + 2 < bw && colInk[size_t(x + run)] == 0 &&
                    colInk[size_t(x + run + 1)] == 0 && colInk[size_t(x + run + 2)] == 0) {
                    bool fullHeightStroke = true;
                    for (int k = 0; k < run; ++k) {
                        const int sx = rect.left + x + k;
                        const auto& tb = tileBg[size_t(std::min(numTiles - 1, (x + k) / kTileW))];
                        if (colInk[size_t(x + k)] < minBorderRows ||
                            !edgeHasInk(sx, rect.top, tb) || !edgeHasInk(sx, rect.bottom - 1, tb)) {
                            fullHeightStroke = false;
                            break;
                        }
                    }
                    if (fullHeightStroke) {
                        for (int k = 0; k < run; ++k) {
                            colInk[size_t(x + k)] = 0;
                            colBorder[size_t(x + k)] = 1;
                        }
                    }
                }
            }
        }
        const int inkH = (inkMaxY > inkMinY) ? std::max(8, inkMaxY - inkMinY) : std::max(10, (bh * 3) / 5);
        // Inline dropdown chevrons (e.g. "Auto v  Optimized for: Balance v  [icon]") separate adjacent
        // toolbar controls: a tiny vertically-centred triangle (<= 48% of line ink height) with at
        // least 5 px clear on both sides and a wide inter-control gap (>= max(12, 1.1*inkH)) on one
        // side marks a control boundary without ever matching single-letter words in normal prose.
        for (int x = 5; x < bw - 5;) {
            if (colInk[size_t(x)] == 0) {
                ++x;
                continue;
            }
            const int c0 = x;
            int cTop = y1, cBot = y0;
            while (x < bw && colInk[size_t(x)] > 0) {
                cTop = std::min(cTop, colTopY[size_t(x)]);
                cBot = std::max(cBot, colBotY[size_t(x)]);
                ++x;
            }
            const int c1 = x;
            const int cw = c1 - c0, ch = cBot - cTop;
            if (cw >= 4 && cw <= std::max(10, int(inkH * .90f)) && ch >= 2 &&
                ch <= std::max(4, int(inkH * .48f)) && cTop >= inkMinY + 2 && cBot <= inkMaxY - 2) {
                int leftClean = 0;
                for (int lx = c0 - 1; lx >= 0 && colInk[size_t(lx)] == 0; --lx)
                    ++leftClean;
                int rightClean = 0;
                for (int rx = c1; rx < bw && colInk[size_t(rx)] == 0; ++rx)
                    ++rightClean;
                if (leftClean >= 5 && rightClean >= 5 &&
                    std::max(leftClean, rightClean) >= std::max(12, int(std::lround(inkH * 1.1f)))) {
                    for (int k = c0; k < c1; ++k) {
                        colInk[size_t(k)] = 0;
                        colBorder[size_t(k)] = 1;
                    }
                }
            }
        }
        const int plainSplitGap =
            std::max(18, int(std::lround(std::min(float(bh) * 1.12f, float(inkH) * 1.75f))));
        const int borderedSplitGap = std::max(6, int(std::lround(float(inkH) * .48f)));
        const int surfaceStepGap = std::max(8, int(std::lround(float(inkH) * .62f)));

        // Detect curved pill rims or background surface steps inside an ink-free horizontal gap.
        auto analyzeGapBoundary = [&](int segStart, int gapStart, int gapEnd, int& cutLeftEnd,
                                      int& cutRightStart) -> bool {
            cutLeftEnd = gapStart;
            cutRightStart = gapEnd;
            const int gapW = gapEnd - gapStart;
            bool hasStraightBorder = false;
            int bMin = gapEnd, bMax = gapStart;
            for (int gx = gapStart; gx < gapEnd; ++gx)
                if (colBorder[size_t(gx)]) {
                    hasStraightBorder = true;
                    bMin = std::min(bMin, gx);
                    bMax = std::max(bMax, gx + 1);
                }
            if (hasStraightBorder && gapW >= borderedSplitGap) {
                cutLeftEnd = std::max(segStart + 4, bMin - 1);
                cutRightStart = std::min(bw - 4, bMax + 1);
                return true;
            }
            if (gapW >= std::max(borderedSplitGap, 8) && y1 - y0 >= 8) {
                // Stay strictly inside the whitespace gap (>= 2 px clear of both adjacent words) so
                // vertical glyph stems ("m", "l", "p", "h") and their JPEG halos never read as a border.
                const int gx0 = gapStart + 2;
                const int gx1 = gapEnd - 3;
                if (gx1 >= gx0) {
                    auto maxGradInRow = [&](int sy, int& outX) {
                        int bestX = -1, bestGrad = 0;
                        for (int gx = gx0; gx <= gx1; ++gx) {
                            const int sx = rect.left + gx;
                            if (sx <= 0 || sx + 1 >= image.width)
                                continue;
                            const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                            const auto* pl = &image.bgra[(size_t(sy) * image.width + sx - 1) * 4];
                            const auto* pr = &image.bgra[(size_t(sy) * image.width + sx + 1) * 4];
                            const int gL = std::abs(int(p[2]) - int(pl[2])) +
                                           std::abs(int(p[1]) - int(pl[1])) +
                                           std::abs(int(p[0]) - int(pl[0]));
                            const int gR = std::abs(int(p[2]) - int(pr[2])) +
                                           std::abs(int(p[1]) - int(pr[1])) +
                                           std::abs(int(p[0]) - int(pr[0]));
                            const int g = std::max(gL, gR);
                            if (g > bestGrad) {
                                bestGrad = g;
                                bestX = gx;
                            }
                        }
                        outX = bestX;
                        return bestGrad;
                    };
                    int prevX = -1, hitRows = 0, continuousRows = 0;
                    int curveMinX = gx1, curveMaxX = gx0;
                    for (int sy = y0; sy < y1; ++sy) {
                        int bestX = -1;
                        const int bestGrad = maxGradInRow(sy, bestX);
                        if (bestGrad >= 18 && bestX >= 0) {
                            ++hitRows;
                            if (prevX < 0 || std::abs(bestX - prevX) <= 2)
                                ++continuousRows;
                            prevX = bestX;
                            curveMinX = std::min(curveMinX, bestX);
                            curveMaxX = std::max(curveMaxX, bestX);
                        } else {
                            prevX = -1;
                        }
                    }
                    int extHits = 0;
                    for (int sy = std::max(0, y0 - 4); sy < y0; ++sy) {
                        int dummy = -1;
                        if (maxGradInRow(sy, dummy) >= 16)
                            ++extHits;
                    }
                    for (int sy = y1; sy < std::min(image.height, y1 + 4); ++sy) {
                        int dummy = -1;
                        if (maxGradInRow(sy, dummy) >= 16)
                            ++extHits;
                    }
                    const int spanRows = y1 - y0;
                    if (extHits >= 2 && hitRows * 10 >= spanRows * 6 &&
                        continuousRows * 10 >= spanRows * 5 &&
                        curveMaxX - curveMinX <= std::max(8, bh / 2)) {
                        cutLeftEnd = std::max(segStart + 4, std::min(gapStart + 2, curveMinX - 1));
                        cutRightStart = std::max(cutLeftEnd + 2, std::max(gapEnd - 2, curveMaxX + 2));
                        return true;
                    }
                }
            }
            if (gapW >= surfaceStepGap) {
                int lR = 0, lG = 0, lB = 0, rR = 0, rG = 0, rB = 0;
                const LONG lx0 = rect.left + std::max(0, gapStart - std::min(36, gapStart - segStart));
                const LONG lx1 = rect.left + gapStart;
                const LONG rx0 = rect.left + gapEnd;
                const LONG rx1 = rect.left + std::min(bw, gapEnd + 36);
                if (lx1 - lx0 >= 6 && rx1 - rx0 >= 6 &&
                    modeRgb({lx0, rect.top, lx1, rect.bottom}, lR, lG, lB) &&
                    modeRgb({rx0, rect.top, rx1, rect.bottom}, rR, rG, rB)) {
                    const int stepDiff = std::abs(lR - rR) + std::abs(lG - rG) + std::abs(lB - rB);
                    if (stepDiff >= 18) {
                        int firstNonLeft = gapEnd, lastNonRight = gapStart;
                        for (int gx = gapStart; gx < gapEnd; ++gx) {
                            int cR = 0, cG = 0, cB = 0;
                            if (modeRgb({rect.left + gx, rect.top + 2, rect.left + gx + 1, rect.bottom - 2},
                                        cR, cG, cB)) {
                                const int dL = std::abs(cR - lR) + std::abs(cG - lG) + std::abs(cB - lB);
                                const int dR = std::abs(cR - rR) + std::abs(cG - rG) + std::abs(cB - rB);
                                if (dL >= 12)
                                    firstNonLeft = std::min(firstNonLeft, gx);
                                if (dR >= 12)
                                    lastNonRight = std::max(lastNonRight, gx + 1);
                            }
                        }
                        cutLeftEnd = std::max(segStart + 4, std::min(gapEnd - 2, firstNonLeft - 1));
                        cutRightStart = std::max(cutLeftEnd + 2, std::max(gapStart + 2, lastNonRight + 1));
                        return true;
                    }
                }
            }
            if (gapW >= plainSplitGap) {
                const int mid = (gapStart + gapEnd) / 2;
                const int padX = std::max(2, inkH / 5);
                cutLeftEnd = std::min(mid, gapStart + padX);
                cutRightStart = std::max(mid, gapEnd - padX);
                return true;
            }
            return false;
        };

        struct SplitSpan {
            int left{}, right{};
        };
        std::vector<SplitSpan> segments;
        int segStart = -1, gapStart = -1, pendingLeftBound = 0;
        for (int x = 0; x < bw; ++x) {
            if (colInk[size_t(x)] > 0) {
                if (segStart < 0) {
                    segStart = x;
                    pendingLeftBound = std::max(0, x - std::max(2, inkH / 5));
                } else if (gapStart >= 0) {
                    int cutLeftEnd = gapStart, cutRightStart = x;
                    if (gapStart - segStart >= 4 && segments.size() + 1 < 8 &&
                        analyzeGapBoundary(segStart, gapStart, x, cutLeftEnd, cutRightStart)) {
                        segments.push_back({pendingLeftBound, cutLeftEnd});
                        segStart = x;
                        pendingLeftBound = std::min(x, cutRightStart);
                    }
                }
                gapStart = -1;
            } else if (segStart >= 0 && gapStart < 0) {
                gapStart = x;
            }
        }
        if (segStart >= 0) {
            const int segEnd = gapStart >= 0 ? gapStart : bw;
            if (segEnd - segStart >= 4) {
                const int padX = std::max(2, inkH / 5);
                segments.push_back({pendingLeftBound, std::min(bw, segEnd + padX)});
            }
        }
        if (segments.size() <= 1) {
            splitBoxes.push_back({trimGeometricAffixes(rect), {}});
        } else {
            for (const auto& seg : segments) {
                const LONG subLeft = rect.left + seg.left;
                const LONG subRight = rect.left + seg.right;
                if (subRight - subLeft >= 4 && splitBoxes.size() < 512)
                    splitBoxes.push_back({trimGeometricAffixes({subLeft, rect.top, subRight, rect.bottom}), {}});
            }
        }
    }
    std::sort(splitBoxes.begin(), splitBoxes.end(),
              [](const DetectedBox& a, const DetectedBox& b) {
                  return a.rect.top == b.rect.top ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
              });
    return splitBoxes;
}
std::vector<RECT> Boxes(const Ort::Value& output, const Image& image, DbParams params = {}) {
    auto detected = BoxesWithObb(output, image, params);
    std::vector<RECT> boxes;
    boxes.reserve(detected.size());
    for (const auto& d : detected)
        boxes.push_back(d.rect);
    return boxes;
}
std::pair<std::string, float> Decode(const Ort::Value& output, const std::vector<std::string>& alphabet) {
    auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() != 3 || shape[0] != 1 || shape[1] < 1 || shape[2] != int64_t(alphabet.size()))
        throw AppError("ocr", "Recognition dictionary mismatch");
    const float* data = output.GetTensorData<float>();
    std::string text;
    int64_t previous = -1;
    double confidence = 0;
    size_t count = 0;
    for (int64_t t = 0; t < shape[1]; ++t) {
        auto row = data + t * shape[2];
        auto best = std::max_element(row, row + shape[2]);
        int64_t index = best - row;
        if (index && index != previous) {
            text += alphabet[size_t(index)];
            confidence += *best;
            ++count;
        }
        previous = index;
    }
    return {text, count ? float(confidence / count) : 0.f};
}
void AppendUtf8(std::string& out, char32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}
char32_t MirrorBidiBracket(char32_t code) {
    switch (code) {
    case U'(': return U')';
    case U')': return U'(';
    case U'[': return U']';
    case U']': return U'[';
    case U'{': return U'}';
    case U'}': return U'{';
    case U'<': return U'>';
    case U'>': return U'<';
    case 0x00AB: return 0x00BB;
    case 0x00BB: return 0x00AB;
    default: return code;
    }
}
bool IsStrongLtrCodePoint(char32_t code) {
    return (code >= U'A' && code <= U'Z') || (code >= U'a' && code <= U'z') ||
           (code >= 0x00C0 && code <= 0x024F) || (code >= 0x0370 && code <= 0x052F) ||
           (code >= 0x0900 && code <= 0x0EFF) || (code >= 0x1E00 && code <= 0x1EFF) ||
           (code >= 0x2E80 && code <= 0x9FFF) || (code >= 0xAC00 && code <= 0xD7AF);
}
bool IsNumberCodePoint(char32_t code) {
    return (code >= U'0' && code <= U'9') || (code >= 0x0660 && code <= 0x0669) ||
           (code >= 0x06F0 && code <= 0x06F9);
}
bool HasGreekScript(std::string_view utf8) {
    for (size_t i = 0; i < utf8.size();) {
        const char32_t code = NextCodePoint(utf8, i);
        if ((code >= 0x0370 && code <= 0x03FF) || (code >= 0x1F00 && code <= 0x1FFF))
            return true;
    }
    return false;
}
bool HasCyrillicScript(std::string_view utf8) {
    for (size_t i = 0; i < utf8.size();) {
        const char32_t code = NextCodePoint(utf8, i);
        if (code >= 0x0400 && code <= 0x052F)
            return true;
    }
    return false;
}
bool HasLatinLetters(std::string_view utf8) {
    for (size_t i = 0; i < utf8.size();) {
        const char32_t code = NextCodePoint(utf8, i);
        if ((code >= U'A' && code <= U'Z') || (code >= U'a' && code <= U'z') ||
            (code >= 0x00C0 && code <= 0x024F))
            return true;
    }
    return false;
}
// PP-OCRv6_rec_small maps Cyrillic letters onto visually similar ASCII letters (e.g. В->B, Т->T,
// М->M, р->p, и->u, с->c), producing words with mid-word lowercase->uppercase jumps like "IpuBeT".
bool LooksLikeMisreadCyrillic(std::string_view utf8) {
    int suspiciousWords = 0;
    bool inWord = false, seenLower = false, wordSuspicious = false;
    int wordLen = 0;
    for (unsigned char ch : utf8) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
            const bool upper = ch >= 'A' && ch <= 'Z';
            if (!inWord) {
                inWord = true;
                seenLower = !upper;
                wordSuspicious = false;
                wordLen = 1;
            } else {
                ++wordLen;
                if (!upper)
                    seenLower = true;
                else if (seenLower)
                    wordSuspicious = true;
            }
        } else {
            if (inWord && wordSuspicious && wordLen >= 3)
                ++suspiciousWords;
            inWord = seenLower = wordSuspicious = false;
            wordLen = 0;
        }
    }
    if (inWord && wordSuspicious && wordLen >= 3)
        ++suspiciousWords;
    return suspiciousWords >= 1;
}
// Arabic CTC recognition runs left-to-right across the image, producing visual order.
// Applying UBA L2/L4 converts visual order back to logical reading order while keeping
// embedded numbers, Latin runs, and combining marks intact.
std::string ReorderVisualToLogical(std::string_view utf8) {
    if (!HasRtlScript(utf8))
        return std::string(utf8);
    struct Cluster {
        std::vector<char32_t> codes;
        int dir{-1}; // 1=RTL, 0=LTR, 2=Number, -1=Neutral
        int level{0};
    };
    std::vector<Cluster> clusters;
    for (size_t i = 0; i < utf8.size();) {
        const char32_t code = NextCodePoint(utf8, i);
        if (IsCombiningMark(code) && !clusters.empty()) {
            clusters.back().codes.push_back(code);
            continue;
        }
        int dir = -1;
        if (IsNumberCodePoint(code))
            dir = 2;
        else if (IsRtlCodePoint(code))
            dir = 1;
        else if (IsStrongLtrCodePoint(code))
            dir = 0;
        clusters.push_back({{code}, dir, 0});
    }
    if (clusters.empty())
        return {};
    bool baseRtl = true;
    for (const auto& c : clusters) {
        if (c.dir == 1) {
            baseRtl = true;
            break;
        }
        if (c.dir == 0) {
            baseRtl = false;
            break;
        }
    }
    const int baseDir = baseRtl ? 1 : 0;
    auto effDir = [&](int dir) { return dir == 2 ? 1 : dir; };
    for (size_t i = 0; i < clusters.size();) {
        if (clusters[i].dir != -1) {
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < clusters.size() && clusters[j].dir == -1)
            ++j;
        const int left = i > 0 ? (clusters[i - 1].dir == 2 && !baseRtl ? 0 : effDir(clusters[i - 1].dir)) : baseDir;
        const int right =
            j < clusters.size() ? (clusters[j].dir == 2 && !baseRtl ? 0 : effDir(clusters[j].dir)) : baseDir;
        const int resolved = (left == right) ? left : baseDir;
        for (size_t k = i; k < j; ++k)
            clusters[k].dir = resolved;
        i = j;
    }
    for (auto& c : clusters) {
        if (c.dir == 1)
            c.level = 1;
        else if (c.dir == 2)
            c.level = 2;
        else
            c.level = baseRtl ? 2 : 0;
        if ((c.level & 1) != 0 && c.codes.size() == 1)
            c.codes[0] = MirrorBidiBracket(c.codes[0]);
    }
    for (int lvl = 2; lvl >= 1; --lvl) {
        for (size_t i = 0; i < clusters.size();) {
            if (clusters[i].level < lvl) {
                ++i;
                continue;
            }
            size_t j = i + 1;
            while (j < clusters.size() && clusters[j].level >= lvl)
                ++j;
            std::reverse(clusters.begin() + i, clusters.begin() + j);
            i = j;
        }
    }
    std::string out;
    out.reserve(utf8.size());
    for (const auto& c : clusters)
        for (char32_t code : c.codes)
            AppendUtf8(out, code);
    return out;
}
// Text-line orientation is a 0/180 classification. RapidOCR applies cls_thresh=0.9,
// so a weak 180-degree preference keeps the crop as detected.
float UpsideDownScore(const float* data) {
    const float margin = data[0] - data[1];
    if (margin > 30.f)
        return 0.f;
    return 1.f / (1.f + std::exp(margin));
}
struct TimeoutGuard {
    std::atomic<bool> timedOut{false};
    std::mutex mutex;
    std::condition_variable_any cv;
    std::jthread thread;
    explicit TimeoutGuard(Ort::RunOptions& run, std::chrono::seconds limit = std::chrono::seconds(15))
        : thread([this, &run, limit](std::stop_token done) {
              std::unique_lock lock(mutex);
              cv.wait_for(lock, done, limit, [] { return false; });
              if (!done.stop_requested()) {
                  timedOut = true;
                  run.SetTerminate();
              }
          }) {}
};
} // namespace
std::vector<TextLine> RecognizeBundled(const Image& image, int mode, std::stop_token stop, bool geometryOnly,
                                       bool preserveGeometry, bool retainLowConfidenceText,
                                       const std::vector<RECT>* precomputedBoxes,
                                       const std::vector<TextLine>* precomputedLines) {
    if (!ValidOcrMode(mode))
        throw AppError("ocr", "Invalid OCR mode");
    geometryOnly = geometryOnly || mode == OcrAuto || mode == OcrDirectVision;
    static std::timed_mutex mutex;
    std::unique_lock lock(mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(20)))
        CheckStop(stop);
    CheckStop(stop);
    // Scope sessions to this OCR request; success, cancellation and failure all
    // release weights, execution buffers and worker threads before network I/O.
    DiagnosticSpan loading("ocr_detector_load");
    auto engine = std::make_unique<Engine>();
    loading.End("ok", {{"model", engine->detectorVariant}});
    CheckStop(stop);
    Ort::RunOptions run;
    std::stop_callback cancel(stop, [&] { run.SetTerminate(); });
    TimeoutGuard watchdog(run);
    try {
        float scale = std::min(1.f, 1600.f / std::max(image.width, image.height));
        int w = std::max(32, int(std::round(image.width * scale / 32)) * 32),
            h = std::max(32, int(std::round(image.height * scale / 32)) * 32);
        std::vector<RECT> boxes;
        std::vector<OrientedBox> obbs;
        if (precomputedLines) {
            boxes.reserve(precomputedLines->size());
            obbs.reserve(precomputedLines->size());
            for (const auto& ln : *precomputedLines) {
                boxes.push_back(ln.rect);
                obbs.push_back(ln.obb);
            }
            engine->detector.reset();
        } else if (precomputedBoxes) {
            // The routing pass already detected this page; repeating the detector would add its cost to
            // every horizontal page for boxes that are byte identical.
            boxes = *precomputedBoxes;
            obbs.resize(boxes.size());
            engine->detector.reset();
        } else {
            DiagnosticSpan detection("ocr_detect", {{"width", w}, {"height", h}});
            auto input = Tensor(image, {0, 0, image.width, image.height}, w, h, w, true);
            auto det = engine->detector->Run(input, w, h, run);
            auto detected = BoxesWithObb(det, image);
            boxes.reserve(detected.size());
            obbs.reserve(detected.size());
            for (const auto& d : detected) {
                boxes.push_back(d.rect);
                obbs.push_back(d.obb);
            }
            detection.End("ok", {{"boxes", boxes.size()}});
        }
        // Detection and recognition never need their weights or tensors concurrently.
        engine->detector.reset();
        CheckStop(stop);
        std::vector<TextLine> lines;
        if (geometryOnly) {
            for (size_t i = 0; i < boxes.size(); ++i)
                lines.push_back({boxes[i], " ", 0.f, obbs[i]});
            return lines;
        }
        if (boxes.empty())
            return lines;
        // Orientation classification runs once per detected line; unavailable or
        // unusable weights leave every line in its detected direction.
        std::vector<bool> upsideDown(boxes.size());
        {
            DiagnosticSpan orientation("ocr_orientation");
            const size_t detected = boxes.size();
            size_t flipped = 0;
            bool classified = false;
            try {
                if (auto* classifier = engine->Classifier()) {
                    for (size_t i = 0; i < boxes.size(); ++i) {
                        CheckStop(stop);
                        bool done = false;
                        for (int attempt = 0; attempt < 2 && !done; ++attempt) {
                            const int height = engine->orientationHeight, width = engine->orientationWidth;
                            try {
                                auto pixels = Tensor(image, boxes[i], width, height, width, false);
                                auto output = classifier->Run(pixels, width, height, run);
                                auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
                                if (shape.size() != 2 || shape[0] != 1 || shape[1] != 2)
                                    throw AppError("ocr", "Invalid orientation output");
                                upsideDown[i] = UpsideDownScore(output.GetTensorData<float>()) >= .9f;
                                flipped += upsideDown[i];
                                // Lock the working geometry so later lines skip the fallback.
                                engine->orientationShapeKnown = true;
                                done = true;
                            } catch (const Ort::Exception&) {
                                // An unknown exported geometry falls back once to the
                                // PaddleOCR 48x192 text-line classifier layout.
                                if (engine->orientationShapeKnown ||
                                    (height == 48 && width == 192))
                                    throw;
                                engine->orientationHeight = 48;
                                engine->orientationWidth = 192;
                            }
                        }
                    }
                    classified = true;
                }
            } catch (const std::exception&) {
                CheckStop(stop);
                std::fill(upsideDown.begin(), upsideDown.end(), false);
                flipped = 0;
                classified = false;
                DiagnosticEvent("ocr_fallback", {{"reason", "classifier_failed"}});
            }
            engine->classifier.reset();
            if (classified)
                orientation.End("ok", {{"boxes", detected}});
            else
                orientation.End("skipped");
        }
        CheckStop(stop);
        auto recognize = [&](Model& model, RECT box, OrientedBox obb, bool rotated, bool rtlOrder) {
            CheckStop(stop);
            const int inputHeight = model.InputHeight(48);
            const int fixedWidth = model.InputWidth();
            if (obb.valid()) {
                const bool vertical = obb.height > obb.width * 1.5f;
                const float spanW = vertical ? obb.height : obb.width;
                const float spanH = vertical ? obb.width : obb.height;
                int contentWidth = fixedWidth;
                if (fixedWidth <= 0)
                    contentWidth = std::max(8, int(std::ceil(float(inputHeight) * spanW / std::max(1.f, spanH))));
                if (contentWidth <= 4096) {
                    const int width = fixedWidth > 0 ? fixedWidth : std::max(320, contentWidth);
                    auto pixels = OrientedTensor(image, obb, width, inputHeight, contentWidth, vertical);
                    auto output = model.Run(pixels, width, inputHeight, run);
                    auto [text, score] = Decode(output, model.alphabet);
                    if (rtlOrder && !text.empty())
                        text = ReorderVisualToLogical(text);
                    return TextLine{box, std::move(text), score, obb};
                }
            }
            const bool tall = Height(box) > Width(box) * 3 / 2;
            auto attempt = [&](bool vertical, bool quarter, int blocks) {
                // Float arithmetic keeps the aspect-ratio width bit-identical to the original mobile
                // preprocessing; integer division would drop a pixel.
                int contentWidth = fixedWidth;
                if (fixedWidth <= 0) {
                    if (vertical)
                        contentWidth = std::max(8, inputHeight * blocks);
                    else if (quarter)
                        contentWidth =
                            std::max(8, int(std::ceil(float(inputHeight) * Height(box) / Width(box))));
                    else
                        contentWidth =
                            std::max(8, int(std::ceil(float(inputHeight) * Width(box) / Height(box))));
                }
                if (contentWidth > 4096)
                    return TextLine{box, " ", 0.f};
                const int width = fixedWidth > 0 ? fixedWidth : std::max(320, contentWidth);
                auto pixels = Tensor(image, box, width, inputHeight, contentWidth, false,
                                     rotated && !vertical && !quarter, vertical, quarter, blocks);
                auto output = model.Run(pixels, width, inputHeight, run);
                auto [text, score] = Decode(output, model.alphabet);
                if (rtlOrder && !text.empty())
                    text = ReorderVisualToLogical(text);
                return TextLine{box, std::move(text), score};
            };
            if (!tall)
                return attempt(false, false, 1);
            // The column reading needs a block count, and the box margin makes the direct ratio of
            // height to width read low. A few candidates cover the usual character sizes, and the
            // strongest reading wins.
            const int ratio = std::max(1, int(std::lround(double(Height(box)) / std::max(1, Width(box)))));
            const int candidates[]{ratio, ratio + 1, ratio + 2,
                                   std::max(1, int(std::lround(double(Height(box)) /
                                                               std::max(1.0, Width(box) * 1.4))))};
            TextLine best{box, "", 0.f};
            for (int blocks : candidates) {
                auto line = attempt(true, false, blocks);
                if (line.confidence > best.confidence)
                    best = std::move(line);
            }
            if (best.confidence >= .5f)
                return best;
            // A line turned a quarter reads differently from a column of upright characters.
            auto turned = attempt(false, true, 1);
            return turned.confidence > best.confidence ? turned : best;
        };
        try {
            auto quality = [](const TextLine& line) {
                double units = 0;
                for (size_t i = 0; i < line.text.size();) {
                    const char32_t code = NextCodePoint(line.text, i);
                    if (IsCombiningMark(code))
                        continue;
                    if ((code >= 0x2E80 && code <= 0x9FFF) || (code >= 0xAC00 && code <= 0xD7AF) ||
                        (code >= 0xF900 && code <= 0xFAFF) || (code >= 0xFF00 && code <= 0xFFEF))
                        units += .95;
                    else
                        units += .55;
                }
                const double coverage =
                    std::min(1., units * Height(line.rect) / std::max(1, Width(line.rect)));
                return float(line.confidence * (.6 + .4 * coverage));
            };
            std::vector<int> lineLanguages(boxes.size(), mode);
            bool triedRussian = false, triedGreek = false;
            auto lineNeedsRefine = [&](size_t i) {
                const float q = quality(lines[i]);
                if (q < .90f)
                    return true;
                if (!triedGreek && HasGreekScript(lines[i].text) && lineLanguages[i] != OcrGreek)
                    return true;
                if (!triedRussian && LooksLikeMisreadCyrillic(lines[i].text) &&
                    lineLanguages[i] != OcrRussian)
                    return true;
                return false;
            };
            auto acceptCandidate = [&](size_t idx, TextLine&& cand, int language) {
                const float newQ = quality(cand);
                const float oldQ = quality(lines[idx]);
                const bool upgradeCyrillic =
                    language == OcrRussian && LooksLikeMisreadCyrillic(lines[idx].text) &&
                    HasCyrillicScript(cand.text) && newQ >= oldQ - .03f;
                const bool upgradeGreek =
                    language == OcrGreek && HasGreekScript(lines[idx].text) &&
                    HasGreekScript(cand.text) && newQ >= oldQ - .03f;
                if (newQ > oldQ || upgradeCyrillic || upgradeGreek) {
                    lines[idx] = std::move(cand);
                    lineLanguages[idx] = language;
                }
            };
            auto satisfied = [&] {
                double score = 0, weight = 0, lowWeight = 0;
                for (size_t i = 0; i < lines.size(); ++i) {
                    if ((!triedGreek && HasGreekScript(lines[i].text) && lineLanguages[i] != OcrGreek) ||
                        (!triedRussian && LooksLikeMisreadCyrillic(lines[i].text) &&
                         lineLanguages[i] != OcrRussian))
                        return false;
                    const double importance = std::sqrt(double(std::max(1, Width(lines[i].rect))));
                    const float confidence = quality(lines[i]);
                    score += confidence * importance;
                    weight += importance;
                    if (confidence < .72f)
                        lowWeight += importance;
                }
                return score / std::max(1., weight) >= .88 && lowWeight <= weight * .10;
            };
            auto runAll = [&](int language) {
                if (language == OcrRussian)
                    triedRussian = true;
                if (language == OcrGreek)
                    triedGreek = true;
                DiagnosticSpan recognitionLoad("ocr_recognizer_load", {{"ocr_mode", language}});
                auto& model = engine->Recognizer(language);
                recognitionLoad.End("ok", {{"model", engine->recognizerVariant}});
                DiagnosticSpan recognition("ocr_recognize", {{"ocr_mode", language}});
                std::vector<TextLine> candidate;
                candidate.reserve(boxes.size());
                size_t recognized = 0;
                const bool rtl = language == OcrArabic;
                for (size_t i = 0; i < boxes.size(); ++i) {
                    if (mode == OcrLocalAuto && !lines.empty() && !lineNeedsRefine(i)) {
                        candidate.push_back(lines[i]);
                    } else {
                        candidate.push_back(recognize(model, boxes[i], obbs[i], upsideDown[i], rtl));
                        ++recognized;
                    }
                }
                recognition.End("ok", {{"recognized_lines", recognized}});
                if (lines.empty()) {
                    lineLanguages.assign(candidate.size(), language);
                    lines = std::move(candidate);
                } else {
                    for (size_t i = 0; i < lines.size(); ++i)
                        acceptCandidate(i, std::move(candidate[i]), language);
                }
            };
            if (mode != OcrLocalAuto) {
                runAll(mode);
            } else {
                const auto root = AppDirectory();
                std::error_code modelEc;
                const bool hasUnifiedModel =
                    std::filesystem::is_regular_file(root / L"models/rec-ja-server.onnx", modelEc);
                const auto recognitionStart = std::chrono::steady_clock::now();
                constexpr auto kAutoCascadeBudget = std::chrono::seconds(6);
                if (hasUnifiedModel) {
                    runAll(OcrJapanese);
                } else {
                    for (int language : {OcrEnglish, OcrJapanese, OcrKorean}) {
                        runAll(language);
                        if (satisfied())
                            break;
                    }
                }
                if (!satisfied()) {
                    const std::array<int, 8> probeOrder{OcrRussian, OcrGreek,   OcrKorean,
                                                        OcrThai,    OcrArabic,  OcrDevanagari,
                                                        OcrEnglish, OcrLatin};
                    for (int language : probeOrder) {
                        if (!hasUnifiedModel &&
                            (language == OcrKorean || language == OcrEnglish))
                            continue;
                        if (std::chrono::steady_clock::now() - recognitionStart >= kAutoCascadeBudget)
                            break;
                        if ((language == OcrKorean || language == OcrEnglish)
                                ? !FindModel(root, RecognizerCandidates(language)).has_value()
                                : !HasDedicatedRecognizer(root, language))
                            continue;
                        std::vector<size_t> uncertain;
                        bool anyLatin = false, anyGreek = false, anyCyrillicHint = false;
                        for (size_t i = 0; i < lines.size(); ++i) {
                            if (!lineNeedsRefine(i))
                                continue;
                            uncertain.push_back(i);
                            anyLatin = anyLatin || HasLatinLetters(lines[i].text);
                            anyGreek = anyGreek || HasGreekScript(lines[i].text);
                            anyCyrillicHint = anyCyrillicHint || LooksLikeMisreadCyrillic(lines[i].text);
                        }
                        if (uncertain.empty())
                            break;
                        if (hasUnifiedModel && (language == OcrEnglish || language == OcrLatin) && !anyLatin)
                            continue;
                        if (language == OcrRussian)
                            triedRussian = true;
                        if (language == OcrGreek)
                            triedGreek = true;
                        DiagnosticSpan recognitionLoad("ocr_recognizer_load", {{"ocr_mode", language}});
                        auto& model = engine->Recognizer(language);
                        recognitionLoad.End("ok", {{"model", engine->recognizerVariant}});
                        DiagnosticSpan recognition("ocr_recognize", {{"ocr_mode", language}});
                        const bool rtl = language == OcrArabic;
                        size_t recognized = 0;
                        if (uncertain.size() <= 4) {
                            for (size_t idx : uncertain) {
                                auto cand = recognize(model, boxes[idx], obbs[idx], upsideDown[idx], rtl);
                                ++recognized;
                                acceptCandidate(idx, std::move(cand), language);
                            }
                        } else {
                            // Probe up to 3 representative uncertain lines first so non-matching
                            // script models exit quickly without running every box on the page.
                            std::vector<size_t> ranked = uncertain;
                            std::stable_sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) {
                                const float qa = quality(lines[a]), qb = quality(lines[b]);
                                if ((qa < .80f) != (qb < .80f))
                                    return qa < .80f;
                                const long long areaA = 1LL * Width(boxes[a]) * Height(boxes[a]);
                                const long long areaB = 1LL * Width(boxes[b]) * Height(boxes[b]);
                                return areaA > areaB;
                            });
                            std::vector<size_t> probes;
                            for (size_t idx : ranked) {
                                if (Width(boxes[idx]) >= 20 && Height(boxes[idx]) >= 8)
                                    probes.push_back(idx);
                                if (probes.size() == 3)
                                    break;
                            }
                            if (probes.empty())
                                for (size_t k = 0; k < std::min<size_t>(3, ranked.size()); ++k)
                                    probes.push_back(ranked[k]);
                            bool promoted = false;
                            for (size_t idx : probes) {
                                const float oldQ = quality(lines[idx]);
                                const std::string oldText = lines[idx].text;
                                auto cand = recognize(model, boxes[idx], obbs[idx], upsideDown[idx], rtl);
                                ++recognized;
                                const float newQ = quality(cand);
                                if ((newQ >= .75f && newQ > oldQ + .05f) ||
                                    (oldQ < .60f && newQ >= .68f) ||
                                    (language == OcrRussian && LooksLikeMisreadCyrillic(oldText) &&
                                     HasCyrillicScript(cand.text) && newQ >= oldQ - .03f) ||
                                    (language == OcrGreek && HasGreekScript(oldText) &&
                                     HasGreekScript(cand.text) && newQ >= oldQ - .03f))
                                    promoted = true;
                                acceptCandidate(idx, std::move(cand), language);
                            }
                            if (promoted) {
                                for (size_t idx : uncertain) {
                                    if (std::find(probes.begin(), probes.end(), idx) != probes.end())
                                        continue;
                                    auto cand = recognize(model, boxes[idx], obbs[idx], upsideDown[idx], rtl);
                                    ++recognized;
                                    acceptCandidate(idx, std::move(cand), language);
                                }
                            }
                        }
                        recognition.End("ok", {{"recognized_lines", recognized}});
                        if (satisfied())
                            break;
                    }
                }
                for (auto& line : lines)
                    line.confidence = quality(line);
                for (int language : {OcrEnglish, OcrJapanese, OcrKorean, OcrChinese, OcrLatin, OcrRussian,
                                     OcrThai, OcrArabic, OcrGreek, OcrDevanagari}) {
                    const auto count = std::count(lineLanguages.begin(), lineLanguages.end(), language);
                    if (count)
                        DiagnosticEvent("ocr_language_selected",
                                        {{"ocr_mode", language}, {"recognized_lines", count}});
                }
            }
            // Detach leading/trailing UI icons (bug, cloud, chat bubble, info badge, chevron) that DB
            // grouped into the same horizontal crop as adjacent button or label text.
            {
                auto trimAscii = [](std::string_view s) {
                    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                        s.remove_prefix(1);
                    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                        s.remove_suffix(1);
                    return s;
                };
                auto isBlank = [&](std::string_view s) { return trimAscii(s).empty(); };
                auto isTrailingPunct = [](char c) {
                    return c == '.' || c == '?' || c == '!' || c == ':' || c == ';' || c == ')' || c == ']' ||
                           c == '%' || c == '"' || c == '\'';
                };
                auto hasAlnum = [](std::string_view s) {
                    for (unsigned char c : s)
                        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80)
                            return true;
                    return false;
                };
                // Group line indices by recognizer language (starting with the loaded model) to avoid
                // redundant ONNX session reloads during affix-icon verification.
                std::vector<size_t> order(lines.size());
                for (size_t i = 0; i < lines.size(); ++i)
                    order[i] = i;
                const int loadedLang = engine->recognizerMode;
                auto langOf = [&](size_t idx) {
                    return (idx < lineLanguages.size() && lineLanguages[idx] != OcrLocalAuto)
                               ? lineLanguages[idx]
                               : (loadedLang > 0 ? loadedLang : OcrJapanese);
                };
                std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                    const int la = langOf(a), lb = langOf(b);
                    if ((la == loadedLang) != (lb == loadedLang))
                        return la == loadedLang;
                    return la < lb;
                });
                for (size_t i : order) {
                    CheckStop(stop);
                    if (isBlank(lines[i].text) || lines[i].obb.valid())
                        continue;
                    RECT box = lines[i].rect;
                    const int bw = Width(box), bh = Height(box);
                    if (bw < 24 || bh < 10 || bh > 68 || bw <= bh)
                        continue;
                    const int lang = langOf(i);
                    const bool rtl = lang == OcrArabic;
                    auto extractSpans = [&](RECT r, int& outInkH) {
                        struct InkSpan {
                            int x0{}, x1{};
                        };
                        std::vector<InkSpan> spans;
                        const int rw = Width(r), rh = Height(r);
                        outInkH = std::max(8, (rh * 3) / 5);
                        if (rw < 16 || rh < 8)
                            return spans;
                        std::array<size_t, 4096> hist{};
                        for (int y = r.top; y < r.bottom; ++y)
                            for (int x = r.left; x < r.right; ++x) {
                                const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                                ++hist[(int(p[2] >> 4) << 8) | (int(p[1] >> 4) << 4) | int(p[0] >> 4)];
                            }
                        const int m = int(std::max_element(hist.begin(), hist.end()) - hist.begin());
                        const int br = ((m >> 8) & 15) * 16 + 8;
                        const int bg = ((m >> 4) & 15) * 16 + 8;
                        const int bb = (m & 15) * 16 + 8;
                        std::vector<int> colInk(size_t(rw), 0);
                        int minY = rh, maxY = 0;
                        for (int x = 0; x < rw; ++x) {
                            for (int y = 0; y < rh; ++y) {
                                const auto* p = &image.bgra[(size_t(r.top + y) * image.width + (r.left + x)) * 4];
                                if (std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) +
                                        std::abs(int(p[0]) - bb) >=
                                    46) {
                                    ++colInk[size_t(x)];
                                    minY = std::min(minY, y);
                                    maxY = std::max(maxY, y + 1);
                                }
                            }
                        }
                        if (maxY > minY)
                            outInkH = std::max(7, maxY - minY);
                        for (int x = 0; x < rw;) {
                            if (colInk[size_t(x)] == 0) {
                                ++x;
                                continue;
                            }
                            const int x0 = x;
                            int gap = 0;
                            while (x < rw) {
                                if (colInk[size_t(x)] > 0) {
                                    gap = 0;
                                    ++x;
                                } else if (gap == 0 && x + 1 < rw && colInk[size_t(x + 1)] > 0) {
                                    gap = 1;
                                    ++x;
                                } else {
                                    break;
                                }
                            }
                            const int x1 = (x > x0 && colInk[size_t(x - 1)] == 0) ? x - 1 : x;
                            if (x1 > x0)
                                spans.push_back({x0, x1});
                        }
                        return spans;
                    };
                    auto shouldDetach = [&](const TextLine& fullLine, const TextLine& iconLine,
                                            const TextLine& bodyLine, bool trailing) -> bool {
                        const auto fullT = trimAscii(fullLine.text);
                        const auto bodyT = trimAscii(bodyLine.text);
                        const auto iconT = trimAscii(iconLine.text);
                        if (bodyT.empty() || bodyLine.confidence < .70f)
                            return false;
                        if (bodyT == fullT)
                            return true;
                        if (trailing && !fullT.empty() && isTrailingPunct(fullT.back()) &&
                            (bodyT.empty() || bodyT.back() != fullT.back()))
                            return false;
                        const float fullRawConf = fullLine.confidence;
                        const float bodyQ = mode == OcrLocalAuto ? quality(bodyLine) : bodyLine.confidence;
                        if ((iconT.empty() || iconLine.confidence < .68f) && bodyQ >= fullRawConf - .06f)
                            return true;
                        if (bodyQ >= fullRawConf + .08f)
                            return true;
                        // Detect hallucination instability on leading icons (e.g. full="Jo Auto" vs icon="gro").
                        if (!trailing && fullT.size() > bodyT.size() &&
                            fullT.substr(fullT.size() - bodyT.size()) == bodyT) {
                            auto prefix = trimAscii(fullT.substr(0, fullT.size() - bodyT.size()));
                            if (!prefix.empty() && !iconT.empty() && prefix[0] != iconT[0] &&
                                bodyLine.confidence >= .85f)
                                return true;
                        }
                        // Detach a trailing isolated non-alphanumeric symbol/arrow (e.g. '->' read as '+').
                        if (trailing && !fullT.empty() && !isTrailingPunct(fullT.back()) &&
                            fullT.size() > bodyT.size() + 1 && fullT.substr(0, bodyT.size()) == bodyT &&
                            fullT[bodyT.size()] == ' ') {
                            auto suffix = trimAscii(fullT.substr(bodyT.size()));
                            if (!suffix.empty() && !hasAlnum(suffix) && !hasAlnum(iconT) &&
                                bodyLine.confidence >= .82f)
                                return true;
                        }
                        return false;
                    };
                    // Check trailing icon/chevron first so removing a wide trailing dropdown gap does
                    // not skew inner-gap statistics when subsequently checking a leading icon.
                    for (int pass : {1, 0}) {
                        RECT curBox = lines[i].rect;
                        int inkH = 12;
                        auto spans = extractSpans(curBox, inkH);
                        if (spans.size() < 2)
                            break;
                        const int minIconGap = std::max(5, int(std::lround(inkH * .45f)));
                        const int maxIconW = std::max(22, int(std::lround(inkH * 1.85f)));
                        if (pass == 0) {
                            const int w0 = spans.front().x1 - spans.front().x0;
                            const int gap0 = spans[1].x0 - spans.front().x1;
                            const int restW = spans.back().x1 - spans[1].x0;
                            int medInner = 0;
                            if (spans.size() >= 3) {
                                std::vector<int> ig;
                                for (size_t k = 1; k + 1 < spans.size(); ++k)
                                    ig.push_back(spans[k + 1].x0 - spans[k].x1);
                                std::sort(ig.begin(), ig.end());
                                medInner = ig[ig.size() / 2];
                            }
                            if (w0 >= 4 && w0 <= maxIconW && gap0 >= minIconGap &&
                                restW >= std::max(12, int(inkH * .90f)) &&
                                (spans.size() == 2 || gap0 >= medInner + 2)) {
                                auto& activeModel = engine->Recognizer(lang);
                                const LONG splitX = curBox.left + (spans.front().x1 + spans[1].x0) / 2;
                                const RECT iconRect{curBox.left, curBox.top,
                                                    std::min(curBox.right, curBox.left + spans.front().x1 + 3),
                                                    curBox.bottom};
                                const RECT bodyRect{std::max(curBox.left + spans.front().x1 + 2, splitX),
                                                    curBox.top, curBox.right, curBox.bottom};
                                if (Width(iconRect) >= 4 && Width(bodyRect) >= 8) {
                                    auto iconRes = recognize(activeModel, iconRect, {}, upsideDown[i], rtl);
                                    auto bodyRes = recognize(activeModel, bodyRect, {}, upsideDown[i], rtl);
                                    if (shouldDetach(lines[i], iconRes, bodyRes, false)) {
                                        if (mode == OcrLocalAuto)
                                            bodyRes.confidence = quality(bodyRes);
                                        lines[i] = std::move(bodyRes);
                                    }
                                }
                            }
                        } else {
                            const auto& lastS = spans.back();
                            const auto& prevS = spans[spans.size() - 2];
                            const int wLast = lastS.x1 - lastS.x0;
                            const int gapLast = lastS.x0 - prevS.x1;
                            const int bodyW = prevS.x1 - spans.front().x0;
                            int medInner = 0;
                            if (spans.size() >= 3) {
                                std::vector<int> ig;
                                for (size_t k = 0; k + 2 < spans.size(); ++k)
                                    ig.push_back(spans[k + 1].x0 - spans[k].x1);
                                std::sort(ig.begin(), ig.end());
                                medInner = ig[ig.size() / 2];
                            }
                            const int minTrailingGap = std::max(3, int(std::lround(inkH * .34f)));
                            const auto curT = trimAscii(lines[i].text);
                            const bool endsSentencePunct = !curT.empty() && isTrailingPunct(curT.back());
                            if (wLast >= 4 && wLast <= maxIconW && gapLast >= minTrailingGap &&
                                (!endsSentencePunct || gapLast >= minIconGap) &&
                                bodyW >= std::max(12, int(inkH * .90f)) &&
                                (spans.size() == 2 || gapLast >= medInner + 1)) {
                                auto& activeModel = engine->Recognizer(lang);
                                const LONG splitX = curBox.left + (prevS.x1 + lastS.x0 + 1) / 2;
                                const RECT iconRect{std::max(curBox.left, curBox.left + lastS.x0 - 3),
                                                    curBox.top, curBox.right, curBox.bottom};
                                const RECT bodyRect{
                                    curBox.left, curBox.top,
                                    std::min({curBox.left + lastS.x0 - 2, splitX, curBox.left + prevS.x1 + 1}),
                                    curBox.bottom};
                                if (Width(iconRect) >= 4 && Width(bodyRect) >= 8) {
                                    auto iconRes = recognize(activeModel, iconRect, {}, upsideDown[i], rtl);
                                    auto bodyRes = recognize(activeModel, bodyRect, {}, upsideDown[i], rtl);
                                    if (shouldDetach(lines[i], iconRes, bodyRes, true)) {
                                        if (mode == OcrLocalAuto)
                                            bodyRes.confidence = quality(bodyRes);
                                        lines[i] = std::move(bodyRes);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } catch (const std::exception&) {
            CheckStop(stop);
            if (!preserveGeometry)
                throw;
            DiagnosticEvent("ocr_fallback",
                            {{"reason", watchdog.timedOut ? "recognition_timeout" : "recognition_failed"},
                             {"boxes", boxes.size()},
                             {"recognized_lines", lines.size()}});
            // Recognition failure must not reload and rerun a detector that already succeeded.
            for (size_t i = lines.size(); i < boxes.size(); ++i)
                lines.push_back({boxes[i], " ", 0.f});
            for (auto& line : lines)
                if ((!retainLowConfidenceText && line.confidence < .65f) || line.text.empty()) {
                    line.text = " ";
                    line.confidence = 0.f;
                }
            return lines;
        }
        size_t rejected = 0;
        for (auto& line : lines)
            if ((!retainLowConfidenceText && line.confidence < .65f) || line.text.empty()) {
                ++rejected;
                line.text = " ";
                line.confidence = 0.f;
            }
        if (watchdog.timedOut)
            throw AppError("ocr", "OCR time limit exceeded");
        CheckStop(stop);
        // Low confidence must not silently constrain vision translation to incomplete OCR IDs.
        if (!preserveGeometry && rejected > std::max<size_t>(2, boxes.size() / 5))
            throw AppError("ocr", "OCR confidence insufficient");
        return lines;
    } catch (const Ort::Exception&) {
        CheckStop(stop);
        DiagnosticEvent("ocr_error", {{"reason", watchdog.timedOut ? "timeout" : "inference_failed"}});
        throw AppError("ocr", watchdog.timedOut ? "OCR time limit exceeded" : "Bundled OCR inference failed");
    }
}
bool MangaOcrAvailable() {
    std::error_code error;
    const auto root = AppDirectory();
    return std::filesystem::is_regular_file(root / L"models/manga_det.onnx", error) &&
           std::filesystem::is_regular_file(root / L"models/manga_rec.onnx", error) &&
           std::filesystem::is_regular_file(root / L"models/ppocrv6_dict.txt", error);
}
std::vector<TextLine> RecognizeMangaBundled(const Image& image, std::stop_token stop) {
    if (image.width <= 0 || image.height <= 0)
        return {};
    static std::timed_mutex mutex;
    std::unique_lock lock(mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(20)))
        CheckStop(stop);
    CheckStop(stop);
    auto engine = std::make_unique<Engine>();
    auto* detector = engine->MangaDetector();
    auto* recognizer = engine->MangaRecognizer();
    // A machine without the manga weights keeps the general engine; the caller routes accordingly.
    if (!detector || !recognizer)
        return {};
    Ort::RunOptions run;
    std::stop_callback cancel(stop, [&] { run.SetTerminate(); });
    TimeoutGuard watchdog(run);
    auto detection = DiagnosticSpan("ocr_detect_manga");
    try {
        // Official geometry: long side 960, or the pixel budget of a 3:4 page for long strips.
        const double longSide = double(std::max(image.width, image.height));
        const double shortSide = double(std::max(1, std::min(image.width, image.height)));
        double ratio = 960.0 / longSide;
        if (longSide / shortSide > 2.0)
            ratio = std::min(1.0, std::sqrt(0.75 * 960.0 * 960.0 /
                                            (double(image.width) * double(image.height))));
        const int w = std::max(32, int(std::round(image.width * ratio / 32)) * 32);
        const int h = std::max(32, int(std::round(image.height * ratio / 32)) * 32);
        auto input = Tensor(image, {0, 0, image.width, image.height}, w, h, w, true);
        auto det = detector->Run(input, w, h, run);
        auto boxes = BoxesWithObb(det, image, {.15f, .25f, 1.4f, 4});
        detection.End("ok", {{"boxes", boxes.size()}, {"width", w}, {"height", h}});
        engine->mangaDetector.reset();
        CheckStop(stop);
        std::vector<TextLine> lines;
        if (boxes.empty())
            return lines;
        auto recognition = DiagnosticSpan("ocr_recognize_manga");
        const int inputHeight = recognizer->InputHeight(48);
        const int fixedWidth = recognizer->InputWidth();
        for (const auto& detBox : boxes) {
            CheckStop(stop);
            const RECT box = detBox.rect;
            const OrientedBox obb = detBox.obb;
            // A column reads after a quarter turn, which is the orientation the weights expect.
            const bool vertical = obb.valid() ? (obb.height > obb.width) : (Height(box) > Width(box));
            const int spanWidth = obb.valid() ? int(std::lround(vertical ? obb.height : obb.width))
                                              : (vertical ? Height(box) : Width(box));
            const int spanHeight = obb.valid() ? int(std::lround(vertical ? obb.width : obb.height))
                                               : (vertical ? Width(box) : Height(box));
            int contentWidth = fixedWidth;
            if (contentWidth <= 0)
                contentWidth = std::max(16, int(std::round(48.0 * spanWidth / std::max(1, spanHeight))));
            if (contentWidth > 4096)
                continue;
            auto pixels = obb.valid()
                              ? OrientedTensor(image, obb, contentWidth, inputHeight, contentWidth, vertical)
                              : Tensor(image, box, contentWidth, inputHeight, contentWidth, false, false,
                                       false, false, 0, vertical);
            auto output = recognizer->Run(pixels, contentWidth, inputHeight, run);
            auto [text, score] = Decode(output, recognizer->alphabet);
            lines.push_back({box, std::move(text), score, obb});
        }
        recognition.End("ok", {{"recognized_lines", lines.size()}});
        // Manga pages read columns right to left, each column top to bottom. Horizontal passages stay
        // in natural order, which the detector already produced.
        const bool columns =
            std::count_if(lines.begin(), lines.end(),
                          [](const TextLine& line) { return Height(line.rect) > Width(line.rect); }) * 2 >
            int(lines.size());
        if (columns)
            std::stable_sort(lines.begin(), lines.end(), [](const TextLine& a, const TextLine& b) {
                const int tolerance = std::max(8, int(std::min(Width(a.rect), Height(a.rect)) / 2));
                if (std::abs(a.rect.left - b.rect.left) > tolerance)
                    return a.rect.right > b.rect.right;
                return a.rect.top < b.rect.top;
            });
        if (watchdog.timedOut)
            throw AppError("ocr", "Manga OCR time limit exceeded");
        return lines;
    } catch (const Ort::Exception&) {
        const bool timedOut = watchdog.timedOut.load();
        detection.Fail(AppError("ocr", timedOut ? "Manga OCR time limit exceeded" : "Manga OCR inference failed"));
        CheckStop(stop);
        throw AppError("ocr", timedOut ? "Manga OCR time limit exceeded" : "Manga OCR inference failed");
    }
}
} // namespace sat

