#include "TextDetection.h"
#include "Capture.h"
#include "common/Platform.h"
#include "common/Diagnostics.h"
#include "BundledOcr.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

namespace sat {
namespace {
RECT TightGlyphBounds(RECT rect, const Image& image) {
    // DB expands a shrunken mask for recognition. Recover glyph height on flat backgrounds
    // before grouping; expanded recognition crops must not be mistaken for line spacing.
    std::array<size_t, 32768> colors{};
    size_t count = 0;
    auto key = [](const unsigned char* p) {
        return (int(p[0] >> 3) << 10) | (int(p[1] >> 3) << 5) | (p[2] >> 3);
    };
    for (int y = rect.top; y < rect.bottom; ++y)
        for (int x = rect.left; x < rect.right; ++x) {
            ++colors[key(&image.bgra[(size_t(y) * image.width + x) * 4])];
            ++count;
        }
    auto mode = std::max_element(colors.begin(), colors.end());
    if (!count)
        return rect;
    // Shaded artwork spreads its backdrop across neighbouring bins, so one mode bin can cover less
    // than half of a gradient band. The bin still marks the surface wherever glyphs leave it visible;
    // the share guard at the end rejects a measurement that cannot separate ink from backdrop.
    int index = int(mode - colors.begin()), b = ((index >> 10) & 31) * 8 + 4, g = ((index >> 5) & 31) * 8 + 4,
        r = (index & 31) * 8 + 4;
    RECT ink{rect.right, rect.bottom, rect.left, rect.top};
    for (int y = rect.top; y < rect.bottom; ++y) {
        int pixels = 0, l = rect.right, rr = rect.left;
        for (int x = rect.left; x < rect.right; ++x) {
            auto p = &image.bgra[(size_t(y) * image.width + x) * 4];
            if (std::abs(int(p[0]) - b) + std::abs(int(p[1]) - g) + std::abs(int(p[2]) - r) > 150) {
                ++pixels;
                l = std::min(l, x);
                rr = std::max(rr, x + 1);
            }
        }
        if (pixels >= std::max(2, Width(rect) / 500)) {
            ink.left = std::min<LONG>(ink.left, l);
            ink.right = std::max<LONG>(ink.right, rr);
            ink.top = std::min<LONG>(ink.top, y);
            ink.bottom = y + 1;
        }
    }
    if (!Valid(ink) || Height(ink) < 3)
        return rect;
    // Only the vertical extent is recovered here. The horizontal extent belongs to the detector: a
    // narrowed box would leave the translation less width than the source text occupied.
    ink.left = rect.left;
    ink.right = rect.right;
    // A dense heading fills most of its box with glyph pixels, and the single mode then marks the
    // heading colour as the backdrop, which inverts the measurement. Only a tightening that keeps a
    // plausible ink share counts; everything else keeps the detector rectangle.
    const float heightShare = float(Height(ink)) / float(Height(rect));
    if (heightShare > .92f || heightShare < .3f)
        return rect;
    return ink;
}
// A decorated line opens a passage of its own. Bullets, note markers and banner glyphs separate
// sections in UI copy and article pages, and merging across them flattens the page structure into a
// single block that no translation can lay out faithfully. Only full-width decorations qualify:
// ASCII markers also appear inside ordinary sentences, where they start no passage.
bool StartsPassage(const std::string& text) {
    const auto wide = Wide(text);
    const auto first = wide.find_first_not_of(L" \t");
    if (first == std::wstring::npos)
        return false;
    return std::wstring_view(L"◆◇●○◎■□▲△※").find(wide[first]) != std::wstring_view::npos;
}
bool HasSeparator(const Image& image, RECT a, RECT b) {    const int left = std::max(a.left, b.left), right = std::min(a.right, b.right);
    if (right - left < 20)
        return false;
    for (int y = a.bottom + 1; y < b.top - 1; ++y) {
        int edges = 0;
        for (int x = left; x < right; ++x) {
            const auto p = (size_t(y) * image.width + x) * 4;
            int delta = 0;
            for (int c = 0; c < 3; ++c)
                delta += std::abs(int(image.bgra[p + c]) - int(image.bgra[p - image.width * 4 + c]));
            if (delta > 24)
                ++edges;
        }
        if (edges > .65 * (right - left))
            return true;
    }
    return false;
}
} // namespace
std::vector<TextRegion> GroupTextLines(std::vector<TextLine> lines, const Image& image) {
    if (image.width <= 0 || image.height <= 0 || image.bgra.size() != size_t(image.width) * image.height * 4)
        return {};
    const RECT bounds{0, 0, image.width, image.height};
    for (auto& line : lines) {
        RECT clipped{};
        IntersectRect(&clipped, &line.rect, &bounds);
        line.rect = clipped;
    }
    std::erase_if(lines, [](const TextLine& l) { return !Valid(l.rect) || l.text.empty(); });
    // A detector may also return a small icon inside an expanded text line. It
    // must not become a second protected region that hides the readable passage.
    for (size_t i = 0; i < lines.size();) {
        bool redundant = false;
        for (size_t j = 0; j < lines.size(); ++j) {
            if (i == j || lines[j].confidence < .65f || lines[i].confidence >= .65f)
                continue;
            RECT overlap{};
            const auto a = lines[i].rect, b = lines[j].rect;
            const bool unread = lines[i].text.find_first_not_of(" \t\r\n") == std::string::npos;
            if (IntersectRect(&overlap, &a, &b) &&
                Width(overlap) * Height(overlap) >= (unread ? .45f : .65f) * Width(a) * Height(a) &&
                Width(a) < Width(b) * .4f) {
                redundant = true;
                break;
            }
        }
        if (redundant)
            lines.erase(lines.begin() + i);
        else
            ++i;
    }
    // A list marker and its text can be detected as two slightly overlapping boxes, and a short
    // caption word can arrive as several fragments ("Angelic Bazaar" as A / n g e / lic / Bazaar).
    // Join boxes that share one baseline and sit within reach of each other; columns stay apart
    // because the horizontal gap of a real column break exceeds one and a half line heights.
    for (size_t i = 0; i < lines.size(); ++i)
        for (size_t j = i + 1; j < lines.size();) {
            auto a = lines[i].rect, b = lines[j].rect;
            // A vertical column is its own reading unit: joining columns side by side would splice
            // separate lines of dialogue together, and same-column rows are joined by the passage
            // pass below.
            if (Height(a) > Width(a) * 3 / 2 || Height(b) > Width(b) * 3 / 2) {
                ++j;
                continue;
            }
            const LONG lineHeight = std::min(Height(a), Height(b));
            const LONG centreA = (a.top + a.bottom) / 2, centreB = (b.top + b.bottom) / 2;
            RECT overlap{};
            const bool touching = IntersectRect(&overlap, &a, &b) && Height(overlap) >= .75f * lineHeight &&
                                  Width(overlap) < .5f * std::min(Width(a), Width(b));
            const LONG gap = std::max(a.left, b.left) - std::min(a.right, b.right);
            // Fragments of one caption sit within a line height of each other. An independent badge
            // keeps a wider gutter, so one line height separates the two cases.
            const bool adjacent = std::abs(centreA - centreB) <= lineHeight / 2 &&
                                  gap <= LONG(lineHeight) && gap >= -std::max<LONG>(4, lineHeight / 4);
            if (touching || adjacent) {
                lines[i].text = a.left < b.left ? lines[i].text + " " + lines[j].text
                                                : lines[j].text + " " + lines[i].text;
                lines[i].confidence = std::min(lines[i].confidence, lines[j].confidence);
                UnionRect(&lines[i].rect, &a, &b);
                lines.erase(lines.begin() + j);
            } else
                ++j;
        }
    // Reading order follows the page shape. A page of vertical columns reads right to left, each
    // column top to bottom; a horizontal page reads top to bottom, left to right.
    const bool columnPage =
        std::count_if(lines.begin(), lines.end(),
                      [](const auto& line) { return Height(line.rect) > Width(line.rect); }) *
            2 >
        int(lines.size());
    if (columnPage)
        std::stable_sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) {
            const int tolerance = std::max(8, int(std::min(Width(a.rect), Height(a.rect)) / 2));
            if (std::abs(a.rect.left - b.rect.left) > tolerance)
                return a.rect.right > b.rect.right;
            return a.rect.top < b.rect.top;
        });
    else
        std::stable_sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) {
            return a.rect.top == b.rect.top ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
        });
    std::vector<TextRegion> regions;
    for (const auto& line : lines) {
        TextRegion* best = nullptr;
        int bestGap = INT_MAX;
        // A decorated line starts a new passage; it never continues the block above it.
        const bool opensPassage = StartsPassage(line.text);
        for (auto& region : regions) {
            if (opensPassage)
                continue;
            const auto last = region.lines.back();
            const int gap = line.rect.top - last.bottom;
            const float h = region.lineHeight;
            const int overlap = std::min(last.right, line.rect.right) - std::max(last.left, line.rect.left);
            if (gap < -h * .25f || gap > h * .9f || gap >= bestGap || Height(line.rect) < h * .65f ||
                Height(line.rect) > h * 1.45f)
                continue;
            if ((std::abs(line.rect.left - last.left) > h * 2 &&
                 std::abs(line.rect.right - last.right) > h) ||
                overlap < .65f * std::min(Width(last), Width(line.rect)))
                continue;
            if (region.text.size() + line.text.size() + 1 > 8192 || HasSeparator(image, last, line.rect))
                continue;
            RECT joined{};
            UnionRect(&joined, &region.rect, &line.rect);
            bool crosses = false;
            for (const auto& other : lines) {
                if (&other == &line || std::any_of(region.lines.begin(), region.lines.end(),
                                                   [&](RECT r) { return EqualRect(&r, &other.rect); }))
                    continue;
                RECT intersection{};
                if (IntersectRect(&intersection, &joined, &other.rect) &&
                    Height(intersection) > std::min(Height(other.rect), Height(line.rect)) * .25f) {
                    crosses = true;
                    break;
                }
            }
            if (crosses)
                continue;
            best = &region;
            bestGap = gap;
        }
        if (!best) {
            regions.push_back(
                {0, line.rect, line.text, {line.rect}, float(Height(line.rect)), {}, line.confidence});
            continue;
        }
        best->confidence = std::min(best->confidence, line.confidence);
        best->text += '\n';
        best->text += line.text;
        best->lines.push_back(line.rect);
        RECT merged{};
        UnionRect(&merged, &best->rect, &line.rect);
        best->rect = merged;
        std::vector<int> heights;
        for (auto r : best->lines)
            heights.push_back(Height(r));
        std::sort(heights.begin(), heights.end());
        best->lineHeight = float(heights[heights.size() / 2]);
    }
    // One balloon wraps its sentence across neighbouring strips, and a strip on its own carries only a
    // fragment. Joining the strips of one balloon is what lets the translation read as one spoken line
    // instead of a row of separate words. Every strip rectangle survives in the region, so the glyph
    // size stays measurable from a single strip width.
    if (columnPage) {
        const auto strips = [](const TextRegion& region) {
            return !region.lines.empty() &&
                   std::all_of(region.lines.begin(), region.lines.end(), [](RECT line) {
                       return Width(line) > 0 && Height(line) * 5 > Width(line) * 6;
                   });
        };
        for (size_t i = 0; i < regions.size(); ++i)
            for (size_t j = i + 1; j < regions.size();) {
                auto& keep = regions[i];
                const auto& join = regions[j];
                const float strip = float(std::max(1, Width(keep.lines.back())));
                const LONG overlap =
                    std::min(keep.rect.bottom, join.rect.bottom) - std::max(keep.rect.top, join.rect.top);
                const LONG spacing = keep.rect.left - join.rect.right;
                // Neighbouring strips of one balloon share most of their glyph run and sit about one
                // strip width apart. Separate balloons keep a wider gutter or a different vertical span.
                if (!strips(keep) || !strips(join) || spacing < -LONG(strip * .5f) ||
                    spacing > LONG(strip * 1.9f) ||
                    overlap * 100 <= std::min(Height(keep.rect), Height(join.rect)) * 45) {
                    ++j;
                    continue;
                }
                keep.text += ' ';
                keep.text += join.text;
                keep.lines.insert(keep.lines.end(), join.lines.begin(), join.lines.end());
                RECT merged{};
                UnionRect(&merged, &keep.rect, &join.rect);
                keep.rect = merged;
                keep.confidence = std::min(keep.confidence, join.confidence);
                regions.erase(regions.begin() + j);
            }
        // A strip is one glyph wide, so the column width is the glyph size the layout must follow. The
        // column height measures the whole run instead and would inflate the face far past the balloon.
        for (auto& region : regions) {
            if (!strips(region))
                continue;
            std::vector<int> widths;
            for (auto line : region.lines)
                widths.push_back(Width(line));
            std::sort(widths.begin(), widths.end());
            region.lineHeight = float(widths[widths.size() / 2]);
        }
    }
    for (size_t i = 0; i < regions.size(); ++i)
        regions[i].id = int(i + 1);
    return regions;
}
static TextDetection DetectLocal(const Image& image, std::stop_token stop, int mode, bool geometryOnly,
                                 bool preserveGeometry = false, bool retainLowConfidenceText = false,
                                 bool useManga = false, const std::vector<RECT>* precomputedBoxes = nullptr) {
    CheckStop(stop);
    if (image.width <= 0 || image.height <= 0 || image.bgra.size() != size_t(image.width) * image.height * 4)
        return {{}, "invalid-image"};
    if (!ValidOcrMode(mode))
        return {{}, "invalid-mode"};
    if (!geometryOnly && (mode == OcrAuto || mode == OcrDirectVision))
        return {{}, "direct-ai"};
    try {
        // The manga engine carries its own detector and recognizer; both routes share the geometry
        // clean-up and region grouping below so downstream code sees one format.
        auto lines = useManga ? RecognizeMangaBundled(image, stop)
                              : RecognizeBundled(image, mode, stop, geometryOnly, preserveGeometry,
                                                 retainLowConfidenceText, precomputedBoxes);
        const auto unrecognizedLines = std::count_if(lines.begin(), lines.end(), [](const auto& line) {
            return line.text.find_first_not_of(" \t\r\n") == std::string::npos;
        });
        for (auto& line : lines) {
            line.rect = TightGlyphBounds(line.rect, image);
            line.rect.left = std::max(0L, line.rect.left - 1);
            line.rect.top = std::max(0L, line.rect.top - 1);
            line.rect.right = std::min<LONG>(image.width, line.rect.right + 1);
            line.rect.bottom = std::min<LONG>(image.height, line.rect.bottom + 1);
        }
        auto regions = GroupTextLines(std::move(lines), image);
        size_t textBytes = 0;
        for (const auto& r : regions)
            textBytes += r.text.size();
        if (regions.size() > 256 || textBytes > 128 * 1024) {
            DiagnosticEvent("ocr_fallback", {{"reason", "too_many_regions"}, {"regions", regions.size()}});
            return {{}, "too-many-regions"};
        }
        if (regions.empty())
            return {{}, "no-text"};
        for (auto& r : regions) {
            if (geometryOnly || r.text.find_first_not_of(" \t\r\n") == std::string::npos)
                r.text.clear();
            int x = std::min(999, MulDiv(r.rect.left, 1000, image.width)),
                y = std::min(999, MulDiv(r.rect.top, 1000, image.height));
            r.imageBox = {x, y, std::max(1, MulDiv(r.rect.right, 1000, image.width) - x),
                          std::max(1, MulDiv(r.rect.bottom, 1000, image.height) - y)};
        }
        const bool onlyGeometry =
            std::all_of(regions.begin(), regions.end(), [](const auto& r) { return r.text.empty(); });
        return {std::move(regions), geometryOnly || onlyGeometry ? "local-geometry-ai" : "bundled-ocr",
                size_t(unrecognizedLines)};
    } catch (const Cancelled&) {
        throw;
    } catch (const AppError& e) {
        CheckStop(stop);
        DiagnosticSpan failure("ocr_fallback");
        failure.Fail(e);
        return {{}, "ocr-unavailable"};
    } catch (const std::exception&) {
        CheckStop(stop);
        DiagnosticEvent("ocr_fallback", {{"reason", "ocr_unavailable"}});
        return {{}, "ocr-unavailable"};
    }
}
TextDetection DetectText(const Image& image, std::stop_token stop, int mode) {
    return DetectLocal(image, stop, mode, false);
}
TextDetection DetectTextGeometry(const Image& image, std::stop_token stop) {
    return DetectLocal(image, stop, 0, true);
}
TextDetection DetectTextForTranslation(const Image& image, std::stop_token stop, int mode) {
    return DetectLocal(image, stop, mode, mode == OcrAuto || mode == OcrDirectVision, true);
}
TextDetection DetectOcrText(const Image& image, std::stop_token stop, int language) {
    if (language != OcrLocalAuto && language != OcrEnglish && language != OcrJapanese && language != OcrKorean)
        throw AppError("ocr", "OCR 文字翻译需要选择本地自动识别、英语、日语或韩语");
    // The page shape decides the engine, and the detected boxes already describe it. A full general pass
    // recognizes every line three times over, so starting there costs seconds of work that a manga page
    // discards. Detection first keeps the spent time inside the engine that actually reads the page, and
    // the boxes travel on so the routed pass repeats no detector run.
    auto shapeLines = RecognizeBundled(image, language, stop, true);
    std::vector<RECT> detectedBoxes;
    detectedBoxes.reserve(shapeLines.size());
    for (const auto& line : shapeLines)
        detectedBoxes.push_back(line.rect);
    // Routing follows the measured page shape: the share of vertical columns decides the engine without a
    // second detection budget. A page of columns goes to the manga engine, whose recognizer reads
    // quarter-turned columns and keeps kana and rare kanji apart where the general engine substitutes.
    size_t vertical = 0;
    for (auto box : detectedBoxes)
        if (Height(box) * 3 > Width(box) * 2)
            ++vertical;
    const double share = detectedBoxes.empty() ? 0.0 : double(vertical) / double(detectedBoxes.size());
    const bool mangaCapable = language == OcrLocalAuto || language == OcrJapanese;
    if (detectedBoxes.empty() || share < .3 || !mangaCapable || !MangaOcrAvailable()) {
        DiagnosticEvent("ocr_route", {{"engine", "general"},
                                      {"reason", detectedBoxes.empty() ? "no_regions"
                                                 : share < .3          ? "horizontal"
                                                 : !mangaCapable       ? "language"
                                                                       : "manga_unavailable"},
                                      {"vertical_share", share},
                                      {"regions", detectedBoxes.size()}});
        return DetectLocal(image, stop, language, false, true, true, false,
                           detectedBoxes.empty() ? nullptr : &detectedBoxes);
    }
    CheckStop(stop);
    DiagnosticSpan manga("ocr_manga");
    auto located = DetectLocal(image, stop, language, false, true, true, true);
    if (located.regions.empty()) {
        manga.End("ok", {{"regions", 0}});
        DiagnosticEvent("ocr_route",
                        {{"engine", "general"}, {"reason", "manga_empty"}, {"vertical_share", share}});
        return DetectLocal(image, stop, language, false, true, true, false, &detectedBoxes);
    }
    manga.End("ok", {{"regions", located.regions.size()}});
    DiagnosticEvent("ocr_route", {{"engine", "manga"},
                                  {"reason", "vertical_columns"},
                                  {"vertical_share", share},
                                  {"regions", located.regions.size()}});
    return located;
}

} // namespace sat
