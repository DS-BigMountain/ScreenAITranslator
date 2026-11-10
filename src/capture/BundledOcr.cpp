#include "BundledOcr.h"
#include "common/Diagnostics.h"
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <array>
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
struct ModelFile {
    std::filesystem::path path;
    const char* variant;
};
ModelFile PickModel(const std::filesystem::path& root,
                    std::initializer_list<std::pair<const wchar_t*, const char*>> candidates) {
    std::error_code error;
    for (const auto& [name, variant] : candidates) {
        const auto path = root / name;
        if (std::filesystem::is_regular_file(path, error))
            return {path, variant};
    }
    throw AppError("ocr", "Bundled OCR model unavailable");
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
            ModelFile picked;
            if (mode == OcrEnglish)
                picked = PickModel(root, {{L"models/rec-en-server.onnx", "server"},
                                          {L"models/rec-en.onnx", "mobile"}});
            else if (mode == OcrKorean)
                picked = PickModel(root, {{L"models/rec-ko-server.onnx", "server"},
                                          {L"models/rec-ko.onnx", "mobile"}});
            else
                picked = PickModel(root, {{L"models/rec-ja-server.onnx", "server"},
                                          {L"models/rec-ja.onnx", "mobile"}});
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
    float minAverage{.55f};
    float unclip{1.6f};
    size_t minPixels{6};
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
        // A tall box is either a vertical column or a rotated line. Both reach recognition; the
        // recognizer decides which reading the crop supports.
        boxes.push_back(rect);
    }
    // Recognition expansion can touch adjacent baselines. Split the shared margin at
    // their centre midpoint so a crop cannot import ascenders/descenders from its neighbour.
    const auto expanded = boxes;
    for (size_t i = 0; i < boxes.size(); ++i)
        for (size_t j = 0; j < boxes.size(); ++j) {
            if (i == j)
                continue;
            auto a = expanded[i], b = expanded[j];
            int overlap = std::min(a.right, b.right) - std::max(a.left, b.left);
            int ca = (a.top + a.bottom) / 2, cb = (b.top + b.bottom) / 2, delta = cb - ca;
            if (overlap < std::min(Width(a), Width(b)) * .5f ||
                std::abs(delta) < std::min(Height(a), Height(b)) * .5f)
                continue;
            if (a.bottom > b.top && b.bottom > a.top) {
                int split = (ca + cb) / 2;
                if (delta > 0)
                    boxes[i].bottom = std::min<LONG>(boxes[i].bottom, split);
                else
                    boxes[i].top = std::max<LONG>(boxes[i].top, split);
            }
        }
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
// Text-line orientation is a 0/180 classification. RapidOCR applies cls_thresh=0.9,
// so a weak 180-degree preference keeps the crop as detected.
float UpsideDownScore(const float* data) {
    const float margin = data[0] - data[1];
    if (margin > 30.f)
        return 0.f;
    return 1.f / (1.f + std::exp(margin));
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
    std::atomic<bool> timedOut = false;
    std::mutex timerMutex;
    std::condition_variable_any timerCv;
    std::jthread watchdog([&](std::stop_token done) {
        std::unique_lock timerLock(timerMutex);
        timerCv.wait_for(timerLock, done, std::chrono::seconds(15), [] { return false; });
        if (!done.stop_requested()) {
            timedOut = true;
            run.SetTerminate();
        }
    });
    try {
        float scale = std::min(1.f, 1280.f / std::max(image.width, image.height));
        int w = std::max(32, int(std::round(image.width * scale / 32)) * 32),
            h = std::max(32, int(std::round(image.height * scale / 32)) * 32);
        std::vector<RECT> boxes;
        if (precomputedBoxes) {
            // The routing pass already detected this page; repeating the detector would add its cost to
            // every horizontal page for boxes that are byte identical.
            boxes = *precomputedBoxes;
            engine->detector.reset();
        } else {
            DiagnosticSpan detection("ocr_detect", {{"width", w}, {"height", h}});
            auto input = Tensor(image, {0, 0, image.width, image.height}, w, h, w, true);
            auto det = engine->detector->Run(input, w, h, run);
            boxes = Boxes(det, image);
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
        auto recognize = [&](Model& model, RECT box, bool rotated) {
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
                if (contentWidth > 4096)
                    return TextLine{box, " ", 0.f};
                const int width = fixedWidth > 0 ? fixedWidth : std::max(320, contentWidth);
                auto pixels = Tensor(image, box, width, inputHeight, contentWidth, false,
                                     rotated && !vertical && !quarter, vertical, quarter, blocks);
                auto output = model.Run(pixels, width, inputHeight, run);
                auto [text, score] = Decode(output, model.alphabet);
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
                for (unsigned char ch : line.text)
                    if ((ch & 0xc0) != 0x80)
                        units += ch < 128 ? .55 : .95;
                const double coverage =
                    std::min(1., units * Height(line.rect) / std::max(1, Width(line.rect)));
                return float(line.confidence * (.6 + .4 * coverage));
            };
            std::vector<int> lineLanguages;
            for (int language : {OcrEnglish, OcrJapanese, OcrKorean}) {
                if (mode != OcrLocalAuto && language != mode)
                    continue;
                DiagnosticSpan recognitionLoad("ocr_recognizer_load", {{"ocr_mode", language}});
                auto& model = engine->Recognizer(language);
                recognitionLoad.End("ok", {{"model", engine->recognizerVariant}});
                DiagnosticSpan recognition("ocr_recognize", {{"ocr_mode", language}});
                std::vector<TextLine> candidate;
                size_t recognized = 0;
                for (size_t i = 0; i < boxes.size(); ++i) {
                    if (mode == OcrLocalAuto && !lines.empty() && quality(lines[i]) >= .90f) {
                        candidate.push_back(lines[i]);
                    } else {
                        candidate.push_back(recognize(model, boxes[i], upsideDown[i]));
                        ++recognized;
                    }
                }
                recognition.End("ok", {{"recognized_lines", recognized}});
                if (lines.empty()) {
                    lineLanguages.assign(candidate.size(), language);
                    lines = std::move(candidate);
                } else {
                    // Compare the same detected line across recognizers. A small Japanese
                    // label must not be outvoted by a page containing mostly English.
                    for (size_t i = 0; i < lines.size(); ++i)
                        if (quality(candidate[i]) > quality(lines[i])) {
                            lines[i] = std::move(candidate[i]);
                            lineLanguages[i] = language;
                        }
                }
                double score = 0, weight = 0;
                float minimum = 1.f;
                for (const auto& line : lines) {
                    const double importance = std::sqrt(double(std::max(1, Width(line.rect))));
                    const float confidence = quality(line);
                    score += confidence * importance;
                    weight += importance;
                    minimum = std::min(minimum, confidence);
                }
                if (mode != OcrLocalAuto || (score / std::max(1., weight) >= .90 && minimum >= .80f))
                    break;
            }
            if (mode == OcrLocalAuto) {
                for (auto& line : lines)
                    line.confidence = quality(line);
                for (int language : {OcrEnglish, OcrJapanese, OcrKorean}) {
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
                            {{"reason", timedOut ? "recognition_timeout" : "recognition_failed"},
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
        if (timedOut)
            throw AppError("ocr", "OCR time limit exceeded");
        CheckStop(stop);
        // Low confidence must not silently constrain vision translation to incomplete OCR IDs.
        if (!preserveGeometry && rejected > std::max<size_t>(2, boxes.size() / 5))
            throw AppError("ocr", "OCR confidence insufficient");
        return lines;
    } catch (const Ort::Exception&) {
        CheckStop(stop);
        DiagnosticEvent("ocr_error", {{"reason", timedOut ? "timeout" : "inference_failed"}});
        throw AppError("ocr", timedOut ? "OCR time limit exceeded" : "Bundled OCR inference failed");
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
        auto boxes = Boxes(det, image, {.15f, .25f, 1.4f, 4});
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
        return lines;
    } catch (const Ort::Exception&) {
        detection.Fail(AppError("ocr", "Manga OCR inference failed"));
        CheckStop(stop);
        throw AppError("ocr", "Manga OCR inference failed");
    }
}
} // namespace sat

