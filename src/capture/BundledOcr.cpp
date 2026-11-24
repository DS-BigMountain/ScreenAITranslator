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
// DB post-processing parameters. The general detector keeps its original bias toward strict
// contours; the manga detector pairs a low binarisation threshold with a soft average score and a
// smaller expansion, which is what keeps thin vertical strokes and kana apart on artwork.
struct DbParams {
    float threshold{.3f};
    float minAverage{.48f};
    float unclip{1.6f};
    size_t minPixels{4};
};
std::vector<RECT> Boxes(const Ort::Value& output, const Image& image, DbParams params = {}) {
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
        // A tall box is either a vertical column or a rotated line. Both reach recognition; the
        // recognizer decides which reading the crop supports.
        boxes.push_back(rect);
        rawBoxes.push_back(raw);
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
    // Split wide horizontal boxes that bridge across separate UI buttons, pill borders or column gaps.
    std::vector<RECT> splitBoxes;
    splitBoxes.reserve(boxes.size());
    auto key12 = [](const unsigned char* p) {
        return (int(p[2] >> 4) << 8) | (int(p[1] >> 4) << 4) | (p[0] >> 4);
    };
    for (const RECT& rect : boxes) {
        const int bw = Width(rect), bh = Height(rect);
        if (bh < 10 || bh > 72 || bw <= bh * 2 || image.bgra.size() != size_t(image.width) * image.height * 4) {
            splitBoxes.push_back(rect);
            continue;
        }
        std::array<size_t, 4096> hist{};
        size_t totalSampled = 0;
        for (int y = rect.top; y < rect.bottom; ++y)
            for (int x = rect.left; x < rect.right; ++x) {
                ++hist[key12(&image.bgra[(size_t(y) * image.width + x) * 4])];
                ++totalSampled;
            }
        if (!totalSampled) {
            splitBoxes.push_back(rect);
            continue;
        }
        const int mode = int(std::max_element(hist.begin(), hist.end()) - hist.begin());
        const int br = ((mode >> 8) & 15) * 16 + 8;
        const int bg = ((mode >> 4) & 15) * 16 + 8;
        const int bb = (mode & 15) * 16 + 8;
        const int y0 = std::min<int>(rect.bottom - 1, rect.top + std::max(1, bh / 8));
        const int y1 = std::max<int>(y0 + 1, rect.bottom - std::max(1, bh / 8));
        std::vector<int> colInk(size_t(bw), 0);
        std::vector<unsigned char> colBorder(size_t(bw), 0);
        for (int x = 0; x < bw; ++x) {
            const int sx = rect.left + x;
            int ink = 0, minDiff = 999, maxDiff = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                const int d = std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb);
                if (d >= 56)
                    ++ink;
                minDiff = std::min(minDiff, d);
                maxDiff = std::max(maxDiff, d);
            }
            // A subtle vertical container border has uniform colour from top to bottom.
            const bool uniformVerticalLine = (y1 - y0 >= 8) && (maxDiff - minDiff <= 24) && maxDiff < 160;
            if (uniformVerticalLine && minDiff >= 20)
                colBorder[size_t(x)] = 1;
            colInk[size_t(x)] = uniformVerticalLine ? 0 : ink;
        }
        // Suppress structural vertical spines (axes, table rules) that continue above and below
        // the box even when horizontal axis ticks attach to them, plus isolated 1-2 px full-height
        // vertical border strokes. Narrow standalone glyphs ("i", "l", "1") never extend beyond both
        // vertical edges of the text box and remain intact.
        auto edgeHasInk = [&](int sx, int sy) {
            if (sy < 0 || sy >= image.height)
                return false;
            const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
            const int d = std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb);
            return d >= 44;
        };
        const int minSpineRows = std::max(6, ((y1 - y0) * 9) / 10);
        const int maxTickRows = std::max(3, bh / 5);
        for (int x = 2; x + 2 < bw; ++x) {
            auto isSpineCol = [&](int cx) {
                if (colInk[size_t(cx)] < minSpineRows)
                    return false;
                const int sx = rect.left + cx;
                if (!edgeHasInk(sx, rect.top) || !edgeHasInk(sx, rect.bottom - 1))
                    return false;
                int above = 0, below = 0;
                for (int dy = 1; dy <= 4; ++dy) {
                    above += edgeHasInk(sx, rect.top - dy) ? 1 : 0;
                    below += edgeHasInk(sx, rect.bottom - 1 + dy) ? 1 : 0;
                }
                return above >= 3 && below >= 3;
            };
            if (!isSpineCol(x))
                continue;
            int run = 1;
            while (x + run < bw - 1 && run < 3 && isSpineCol(x + run))
                ++run;
            for (int k = 0; k < run; ++k) {
                colInk[size_t(x + k)] = 0;
                colBorder[size_t(x + k)] = 1;
            }
            const int tickReach = std::min(8, std::max(3, bh / 2));
            for (int lx = x - 1; lx >= std::max(0, x - tickReach); --lx) {
                if (colInk[size_t(lx)] == 0)
                    continue;
                if (colInk[size_t(lx)] <= maxTickRows)
                    colInk[size_t(lx)] = 0;
                else
                    break;
            }
            for (int rx = x + run; rx < std::min(bw, x + run + tickReach); ++rx) {
                if (colInk[size_t(rx)] == 0)
                    continue;
                if (colInk[size_t(rx)] <= maxTickRows)
                    colInk[size_t(rx)] = 0;
                else
                    break;
            }
            x += run - 1;
        }
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
                        if (colInk[size_t(x + k)] < minBorderRows ||
                            !edgeHasInk(sx, rect.top) || !edgeHasInk(sx, rect.bottom - 1)) {
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
        const int plainSplitGap = std::max(17, int(std::lround(bh * 1.12f)));
        const int borderedSplitGap = std::max(10, int(std::lround(bh * .55f)));
        std::vector<std::pair<int, int>> segments;
        int segStart = -1, gapStart = -1;
        for (int x = 0; x < bw; ++x) {
            if (colInk[size_t(x)] > 0) {
                if (segStart < 0) {
                    segStart = x;
                } else if (gapStart >= 0) {
                    bool hasBorder = false;
                    for (int gx = gapStart; gx < x; ++gx)
                        if (colBorder[size_t(gx)]) {
                            hasBorder = true;
                            break;
                        }
                    const int minSplitGap = hasBorder ? borderedSplitGap : plainSplitGap;
                    if (x - gapStart >= minSplitGap && gapStart - segStart >= 4 && segments.size() + 1 < 8) {
                        segments.push_back({segStart, gapStart});
                        segStart = x;
                    }
                }
                gapStart = -1;
            } else if (segStart >= 0 && gapStart < 0) {
                gapStart = x;
            }
        }
        if (segStart >= 0) {
            const int segEnd = gapStart >= 0 ? gapStart : bw;
            if (segEnd - segStart >= 4)
                segments.push_back({segStart, segEnd});
        }
        if (segments.size() <= 1) {
            splitBoxes.push_back(rect);
        } else {
            for (size_t s = 0; s < segments.size(); ++s) {
                const int padX = std::max(2, bh / 5);
                const int leftLimit = s == 0 ? 0 : (segments[s - 1].second + segments[s].first) / 2;
                const int rightLimit =
                    s + 1 == segments.size() ? bw : (segments[s].second + segments[s + 1].first) / 2;
                const LONG subLeft = rect.left + std::max(leftLimit, segments[s].first - padX);
                const LONG subRight = rect.left + std::min(rightLimit, segments[s].second + padX);
                if (subRight - subLeft >= 4 && splitBoxes.size() < 512)
                    splitBoxes.push_back({subLeft, rect.top, subRight, rect.bottom});
            }
        }
    }
    // Split tabular rows that bridged a consistent page-wide column seam (e.g. date + title lists
    // where most rows already split at the same vertical gutter).
    if (image.bgra.size() == size_t(image.width) * image.height * 4 && splitBoxes.size() >= 6) {
        std::vector<RECT> tabSplit;
        tabSplit.reserve(splitBoxes.size());
        for (size_t i = 0; i < splitBoxes.size(); ++i) {
            const RECT rect = splitBoxes[i];
            const int bw = Width(rect), bh = Height(rect);
            if (bh < 8 || bh > 48 || bw <= bh * 4) {
                tabSplit.push_back(rect);
                continue;
            }
            int bestSeam = -1;
            int bestVotes = 0;
            for (size_t j = 0; j < splitBoxes.size(); ++j) {
                if (i == j)
                    continue;
                const RECT& b = splitBoxes[j];
                if (std::abs(b.left - rect.left) > 12 || Height(b) < bh * .55f || Height(b) > bh * 1.7f)
                    continue;
                // True left-hand date/tick columns are compact labels, never wide prose fragments.
                if (Width(b) > LONG(bh * 3.4f))
                    continue;
                if (b.right <= rect.left + LONG(bh * 1.5f) || b.right >= rect.right - LONG(bh * 2.f))
                    continue;
                const int cx = int(b.right);
                int leftVotes = 0, rightVotes = 0;
                for (size_t k = 0; k < splitBoxes.size(); ++k) {
                    if (k == i)
                        continue;
                    const RECT& o = splitBoxes[k];
                    if (Height(o) < bh * .55f || Height(o) > bh * 1.7f)
                        continue;
                    if (std::abs(o.left - rect.left) <= 12 && std::abs(o.right - cx) <= 6 &&
                        Width(o) <= LONG(bh * 3.4f))
                        ++leftVotes;
                    if (std::abs(o.left - cx) <= 6 && o.right > cx + bh)
                        ++rightVotes;
                }
                if (leftVotes >= 5 && rightVotes >= 5 && leftVotes + rightVotes > bestVotes) {
                    bestVotes = leftVotes + rightVotes;
                    bestSeam = cx;
                }
            }
            if (bestSeam < 0) {
                tabSplit.push_back(rect);
                continue;
            }
            std::array<size_t, 4096> hist{};
            for (int y = rect.top; y < rect.bottom; ++y)
                for (int x = rect.left; x < rect.right; ++x)
                    ++hist[key12(&image.bgra[(size_t(y) * image.width + x) * 4])];
            const int mode = int(std::max_element(hist.begin(), hist.end()) - hist.begin());
            const int br = ((mode >> 8) & 15) * 16 + 8;
            const int bg = ((mode >> 4) & 15) * 16 + 8;
            const int bb = (mode & 15) * 16 + 8;
            const int y0 = std::min<int>(rect.bottom - 1, rect.top + std::max(1, bh / 8));
            const int y1 = std::max<int>(y0 + 1, rect.bottom - std::max(1, bh / 8));
            const int sx0 = std::max<int>(rect.left + 6, bestSeam - 14);
            const int sx1 = std::min<int>(rect.right - 6, bestSeam + 8);
            int curStart = -1, bestStart = -1, bestLen = 0;
            for (int sx = sx0; sx <= sx1; ++sx) {
                int ink = 0;
                for (int sy = y0; sy < y1; ++sy) {
                    const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                    if (std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb) >= 56)
                        ++ink;
                }
                if (ink == 0) {
                    if (curStart < 0)
                        curStart = sx;
                    const int len = sx - curStart + 1;
                    if (len > bestLen) {
                        bestLen = len;
                        bestStart = curStart;
                    }
                } else {
                    curStart = -1;
                }
            }
            if (bestLen >= 4 && tabSplit.size() + 2 <= 512) {
                const LONG splitX = bestStart + bestLen / 2;
                tabSplit.push_back({rect.left, rect.top, splitX, rect.bottom});
                tabSplit.push_back({splitX, rect.top, rect.right, rect.bottom});
            } else {
                tabSplit.push_back(rect);
            }
        }
        splitBoxes = std::move(tabSplit);
    }
    boxes = std::move(splitBoxes);
    std::sort(boxes.begin(), boxes.end(),
              [](RECT a, RECT b) { return a.top == b.top ? a.left < b.left : a.top < b.top; });
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
bool HasCjkScript(std::string_view utf8) {
    for (size_t i = 0; i < utf8.size();) {
        const char32_t code = NextCodePoint(utf8, i);
        if ((code >= 0x3040 && code <= 0x30FF) || (code >= 0x3400 && code <= 0x4DBF) ||
            (code >= 0x4E00 && code <= 0x9FFF) || (code >= 0xAC00 && code <= 0xD7AF))
            return true;
    }
    return false;
}
int VisibleCodePointCount(std::string_view utf8) {
    int count = 0;
    for (size_t i = 0; i < utf8.size();) {
        const char32_t code = NextCodePoint(utf8, i);
        if (code == U' ' || code == U'\t' || code == U'\r' || code == U'\n' || code == 0x3000 ||
            IsCombiningMark(code))
            continue;
        ++count;
    }
    return count;
}
double EstimateTextUnits(std::string_view utf8) {
    double units = 0;
    for (size_t i = 0; i < utf8.size();) {
        const char32_t code = NextCodePoint(utf8, i);
        if (IsCombiningMark(code))
            continue;
        if ((code >= 0x2E80 && code <= 0x9FFF) || (code >= 0xAC00 && code <= 0xD7AF) ||
            (code >= 0xF900 && code <= 0xFAFF) || (code >= 0xFF00 && code <= 0xFFEF))
            units += .95;
        else if (code == U' ' || code == U'i' || code == U'l' || code == U'I' || code == U'j' ||
                 code == U't' || code == U'f' || code == U'r' || code == U'.' || code == U',' ||
                 code == U';' || code == U':' || code == U'!' || code == U'|' || code == U'\'' ||
                 code == U'`')
            units += .30;
        else if (code == U'm' || code == U'w' || code == U'M' || code == U'W')
            units += .68;
        else
            units += .50;
    }
    return units;
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
// DBNet detectors use a stride-32 FPN backbone; tight single-line or narrow-strip captures
// (e.g. 1024x33 code comments) otherwise collapse the C5 feature map to a single border-corrupted
// row and drop the middle of the text line. Pad narrow captures with the dominant background color
// so the scaled detector canvas keeps >= 128 px (4 FPN rows) and clean margin on every side.
Image PadForDetection(const Image& image, int& padX, int& padY) {
    padX = 0;
    padY = 0;
    if (image.width <= 0 || image.height <= 0 ||
        image.bgra.size() != size_t(image.width) * size_t(image.height) * 4)
        return {};
    constexpr int kMinDetectSide = 160;
    const double wideScale = std::min(1.0, 1600.0 / double(std::max(image.width + 48, image.height + 48)));
    const int targetSide = std::max(kMinDetectSide, int(std::ceil(128.0 / std::max(0.25, wideScale))));
    if (image.width >= targetSide && image.height >= targetSide)
        return {};
    padY = image.height < targetSide ? std::max(48, (targetSide - image.height + 1) / 2) : 24;
    padX = image.width < targetSide ? std::max(48, (targetSide - image.width + 1) / 2) : 24;
    std::array<size_t, 4096> hist{};
    const int step = std::max(1, int(std::sqrt(double(image.width) * double(image.height) / 65536.0)));
    for (int y = 0; y < image.height; y += step)
        for (int x = 0; x < image.width; x += step) {
            const auto* p = &image.bgra[(size_t(y) * size_t(image.width) + size_t(x)) * 4];
            ++hist[(int(p[2] >> 4) << 8) | (int(p[1] >> 4) << 4) | (int(p[0] >> 4))];
        }
    const int mode = int(std::max_element(hist.begin(), hist.end()) - hist.begin());
    const int mr = ((mode >> 8) & 15) * 16 + 8;
    const int mg = ((mode >> 4) & 15) * 16 + 8;
    const int mb = (mode & 15) * 16 + 8;
    std::array<std::array<size_t, 256>, 3> ch{};
    size_t bgCount = 0;
    for (int y = 0; y < image.height; y += step)
        for (int x = 0; x < image.width; x += step) {
            const auto* p = &image.bgra[(size_t(y) * size_t(image.width) + size_t(x)) * 4];
            if (std::abs(int(p[2]) - mr) + std::abs(int(p[1]) - mg) + std::abs(int(p[0]) - mb) < 48) {
                ++ch[0][p[0]];
                ++ch[1][p[1]];
                ++ch[2][p[2]];
                ++bgCount;
            }
        }
    unsigned char fill[4]{static_cast<unsigned char>(mb), static_cast<unsigned char>(mg),
                          static_cast<unsigned char>(mr), 255};
    if (bgCount > 0) {
        const size_t mid = (bgCount + 1) / 2;
        for (int c = 0; c < 3; ++c) {
            size_t acc = 0;
            for (int v = 0; v < 256; ++v) {
                acc += ch[c][v];
                if (acc >= mid) {
                    fill[c] = static_cast<unsigned char>(v);
                    break;
                }
            }
        }
    }
    Image padded;
    padded.width = image.width + padX * 2;
    padded.height = image.height + padY * 2;
    padded.backend = image.backend;
    padded.bgra.resize(size_t(padded.width) * size_t(padded.height) * 4);
    for (size_t i = 0; i < padded.bgra.size(); i += 4) {
        padded.bgra[i + 0] = fill[0];
        padded.bgra[i + 1] = fill[1];
        padded.bgra[i + 2] = fill[2];
        padded.bgra[i + 3] = 255;
    }
    const size_t rowBytes = size_t(image.width) * 4;
    for (int y = 0; y < image.height; ++y) {
        const auto* src = &image.bgra[size_t(y) * rowBytes];
        auto* dst = &padded.bgra[(size_t(y + padY) * size_t(padded.width) + size_t(padX)) * 4];
        std::copy_n(src, rowBytes, dst);
    }
    return padded;
}
std::vector<RECT> UnpadBoxes(std::vector<RECT> boxes, int padX, int padY, int origWidth, int origHeight) {
    if (padX <= 0 && padY <= 0)
        return boxes;
    std::vector<RECT> unpadded;
    unpadded.reserve(boxes.size());
    for (RECT b : boxes) {
        b.left = std::clamp<LONG>(b.left - padX, 0, origWidth);
        b.top = std::clamp<LONG>(b.top - padY, 0, origHeight);
        b.right = std::clamp<LONG>(b.right - padX, 0, origWidth);
        b.bottom = std::clamp<LONG>(b.bottom - padY, 0, origHeight);
        if (Width(b) >= 3 && Height(b) >= 5)
            unpadded.push_back(b);
    }
    return unpadded;
}
} // namespace
std::vector<TextLine> RecognizeBundled(const Image& image, int mode, std::stop_token stop, bool geometryOnly,
                                       bool preserveGeometry, bool retainLowConfidenceText,
                                       const std::vector<RECT>* precomputedBoxes) {
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
        std::vector<RECT> boxes;
        if (precomputedBoxes) {
            // The routing pass already detected this page; repeating the detector would add its cost to
            // every horizontal page for boxes that are byte identical.
            boxes = *precomputedBoxes;
            engine->detector.reset();
        } else {
            int padX = 0, padY = 0;
            Image padded = PadForDetection(image, padX, padY);
            const Image& detImage = padded.bgra.empty() ? image : padded;
            float scale = std::min(1.f, 1600.f / std::max(detImage.width, detImage.height));
            int w = std::max(32, int(std::round(detImage.width * scale / 32)) * 32),
                h = std::max(32, int(std::round(detImage.height * scale / 32)) * 32);
            DiagnosticSpan detection("ocr_detect", {{"width", w}, {"height", h}});
            auto input = Tensor(detImage, {0, 0, detImage.width, detImage.height}, w, h, w, true);
            auto det = engine->detector->Run(input, w, h, run);
            boxes = UnpadBoxes(Boxes(det, detImage), padX, padY, image.width, image.height);
            detection.End("ok", {{"boxes", boxes.size()}});
        }
        // Detection and recognition never need their weights or tensors concurrently.
        engine->detector.reset();
        CheckStop(stop);
        std::vector<TextLine> lines;
        if (geometryOnly) {
            for (auto box : boxes)
                lines.push_back({box, " "});
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
        auto recognize = [&](Model& model, RECT box, bool rotated, bool rtlOrder) {
            CheckStop(stop);
            const int inputHeight = model.InputHeight(48);
            const int fixedWidth = model.InputWidth();
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
                if (contentWidth > 8192)
                    return TextLine{box, " ", 0.f};
                contentWidth = std::min(4096, contentWidth);
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
            // Precompute tight ink dimensions for each box so mixed-script coverage checks measure
            // actual glyph extent rather than DB unclip padding.
            std::vector<int> boxInkWidth(boxes.size(), 1);
            std::vector<int> boxInkHeight(boxes.size(), 1);
            const bool hasPixels = image.bgra.size() == size_t(image.width) * image.height * 4;
            for (size_t bi = 0; bi < boxes.size(); ++bi) {
                const RECT r = boxes[bi];
                boxInkWidth[bi] = std::max(1, Width(r));
                boxInkHeight[bi] = std::max(1, Height(r));
                if (!hasPixels || !Valid(r))
                    continue;
                const int stepX = std::max(1, Width(r) / 160);
                std::array<size_t, 4096> hist{};
                size_t cnt = 0;
                for (int y = r.top; y < r.bottom; ++y)
                    for (int x = r.left; x < r.right; x += stepX) {
                        const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                        ++hist[(int(p[2] >> 4) << 8) | (int(p[1] >> 4) << 4) | (p[0] >> 4)];
                        ++cnt;
                    }
                if (!cnt)
                    continue;
                const int bgMode = int(std::max_element(hist.begin(), hist.end()) - hist.begin());
                const int br = ((bgMode >> 8) & 15) * 16 + 8;
                const int bg = ((bgMode >> 4) & 15) * 16 + 8;
                const int bb = (bgMode & 15) * 16 + 8;
                int minX = r.right, maxX = r.left, minY = r.bottom, maxY = r.top;
                int inkPx = 0;
                for (int y = r.top; y < r.bottom; ++y) {
                    const auto* row = &image.bgra[size_t(y) * image.width * 4];
                    for (int x = r.left; x < r.right; ++x) {
                        const auto* p = &row[size_t(x) * 4];
                        if (std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb) >= 48) {
                            minX = std::min(minX, x);
                            maxX = std::max(maxX, x + 1);
                            minY = std::min(minY, y);
                            maxY = std::max(maxY, y + 1);
                            ++inkPx;
                        }
                    }
                }
                if (inkPx >= 4 && maxX > minX && maxY > minY) {
                    boxInkWidth[bi] = std::max(maxX - minX, std::max(1, Width(r) / 3));
                    boxInkHeight[bi] = std::max(maxY - minY, std::max(1, Height(r) / 2));
                }
            }
            auto isIconBox = [](RECT r) {
                const int w = Width(r), h = Height(r);
                return (w <= int(h * 1.9f) && std::max(w, h) <= 54) || w < 18 || h < 10;
            };
            size_t nonIconCount = 0;
            for (const RECT& b : boxes)
                if (!isIconBox(b))
                    ++nonIconCount;
            auto lineCoverage = [&](const TextLine& line, size_t idx) {
                const double units = EstimateTextUnits(line.text);
                const RECT r = boxes[idx];
                if (Height(r) > Width(r) * 3 / 2)
                    return std::min(1., units * std::max(1, Width(r)) / std::max(1, Height(r)));
                const double effH = std::max(double(boxInkHeight[idx]), double(Height(r)) * .78);
                const double inkW = std::max(1., double(boxInkWidth[idx]));
                return std::min(1., units * effH / inkW);
            };
            auto quality = [&](const TextLine& line, size_t idx) {
                const double coverage = lineCoverage(line, idx);
                return float(line.confidence * (.6 + .4 * coverage));
            };
            std::vector<int> lineLanguages(boxes.size(), mode);
            bool triedRussian = false, triedGreek = false;
            auto lineNeedsRefine = [&](size_t i, int targetLang) {
                const float q = quality(lines[i], i);
                if (q < .90f)
                    return true;
                if (targetLang == OcrJapanese && lineLanguages[i] == OcrEnglish &&
                    !HasCjkScript(lines[i].text) && Width(boxes[i]) >= Height(boxes[i]) * 3 &&
                    lineCoverage(lines[i], i) < .88)
                    return true;
                if (!triedGreek && !isIconBox(boxes[i]) && HasGreekScript(lines[i].text) &&
                    lineLanguages[i] != OcrGreek)
                    return true;
                if (!triedRussian && !isIconBox(boxes[i]) && LooksLikeMisreadCyrillic(lines[i].text) &&
                    lineLanguages[i] != OcrRussian)
                    return true;
                return false;
            };
            auto acceptCandidate = [&](size_t idx, TextLine&& cand, int language) {
                const float newQ = quality(cand, idx);
                const float oldQ = quality(lines[idx], idx);
                const double oldCov = lineCoverage(lines[idx], idx);
                const double newCov = lineCoverage(cand, idx);
                const bool upgradeCjk =
                    (language == OcrJapanese || language == OcrKorean) && HasCjkScript(cand.text) &&
                    !HasCjkScript(lines[idx].text) && cand.confidence >= .70f && newCov > oldCov + .12 &&
                    (cand.confidence * (.42 + .58 * newCov) > lines[idx].confidence * (.42 + .58 * oldCov));
                const bool upgradeCyrillic =
                    language == OcrRussian && LooksLikeMisreadCyrillic(lines[idx].text) &&
                    HasCyrillicScript(cand.text) && newQ >= oldQ - .03f;
                const bool upgradeGreek =
                    language == OcrGreek && HasGreekScript(lines[idx].text) &&
                    HasGreekScript(cand.text) && newQ >= oldQ - .03f;
                if (newQ > oldQ || upgradeCjk || upgradeCyrillic || upgradeGreek) {
                    lines[idx] = std::move(cand);
                    lineLanguages[idx] = language;
                }
            };
            auto satisfied = [&] {
                double score = 0, weight = 0, lowWeight = 0;
                for (size_t i = 0; i < lines.size(); ++i) {
                    const bool skipIcon =
                        nonIconCount >= 2 &&
                        (isIconBox(boxes[i]) ||
                         (VisibleCodePointCount(lines[i].text) <= 1 && lines[i].confidence < .45f));
                    if (skipIcon)
                        continue;
                    if ((!triedGreek && HasGreekScript(lines[i].text) && lineLanguages[i] != OcrGreek) ||
                        (!triedRussian && LooksLikeMisreadCyrillic(lines[i].text) &&
                         lineLanguages[i] != OcrRussian))
                        return false;
                    const double importance = std::sqrt(double(std::max(1, Width(lines[i].rect))));
                    const float confidence = quality(lines[i], i);
                    score += confidence * importance;
                    weight += importance;
                    if (confidence < .72f)
                        lowWeight += importance;
                }
                return weight > 0. && score / weight >= .88 && lowWeight <= weight * .10;
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
                    if (mode == OcrLocalAuto && !lines.empty() && !lineNeedsRefine(i, language)) {
                        candidate.push_back(lines[i]);
                    } else if (mode == OcrLocalAuto && !lines.empty() && nonIconCount >= 2 &&
                               isIconBox(boxes[i]) && language != OcrEnglish) {
                        candidate.push_back(lines[i]);
                    } else {
                        candidate.push_back(recognize(model, boxes[i], upsideDown[i], rtl));
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
                    for (int language : {OcrEnglish, OcrJapanese}) {
                        runAll(language);
                        if (satisfied())
                            break;
                    }
                }
                if (!satisfied()) {
                    const std::array<int, 8> probeOrder{OcrKorean,  OcrRussian, OcrGreek,
                                                        OcrThai,    OcrArabic,  OcrDevanagari,
                                                        OcrEnglish, OcrLatin};
                    for (int language : probeOrder) {
                        if (!hasUnifiedModel && language == OcrEnglish)
                            continue;
                        if (std::chrono::steady_clock::now() - recognitionStart >= kAutoCascadeBudget)
                            break;
                        if ((language == OcrKorean || language == OcrEnglish)
                                ? !FindModel(root, RecognizerCandidates(language)).has_value()
                                : !HasDedicatedRecognizer(root, language))
                            continue;
                        std::vector<size_t> uncertain;
                        bool anyLatin = false;
                        size_t multiCharUncertain = 0;
                        for (size_t i = 0; i < lines.size(); ++i) {
                            if (!lineNeedsRefine(i, language))
                                continue;
                            if (nonIconCount >= 2 && isIconBox(boxes[i]))
                                continue;
                            uncertain.push_back(i);
                            anyLatin = anyLatin || HasLatinLetters(lines[i].text);
                            if (Width(boxes[i]) >= int(Height(boxes[i]) * 2.2f))
                                ++multiCharUncertain;
                        }
                        if (uncertain.empty())
                            break;
                        const bool isNonLatinProbe =
                            language != OcrEnglish && language != OcrLatin;
                        if (nonIconCount >= 2 && isNonLatinProbe && multiCharUncertain == 0)
                            continue;
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
                        // Probe up to 3 representative uncertain lines first and commit only if the
                        // script genuinely promotes on real multi-character text (preventing single-char
                        // cross-script hallucinations on icons or noisy fragments).
                        std::vector<size_t> ranked = uncertain;
                        std::stable_sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) {
                            const float qa = quality(lines[a], a), qb = quality(lines[b], b);
                            if ((qa < .80f) != (qb < .80f))
                                return qa < .80f;
                            const long long areaA = 1LL * Width(boxes[a]) * Height(boxes[a]);
                            const long long areaB = 1LL * Width(boxes[b]) * Height(boxes[b]);
                            return areaA > areaB;
                        });
                        std::vector<size_t> probes;
                        for (size_t idx : ranked) {
                            if (Width(boxes[idx]) >= int(Height(boxes[idx]) * 2.f) &&
                                Width(boxes[idx]) >= 20 && Height(boxes[idx]) >= 8)
                                probes.push_back(idx);
                            if (probes.size() == 3)
                                break;
                        }
                        if (probes.empty())
                            for (size_t k = 0; k < std::min<size_t>(3, ranked.size()); ++k)
                                probes.push_back(ranked[k]);
                        std::vector<std::pair<size_t, TextLine>> probeResults;
                        probeResults.reserve(probes.size());
                        bool promoted = false;
                        for (size_t idx : probes) {
                            const float oldQ = quality(lines[idx], idx);
                            const std::string oldText = lines[idx].text;
                            auto cand = recognize(model, boxes[idx], upsideDown[idx], rtl);
                            ++recognized;
                            const float newQ = quality(cand, idx);
                            const int cpCnt = VisibleCodePointCount(cand.text);
                            const bool multiBox =
                                Width(boxes[idx]) >= int(Height(boxes[idx]) * 2.f) && cpCnt >= 2;
                            const bool smallPageSingle = boxes.size() <= 2 && cpCnt >= 1 &&
                                                         newQ >= .82f && newQ > oldQ + .15f;
                            if ((multiBox && ((newQ >= .76f && newQ > oldQ + .06f) ||
                                              (oldQ < .58f && newQ >= .70f) ||
                                              (language == OcrRussian && LooksLikeMisreadCyrillic(oldText) &&
                                               HasCyrillicScript(cand.text) && newQ >= oldQ - .03f) ||
                                              (language == OcrGreek && HasGreekScript(oldText) &&
                                               HasGreekScript(cand.text) && newQ >= oldQ - .03f) ||
                                              (!isNonLatinProbe && newQ >= .85f && newQ > oldQ + .03f))) ||
                                smallPageSingle)
                                promoted = true;
                            probeResults.emplace_back(idx, std::move(cand));
                        }
                        if (promoted) {
                            for (auto& [idx, cand] : probeResults)
                                acceptCandidate(idx, std::move(cand), language);
                            for (size_t idx : uncertain) {
                                if (std::find(probes.begin(), probes.end(), idx) != probes.end())
                                    continue;
                                auto cand = recognize(model, boxes[idx], upsideDown[idx], rtl);
                                ++recognized;
                                acceptCandidate(idx, std::move(cand), language);
                            }
                        }
                        recognition.End("ok", {{"recognized_lines", recognized}});
                        if (satisfied())
                            break;
                    }
                }
                for (size_t i = 0; i < lines.size(); ++i)
                    lines[i].confidence = quality(lines[i], i);
                for (int language : {OcrEnglish, OcrJapanese, OcrKorean, OcrChinese, OcrLatin, OcrRussian,
                                     OcrThai, OcrArabic, OcrGreek, OcrDevanagari}) {
                    const auto count = std::count(lineLanguages.begin(), lineLanguages.end(), language);
                    if (count)
                        DiagnosticEvent("ocr_language_selected",
                                        {{"ocr_mode", language}, {"recognized_lines", count}});
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
        int padX = 0, padY = 0;
        Image padded = PadForDetection(image, padX, padY);
        const Image& detImage = padded.bgra.empty() ? image : padded;
        // Official geometry: long side 960, or the pixel budget of a 3:4 page for long strips.
        const double longSide = double(std::max(detImage.width, detImage.height));
        const double shortSide = double(std::max(1, std::min(detImage.width, detImage.height)));
        double ratio = 960.0 / longSide;
        if (longSide / shortSide > 2.0)
            ratio = std::min(1.0, std::sqrt(0.75 * 960.0 * 960.0 /
                                            (double(detImage.width) * double(detImage.height))));
        const int w = std::max(32, int(std::round(detImage.width * ratio / 32)) * 32);
        const int h = std::max(32, int(std::round(detImage.height * ratio / 32)) * 32);
        auto input = Tensor(detImage, {0, 0, detImage.width, detImage.height}, w, h, w, true);
        auto det = detector->Run(input, w, h, run);
        auto boxes = UnpadBoxes(Boxes(det, detImage, {.15f, .25f, 1.4f, 4}), padX, padY, image.width,
                                image.height);
        detection.End("ok", {{"boxes", boxes.size()}, {"width", w}, {"height", h}});
        engine->mangaDetector.reset();
        CheckStop(stop);
        std::vector<TextLine> lines;
        if (boxes.empty())
            return lines;
        auto recognition = DiagnosticSpan("ocr_recognize_manga");
        const int inputHeight = recognizer->InputHeight(48);
        const int fixedWidth = recognizer->InputWidth();
        for (const auto& box : boxes) {
            CheckStop(stop);
            // A column reads after a quarter turn, which is the orientation the weights expect.
            const bool vertical = Height(box) > Width(box);
            const int spanWidth = vertical ? Height(box) : Width(box);
            const int spanHeight = vertical ? Width(box) : Height(box);
            int contentWidth = fixedWidth;
            if (contentWidth <= 0)
                contentWidth = std::max(16, int(std::round(48.0 * spanWidth / std::max(1, spanHeight))));
            if (contentWidth > 4096)
                continue;
            auto pixels = Tensor(image, box, contentWidth, inputHeight, contentWidth, false, false, false,
                                 false, 0, vertical);
            auto output = recognizer->Run(pixels, contentWidth, inputHeight, run);
            auto [text, score] = Decode(output, recognizer->alphabet);
            lines.push_back({box, std::move(text), score});
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

