#include "SpatialLayout.h"
#include "TextRim.h"
#include "capture/Capture.h"
#include <dwrite_1.h>
#include <algorithm>
#include <cmath>
#include <array>
#include <string_view>
namespace sat {
namespace {
// Reference product measurement: a replacement may run past its source box by up to 31 px. A Chinese
// label is regularly wider than its English original, and shrinking the face to the letter would cost
// more readability than the overhang costs. The pipeline keeps that allowance inside the 24 px repair
// ring the rebuilt surface already touches, so the overhang never edits untouched artwork. The fit
// search, the overflow flag and the drawing clip all read this one bound so they never disagree.
constexpr float kOverhangAllowance = 24.f;
RECT MapBox(const Box& b, const Image& image) {
    const int x = std::clamp(b.x, 0, 999), y = std::clamp(b.y, 0, 999);
    const int right =
        int(std::clamp(static_cast<long long>(b.x) + b.width, static_cast<long long>(x + 1), 1000LL));
    const int bottom =
        int(std::clamp(static_cast<long long>(b.y) + b.height, static_cast<long long>(y + 1), 1000LL));
    RECT r{MulDiv(x, image.width, 1000), MulDiv(y, image.height, 1000), MulDiv(right, image.width, 1000),
           MulDiv(bottom, image.height, 1000)};
    r.left = std::min<LONG>(r.left, image.width - 1);
    r.top = std::min<LONG>(r.top, image.height - 1);
    r.right = std::min<LONG>(image.width, std::max(r.left + 1, r.right));
    r.bottom = std::min<LONG>(image.height, std::max(r.top + 1, r.bottom));
    return r;
}
// Adjacent passages share borders after the detector tightens and pads them, and a multi-row passage
// overlaps its neighbour rows by design. Only a passage that covers most of another one describes the
// same surface twice, and only that warrants dropping one of them.
bool Conflicts(RECT a, RECT b) {
    RECT r{};
    if (!IntersectRect(&r, &a, &b))
        return false;
    const long long shared = static_cast<long long>(Width(r)) * Height(r);
    const long long smallest = std::min(static_cast<long long>(Width(a)) * Height(a),
                                        static_cast<long long>(Width(b)) * Height(b));
    return smallest > 0 && shared * 2 > smallest;
}
struct PixelText {
    std::optional<COLORREF> color;
    std::optional<COLORREF> background;
    std::vector<RECT> lines;
    float lineHeight{};
    // Height of the dense ink core of a line. Ascenders, descenders and neighbouring icons inflate the
    // full band, while the core tracks the lowercase body that dominates a line of running text.
    float xHeight{};
};
PixelText InspectTextPixels(const Image& image, RECT box) {
    PixelText result;
    if (!Valid(box))
        return result;
    std::array<size_t, 4096> histogram{};
    auto key = [](const unsigned char* p) {
        return (int(p[2] >> 4) << 8) | (int(p[1] >> 4) << 4) | (p[0] >> 4);
    };
    const int step = std::max(1, int(std::sqrt(double(Width(box)) * Height(box) / 500000.)));
    size_t sampled = 0;
    for (int y = box.top; y < box.bottom; y += step)
        for (int x = box.left; x < box.right; x += step) {
            ++histogram[key(&image.bgra[(size_t(y) * image.width + x) * 4])];
            ++sampled;
        }
    auto mode = std::max_element(histogram.begin(), histogram.end());
    if (!sampled)
        return result;
    int background = int(mode - histogram.begin());
    int br = ((background >> 8) & 15) * 16 + 8, bg = ((background >> 4) & 15) * 16 + 8,
        bb = (background & 15) * 16 + 8;
    // Preserve the actual surface color, including white (255) and colored message bubbles.
    // The exterior ring can cross a bubble edge or include neighboring text.
    // Compression and gradients can split one surface across adjacent histogram bins.
    std::array<std::array<size_t, 256>, 3> backgroundChannels{};
    size_t backgroundCount = 0;
    for (int y = box.top; y < box.bottom; y += step)
        for (int x = box.left; x < box.right; x += step) {
            const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
            if (std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb) < 48) {
                for (int c = 0; c < 3; ++c)
                    ++backgroundChannels[c][p[c]];
                ++backgroundCount;
            }
        }
    // A shaded button, a gradient banner or a painted game surface spreads its backdrop across many
    // bins, so the single mode covers less than the threshold. Row geometry stays measurable on that
    // artwork; only the surface colour hint drops out, and the caller keeps its sampled surface.
    const bool reliableBackdrop = backgroundCount >= sampled * .55;
    if (reliableBackdrop) {
        std::array<int, 3> backgroundMedian{};
        for (int c = 0; c < 3; ++c) {
            size_t accumulated = 0;
            for (int value = 0; value < 256; ++value) {
                accumulated += backgroundChannels[c][value];
                if (accumulated >= (backgroundCount + 1) / 2) {
                    backgroundMedian[c] = value;
                    break;
                }
            }
        }
        result.background = RGB(backgroundMedian[2], backgroundMedian[1], backgroundMedian[0]);
    }
    std::array<size_t, 4096> counts{}, rs{}, gs{}, bs{};
    size_t foreground = 0;
    std::vector<RECT> rows;
    int activeTop = -1, left = box.right, right = box.left, lastRow = -1;
    const int boxWidth = int(Width(box)), boxHeight = int(Height(box));
    std::vector<unsigned char> inkMark(size_t(boxWidth) * size_t(boxHeight), 0);
    auto flush = [&] {
        if (activeTop >= 0) {
            if (lastRow - activeTop >= 2)
                rows.push_back({left, activeTop, right, lastRow + 1});
            activeTop = -1;
            left = box.right;
            right = box.left;
        }
    };
    for (int y = box.top; y < box.bottom; ++y) {
        int rowCount = 0, rowLeft = box.right, rowRight = box.left;
        // Gradient and shaded artwork shift the backdrop across a band, so one global mode leaves the
        // row detection blind on exactly the surfaces this pass exists for. Each row measures its own
        // median backdrop there. Flat artwork keeps the global mode, which a dense bold row would
        // otherwise invert when the glyphs outnumber the surface pixels.
        int rowRed = br, rowGreen = bg, rowBlue = bb;
        if (!reliableBackdrop && boxWidth > 0) {
            std::vector<int> sample[3];
            const int stride = std::max(1, boxWidth / 64);
            for (int x = box.left; x < box.right; x += stride) {
                const auto* q = &image.bgra[(size_t(y) * image.width + x) * 4];
                sample[0].push_back(q[2]);
                sample[1].push_back(q[1]);
                sample[2].push_back(q[0]);
            }
            if (!sample[0].empty()) {
                const size_t middle = sample[0].size() / 2;
                for (int c = 0; c < 3; ++c)
                    std::nth_element(sample[c].begin(), sample[c].begin() + middle, sample[c].end());
                rowRed = sample[0][middle];
                rowGreen = sample[1][middle];
                rowBlue = sample[2][middle];
            }
        }
        for (int x = box.left; x < box.right; ++x) {
            auto p = &image.bgra[(size_t(y) * image.width + x) * 4];
            int distance =
                std::abs(int(p[2]) - rowRed) + std::abs(int(p[1]) - rowGreen) + std::abs(int(p[0]) - rowBlue);
            if (distance < 65)
                continue;
            auto k = key(p);
            ++counts[k];
            rs[k] += p[2];
            gs[k] += p[1];
            bs[k] += p[0];
            ++foreground;
            ++rowCount;
            rowLeft = std::min(rowLeft, x);
            rowRight = x + 1;
            if (boxWidth > 0 && boxHeight > 0)
                inkMark[size_t(y - box.top) * size_t(boxWidth) + size_t(x - box.left)] = 1;
        }
        // Broad filled bars and separators are not text baselines.
        if (rowCount >= 2 && rowCount < Width(box) * .85) {
            if (activeTop < 0)
                activeTop = y;
            lastRow = y;
            left = std::min(left, rowLeft);
            right = std::max(right, rowRight);
        } else if (activeTop >= 0 && y - lastRow > 1)
            flush();
    }
    flush();
    // The dense core of each line is the lowercase body: rows within half the peak ink density. The
    // column projection first isolates the run of text from icons and list markers, because a narrow
    // glyph-shaped element next to the line would otherwise hold the core open to its full height.
    if (boxWidth > 0 && boxHeight > 0) {
        std::vector<float> cores;
        std::vector<int> columnInk(size_t(boxWidth), 0);
        std::vector<int> lineInk;
        for (const auto& line : rows) {
            const int top = int(std::max<LONG>(0, line.top - box.top));
            const int bottom = int(std::min<LONG>(LONG(boxHeight), line.bottom - box.top));
            if (bottom - top < 3)
                continue;
            std::fill(columnInk.begin(), columnInk.end(), 0);
            for (int y = top; y < bottom; ++y)
                for (int x = 0; x < boxWidth; ++x)
                    columnInk[size_t(x)] += inkMark[size_t(y) * size_t(boxWidth) + size_t(x)];
            int bestLeft = 0, bestRight = 0;
            int runStart = -1, gap = 0;
            auto closeRun = [&](int end) {
                if (runStart >= 0 && end - runStart > bestRight - bestLeft) {
                    bestLeft = runStart;
                    bestRight = end;
                }
                runStart = -1;
            };
            for (int x = 0; x < boxWidth; ++x) {
                if (columnInk[size_t(x)] > 0) {
                    if (runStart < 0)
                        runStart = x;
                    gap = 0;
                } else if (runStart >= 0 && ++gap > 2) {
                    closeRun(x - gap - 1);
                }
            }
            closeRun(boxWidth);
            // A run narrower than twice the line height is a marker, so the whole line stays scanned.
            if (bestRight - bestLeft < (bottom - top) * 2 || bestRight <= bestLeft) {
                bestLeft = 0;
                bestRight = boxWidth;
            }
            lineInk.assign(size_t(bottom - top), 0);
            for (int y = top; y < bottom; ++y)
                for (int x = bestLeft; x < bestRight; ++x)
                    lineInk[size_t(y - top)] += inkMark[size_t(y) * size_t(boxWidth) + size_t(x)];
            int peak = 0;
            for (int count : lineInk)
                peak = std::max(peak, count);
            if (peak < 3)
                continue;
            int dense = 0;
            for (int count : lineInk)
                if (count * 2 >= peak)
                    ++dense;
            if (dense >= 3)
                cores.push_back(float(dense));
        }
        if (!cores.empty()) {
            std::sort(cores.begin(), cores.end());
            result.xHeight = cores[cores.size() / 2];
        }
    }
    // A large heading fills most of its band, which makes the colour vote unreliable. The row geometry
    // measured above still stands in that case, so only the colour hint drops out.
    if (foreground >= 2 && foreground <= size_t(Width(box)) * Height(box) * .55) {
        double best = 0;
        int selected = -1;
        for (int i = 0; i < 4096; ++i)
            if (counts[i] >= 2) {
                double distance = std::abs(double(rs[i]) / counts[i] - br) +
                                  std::abs(double(gs[i]) / counts[i] - bg) +
                                  std::abs(double(bs[i]) / counts[i] - bb);
                double weight = counts[i] * distance * distance;
                if (weight > best) {
                    best = weight;
                    selected = i;
                }
            }
        if (selected >= 0)
            result.color = RGB(rs[selected] / counts[selected], gs[selected] / counts[selected],
                               bs[selected] / counts[selected]);
    }
    if (rows.empty() || rows.size() > 100)
        return result;
    std::vector<int> heights;
    for (auto r : rows)
        heights.push_back(Height(r));
    std::sort(heights.begin(), heights.end());
    const int median = heights[heights.size() / 2];
    if (median < 5 || median > 200)
        return result;
    // Reject diagrams and rows with incompatible heights instead of assigning a false font size.
    if (std::count_if(heights.begin(), heights.end(), [&](int h) {
            return h >= median * .6 && h <= median * 1.5;
        }) < int(heights.size() * .8))
        return result;
    for (auto r : rows) {
        if (Height(r) < median * .5)
            continue;
        r.left = std::max(box.left, r.left - 1);
        r.right = std::min(box.right, r.right + 1);
        r.top = std::max(box.top, r.top - 1);
        r.bottom = std::min(box.bottom, r.bottom + 1);
        result.lines.push_back(r);
    }
    result.lineHeight = float(median);
    return result;
}
PixelText InspectSingleParagraph(const Image& image, RECT hint, const std::string& original) {
    if (image.height > 512 || Wide(original).size() < 24)
        return {};
    auto pixels = InspectTextPixels(image, {0, 0, image.width, image.height});
    if (pixels.lines.size() < 2 || pixels.lines.size() > 8)
        return {};
    auto paragraph = pixels.lines.front();
    const float tolerance = pixels.lineHeight * 1.5f;
    for (size_t i = 1; i < pixels.lines.size(); ++i) {
        const auto& line = pixels.lines[i];
        if (line.top - pixels.lines[i - 1].bottom > tolerance ||
            std::abs(line.left - pixels.lines.front().left) > tolerance)
            return {};
        RECT joined{};
        UnionRect(&joined, &paragraph, &line);
        paragraph = joined;
    }
    RECT intersection{};
    if (!IntersectRect(&intersection, &paragraph, &hint))
        return {};
    return pixels;
}
// A detected line box carries padding above and below the ink, and the padding weighs more on small
// text: a 14 px label sits in a 21 px box while a 32 px heading nearly fills its 33 px box. This
// returns the ink share of a detected band before the glyph ratio converts it to a font size.
float InkBand(float band) {
    const float share = .70f + .15f * std::clamp((band - 14.f) / 26.f, 0.f, 1.f);
    return band * share;
}
// Writing systems and character classes decide which part of the em box the ink fills.
struct TextTraits {
    bool cjk{}, upper{}, lower{}, ascender{}, descender{}, digit{};
};
TextTraits AnalyzeText(const std::string& text) {
    TextTraits traits;
    size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        char32_t code = 0;
        size_t length = 1;
        if (lead < 0x80) {
            code = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            code = lead & 0x1F;
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            code = lead & 0x0F;
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            code = lead & 0x07;
            length = 4;
        } else {
            ++i;
            continue;
        }
        if (i + length > text.size())
            break;
        for (size_t k = 1; k < length; ++k)
            code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        i += length;
        if ((code >= 0x2E80 && code <= 0x9FFF) || (code >= 0xAC00 && code <= 0xD7AF) ||
            (code >= 0xF900 && code <= 0xFAFF) || (code >= 0xFF00 && code <= 0xFF60))
            traits.cjk = true;
        else if (code >= U'A' && code <= U'Z')
            traits.upper = true;
        else if (code >= U'0' && code <= U'9')
            traits.digit = true;
        else if (code >= U'a' && code <= U'z') {
            traits.lower = true;
            const auto letter = wchar_t(code);
            if (std::wstring_view(L"bdfhklt").find(letter) != std::wstring_view::npos)
                traits.ascender = true;
            if (std::wstring_view(L"gjpqy").find(letter) != std::wstring_view::npos)
                traits.descender = true;
        }
    }
    return traits;
}
// The detected band measures ink, not the em box. Ink coverage follows the characters a passage
// contains: a lowercase run without ascenders fills half the em box, while capitals, ascenders and
// descenders fill most of it. This ratio converts a band height back to the font size behind it.
float GlyphBandRatio(const TextTraits& traits) {
    // Reference product measurement: the replacement face uses font_px = ink_height, and its ink
    // height lands within 4% of the source. A CJK glyph spans the whole measured band, so the em
    // size behind that band is the band itself. Latin ink covers a narrower share and depends on
    // which vertical extremes appear.
    if (traits.cjk)
        return 1.f;
    if (traits.descender)
        return .95f;
    if (traits.upper || traits.ascender || traits.digit)
        return .74f;
    if (traits.lower)
        return .54f;
    return .74f;
}
// The dense core of a line is the lowercase body. It tracks the em size far better than the full
// band, because ascenders, descenders and neighbouring icons only add sparse rows.
float GlyphCoreRatio(const TextTraits& traits) {
    if (traits.cjk)
        return .78f;
    if (!traits.lower)
        return .70f;
    return .56f;
}
// A source row that ends well short of the box width carries an explicit break: a label, a heading
// or a list item. Rows that fill the box are screen wrapping, and their breaks carry no meaning. The
// last row of a wrapped passage is naturally short, so only a strict majority counts. A wide gap
// between neighbouring rows carries the same meaning on its own: separate labels sit apart, wrapped
// rows sit close.
bool RowsBreakExplicitly(const std::vector<RECT>& lines, LONG boxWidth) {
    if (lines.size() < 2 || boxWidth <= 0)
        return false;
    size_t shortRows = 0;
    for (const auto& line : lines)
        if (Width(line) < boxWidth * .85f)
            ++shortRows;
    if (shortRows * 2 > lines.size())
        return true;
    size_t wideGaps = 0;
    for (size_t i = 1; i < lines.size(); ++i) {
        const int gap = lines[i].top - lines[i - 1].bottom;
        const int band = std::min(Height(lines[i - 1]), Height(lines[i]));
        if (band > 0 && gap > band * .6f)
            ++wideGaps;
    }
    return wideGaps * 2 > lines.size() - 1;
}
// A manga column runs top to bottom, so one glyph occupies one row of the strip and the strip width is
// the glyph size. The column height measures the whole run instead. Reading that height as an em size
// inflates the face far past the strip: a four-glyph column then paints several times its own width.
// A balloon several strips wide keeps every strip rectangle, so this test reads the strips, not the
// passage aspect, and a wide passage of narrow strips still counts as a column.
bool IsVerticalSource(const std::vector<RECT>& lines, const RECT& box) {
    if (lines.empty())
        return Width(box) > 0 && Height(box) > Width(box) * 6 / 5;
    // A glyph box sits close to square, so the strip test accepts a box slightly taller than wide. The
    // previous 1.2 limit left a near-square balloon on the horizontal path, where wrapping the
    // translation into a narrow box squeezed one glyph against the next.
    return std::all_of(lines.begin(), lines.end(), [](const RECT& line) {
        return Width(line) > 0 && Height(line) * 5 > Width(line) * 6;
    });
}
bool HasSeparateTextLines(const std::string& original) {
    // Completed sentences and bracketed fields are semantic rows, not word wrapping.
    // Keep their line breaks, for example a notice followed by a download-size field.
    const auto text = Wide(original);
    size_t start = 0, complete = 0;
    for (size_t end = text.find(L'\n'); end != std::wstring::npos; end = text.find(L'\n', start)) {
        if (end == start)
            return false;
        auto last = text.find_last_not_of(L" \t\r", end - 1);
        if (last == std::wstring::npos || last < start ||
            std::wstring_view(L"。！？.!?】]").find(text[last]) == std::wstring_view::npos)
            return false;
        ++complete;
        start = end + 1;
    }
    return complete > 0;
}
} // namespace
std::vector<PositionedText> LayoutPositioned(const TranslationResult& result, const Image& image,
                                             const Settings& settings, UINT dpi) {
    if (image.width <= 0 || image.height <= 0)
        throw AppError("layout", "截图尺寸无效");
    ComPtr<IDWriteFactory> factory;
    CheckHR(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(factory.GetAddressOf())),
            "layout");
    // Recognized rows without a translated passage join the layout as cover-only entries: the paint
    // pass rebuilds their surface and draws no text, so source text never survives beside the
    // translation while artwork outside those rows keeps its captured pixels.
    std::vector<Segment> segments = result.segments;
    for (const RECT& line : result.uncoveredLines) {
        Segment cover;
        cover.sourcePixels = line;
        cover.sourceLines = {line};
        cover.sourceLineHeight = float(Height(line));
        segments.push_back(std::move(cover));
    }
    std::vector<PositionedText> output(segments.size());
    std::vector<float> lineHeights(segments.size());
    std::vector<float> coreHeights(segments.size());
    std::vector<std::optional<COLORREF>> surfaceColors(segments.size());
    std::vector<std::optional<COLORREF>> textColors(segments.size());
    std::vector<bool> columns(segments.size());
    const RECT bounds{0, 0, image.width, image.height};
    const float scale = dpi / 96.f;
    std::vector<RECT> protectedRegions = result.protectedRegions;
    for (size_t i = 0; i < output.size(); ++i) {
        const auto& s = segments[i];
        if (s.positionUnavailable) {
            output[i].suppressed = true;
            continue;
        }
        auto r = s.sourcePixels.value_or(MapBox(s.box, image));
        RECT clipped{};
        IntersectRect(&clipped, &r, &bounds);
        auto pixels = InspectTextPixels(image, clipped);
        bool measuredParagraph = false;
        if (!s.sourcePixels && segments.size() == 1) {
            auto paragraph = InspectSingleParagraph(image, clipped, s.original);
            if (!paragraph.lines.empty() && paragraph.lines.size() >= pixels.lines.size()) {
                pixels = std::move(paragraph);
                RECT measured = pixels.lines.front();
                for (auto line : pixels.lines) {
                    RECT joined{};
                    UnionRect(&joined, &measured, &line);
                    measured = joined;
                }
                clipped = measured;
                measuredParagraph = true;
            }
        }
        output[i].detectedLines = s.sourceLines;
        lineHeights[i] = s.sourceLineHeight;
        // A vertical column carries its run length in the band and its glyph size in the strip width. The
        // glyph size is what the face follows, so the column takes its own measurement: one strip, or the
        // median strip of a balloon that wraps across several of them.
        if (IsVerticalSource(s.sourceLines, clipped)) {
            columns[i] = true;
            std::vector<float> widths;
            for (auto line : s.sourceLines)
                if (Width(line) > 0)
                    widths.push_back(float(Width(line)));
            if (widths.empty())
                widths.push_back(float(Width(clipped)));
            std::sort(widths.begin(), widths.end());
            lineHeights[i] = std::max(1.f, widths[widths.size() / 2]);
        }
        // AI boxes often touch the captured edge, so partial glyph rows still describe the line rhythm.
        // Accept every detected row set with a plausible line height instead of requiring a fully
        // enclosed paragraph.
        const bool detectedRows = !pixels.lines.empty() && pixels.lineHeight >= 8;
        if (!s.sourcePixels && (measuredParagraph || detectedRows)) {
            // A model rectangle carries no row geometry, so the measured rows describe the passage.
            output[i].detectedLines = pixels.lines;
            lineHeights[i] = pixels.lineHeight;
        } else if (!columns[i] && s.sourcePixels && detectedRows && pixels.lineHeight >= lineHeights[i] * .55f &&
                   pixels.lineHeight <= lineHeights[i] * .95f) {
            // The detector tightens its rows to the ink, yet shaded artwork can still hide part of a
            // glyph row. A measurement inside a plausible ink share refines the band; one far outside
            // it means the pixel pass lost the row and the detector band stands.
            lineHeights[i] = pixels.lineHeight;
        }
        if (settings.matchTextColor)
            output[i].sourceColor = pixels.color;
        coreHeights[i] = pixels.xHeight;
        textColors[i] = pixels.color;
        surfaceColors[i] = pixels.background;
        output[i].sourceRect = clipped;
        protectedRegions.push_back(clipped);
    }
    for (size_t i = 0; i < output.size(); ++i) {
        auto& item = output[i];
        const auto& segment = segments[i];
        const RECT source = item.sourceRect;
        if (!Valid(source) || segment.untranslated) {
            item.suppressed = true;
            continue;
        }
        // A cover-only row exists to remove remaining source text, so a neighbouring translated row
        // keeps its guard against erasing it a second time while the cover proceeds.
        const bool coverOnly = segment.translated.empty() && segment.original.empty();
        if (!coverOnly) {
            // Ambiguous source geometry must not erase a neighboring passage. Full reading retains its text.
            // A fragment that sits inside a complete passage yields to it instead of disabling both: one
            // stray strip must not cost the balloon its translation.
            const long long own = static_cast<long long>(Width(source)) * Height(source);
            for (size_t j = 0; j < output.size(); ++j) {
                if (!segment.sourcePixels || i == j)
                    continue;
                if (!Conflicts(source, output[j].sourceRect))
                    continue;
                const long long other = static_cast<long long>(Width(output[j].sourceRect)) *
                                        Height(output[j].sourceRect);
                // A smaller fragment that already stepped aside does not disable the passage that
                // displaced it; equal passages still both stand down, because neither owns the pixels.
                if (output[j].suppressed && other < own)
                    continue;
                if (other >= own)
                    item.suppressed = true;
            }
        }
        auto bg = AnalyzeBackground(image, source);
        const float detectedHeight = lineHeights[i];
        const bool hasSurface = surfaceColors[i].has_value();
        // The replacement has to read as native content: fill the source surface with the sampled artwork
        // colour and keep the original glyph colour for the translation.
        item.background = hasSurface ? surfaceColors[i].value_or(bg.color) : bg.color;
        const int pad = std::max(1, int(std::ceil(2.0 * scale)));
        // The repair ring only has to reach the glyph fringes. A wide ring repaints artwork that the
        // translation never covers, which reads as a grey plate over a gradient surface.
        const int margin =
            detectedHeight > 0 ? pad + std::clamp(int(std::ceil(detectedHeight * .18f)), 2, 7) : pad;
        RECT area{std::max(0L, source.left - pad), std::max(0L, source.top - margin),
                  std::min<LONG>(image.width, source.right + pad),
                  std::min<LONG>(image.height, source.bottom + margin)};
        // Share only the whitespace between source regions. The region's origin never migrates to another
        // paragraph.
        for (auto other : protectedRegions) {
            if (!Valid(other) || EqualRect(&other, &source))
                continue;
            if (Conflicts(source, other)) {
                // Same precedence as above: a complete passage keeps its translation, and only the
                // smaller fragment steps aside.
                const long long own = static_cast<long long>(Width(source)) * Height(source);
                const long long rival =
                    static_cast<long long>(Width(other)) * Height(other);
                if (segment.sourcePixels && rival >= own)
                    item.suppressed = true;
                continue;
            }
            if (segment.sourcePixels && other.left < area.right && other.right > area.left) {
                if (other.bottom <= source.top)
                    area.top = std::max(area.top, (source.top + other.bottom + 1) / 2);
                if (other.top >= source.bottom)
                    area.bottom = std::min(area.bottom, (source.bottom + other.top) / 2);
            }
            if (segment.sourcePixels && other.top < area.bottom && other.bottom > area.top) {
                if (other.right <= source.left)
                    area.left = std::max(area.left, (source.left + other.right + 1) / 2);
                if (other.left >= source.right)
                    area.right = std::min(area.right, (source.right + other.left) / 2);
            }
        }
        item.rect = area;
        item.padding = float(std::max(0, std::min(pad, (std::min(Width(area), Height(area)) - 1) / 2)));
        {
            // Repair the whole translated box at pixel level: every pixel keeps the captured artwork, and
            // only the glyph strokes are replaced with the local backdrop. Nothing is pasted on top, so the
            // replacement reads as native scenery and no border, veil or blurred band ever appears.
            const int grow = std::max(4, int(std::ceil((detectedHeight > 0 ? detectedHeight : 12.f) * .12f)));
            const auto band = [&](int x0, int x1, int y0, int y1, bool excludeGlyph) -> std::optional<COLORREF> {
                x0 = std::max(0, x0);
                x1 = std::min(image.width, x1);
                y0 = std::max(0, y0);
                y1 = std::min(image.height, y1);
                if (x1 - x0 < 1 || y1 - y0 < 1)
                    return std::nullopt;
                const int stepX = std::max(1, (x1 - x0) / 6), stepY = std::max(1, (y1 - y0) / 8);
                const COLORREF glyph = textColors[i].value_or(RGB(0, 0, 0));
                const bool skipGlyph = excludeGlyph && textColors[i].has_value();
                long long sums[3]{};
                int count = 0;
                for (int y = y0; y < y1; y += stepY)
                    for (int x = x0; x < x1; x += stepX) {
                        const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                        if (skipGlyph && std::abs(p[2] - GetRValue(glyph)) + std::abs(p[1] - GetGValue(glyph)) +
                                                 std::abs(p[0] - GetBValue(glyph)) <
                                             90)
                            continue;
                        sums[0] += p[2];
                        sums[1] += p[1];
                        sums[2] += p[0];
                        ++count;
                    }
                if (count < 4)
                    return std::nullopt;
                return RGB(int(sums[0] / count), int(sums[1] / count), int(sums[2] / count));
            };
            // The artwork supplies the backdrop; the four corner samples keep its gradients continuous.
            const auto buildCover = [&](const RECT& region, COLORREF surfaceRef) {
                std::vector<unsigned char> cover;
                const int coverWidth = int(region.right - region.left);
                const int coverHeight = int(region.bottom - region.top);
                if (coverWidth <= 0 || coverHeight <= 0 || !Valid(region))
                    return cover;
                const int reach = std::max(6, grow * 3);
                const auto cornerColor = [&](int x0, int x1, int y0, int y1, COLORREF fallback) {
                    auto sampled = band(x0, x1, y0, y1, true);
                    return sampled ? *sampled : fallback;
                };
                const COLORREF topLeft = cornerColor(int(region.left) - reach, int(region.left) - 1,
                                                     int(region.top) - reach, int(region.top) - 1, surfaceRef);
                const COLORREF topRight = cornerColor(int(region.right) + 1, int(region.right) + reach,
                                                      int(region.top) - reach, int(region.top) - 1, surfaceRef);
                const COLORREF bottomLeft = cornerColor(int(region.left) - reach, int(region.left) - 1,
                                                        int(region.bottom) + 1, int(region.bottom) + reach,
                                                        surfaceRef);
                const COLORREF bottomRight = cornerColor(int(region.right) + 1, int(region.right) + reach,
                                                         int(region.bottom) + 1, int(region.bottom) + reach,
                                                         surfaceRef);
                const int corner[4][3] = {
                    {GetRValue(topLeft), GetGValue(topLeft), GetBValue(topLeft)},
                    {GetRValue(topRight), GetGValue(topRight), GetBValue(topRight)},
                    {GetRValue(bottomLeft), GetGValue(bottomLeft), GetBValue(bottomLeft)},
                    {GetRValue(bottomRight), GetGValue(bottomRight), GetBValue(bottomRight)}};
                // The backdrop of each block is its per-channel median. Glyphs cover less than half of a
                // block, so the median reproduces the surface exactly: a white page stays white under dark
                // glyphs, and dark scenery stays dark under light glyphs. An average would fold the glyph
                // colour into the backdrop and leave a grey patch wherever text was replaced.
                constexpr int blockSize = 24;
                const int blocksX = (coverWidth + blockSize - 1) / blockSize;
                const int blocksY = (coverHeight + blockSize - 1) / blockSize;
                std::vector<int> blockRed(size_t(blocksX) * size_t(blocksY), 0);
                std::vector<int> blockGreen(size_t(blocksX) * size_t(blocksY), 0);
                std::vector<int> blockBlue(size_t(blocksX) * size_t(blocksY), 0);
                {
                    std::vector<int> histogram(256);
                    std::vector<int> reds, greens, blues;
                    std::vector<int> closeReds, closeGreens, closeBlues;
                    for (int by = 0; by < blocksY; ++by)
                        for (int bx = 0; bx < blocksX; ++bx) {
                            reds.clear();
                            greens.clear();
                            blues.clear();
                            closeReds.clear();
                            closeGreens.clear();
                            closeBlues.clear();
                            const int x0 = bx * blockSize, x1 = std::min(coverWidth, x0 + blockSize);
                            const int y0 = by * blockSize, y1 = std::min(coverHeight, y0 + blockSize);
                            for (int y = y0; y < y1; ++y)
                                for (int x = x0; x < x1; ++x) {
                                    const int sx = std::clamp(int(region.left) + x, 0, image.width - 1);
                                    const int sy = std::clamp(int(region.top) + y, 0, image.height - 1);
                                    const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                                    reds.push_back(p[2]);
                                    greens.push_back(p[1]);
                                    blues.push_back(p[0]);
                                    // A large heading fills most of a block with stroke pixels, and a plain
                                    // median would then return the glyph colour as the backdrop. Pixels
                                    // near the sampled surface anchor the block on the actual surface.
                                    if (std::abs(int(p[2]) - GetRValue(surfaceRef)) +
                                            std::abs(int(p[1]) - GetGValue(surfaceRef)) +
                                            std::abs(int(p[0]) - GetBValue(surfaceRef)) < 96) {
                                        closeReds.push_back(p[2]);
                                        closeGreens.push_back(p[1]);
                                        closeBlues.push_back(p[0]);
                                    }
                                }
                            if (closeReds.size() >= 12) {
                                reds.swap(closeReds);
                                greens.swap(closeGreens);
                                blues.swap(closeBlues);
                            }
                            if (reds.empty()) {
                                reds.push_back(GetRValue(surfaceRef));
                                greens.push_back(GetGValue(surfaceRef));
                                blues.push_back(GetBValue(surfaceRef));
                            }
                            const auto medianOf = [&histogram](std::vector<int>& values) -> int {
                                std::fill(histogram.begin(), histogram.end(), 0);
                                for (int value : values)
                                    ++histogram[size_t(std::clamp(value, 0, 255))];
                                const int target = int(values.size() / 2) + 1;
                                int seen = 0;
                                for (int value = 0; value < 256; ++value) {
                                    seen += histogram[size_t(value)];
                                    if (seen >= target)
                                        return value;
                                }
                                return 255;
                            };
                            const size_t block = size_t(by) * size_t(blocksX) + size_t(bx);
                            blockRed[block] = medianOf(reds);
                            blockGreen[block] = medianOf(greens);
                            blockBlue[block] = medianOf(blues);
                        }
                }
                std::vector<unsigned char> glyphMask(size_t(coverWidth) * size_t(coverHeight), 0);
                // The detected glyph rows limit the repair: a balloon outline, a panel border and the
                // scenery around the text keep their captured pixels. Rebuilding them from the median
                // surface would paint the balloon colour over the artwork beside it.
                const auto insideDetectedLine = [&](int sx, int sy) {
                    if (item.detectedLines.empty())
                        return true;
                    return std::any_of(item.detectedLines.begin(), item.detectedLines.end(),
                                       [&](RECT line) {
                                           return sx >= line.left - grow && sx <= line.right + grow &&
                                                  sy >= line.top - grow && sy <= line.bottom + grow;
                                       });
                };
                // Capturing the soft edge of a glyph as well as its core keeps halo and antialiasing
                // fringes out of the rebuilt surface.
                constexpr int departureThreshold = 60;
                for (int y = 0; y < coverHeight; ++y) {
                    for (int x = 0; x < coverWidth; ++x) {
                        const int sx = std::clamp(int(region.left) + x, 0, image.width - 1);
                        const int sy = std::clamp(int(region.top) + y, 0, image.height - 1);
                        if (!insideDetectedLine(sx, sy))
                            continue;
                        const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                        const size_t block = size_t(y / blockSize) * size_t(blocksX) + size_t(x / blockSize);
                        const int meanR = blockRed[block];
                        const int meanG = blockGreen[block];
                        const int meanB = blockBlue[block];
                        const int departure = std::abs(int(p[2]) - meanR) + std::abs(int(p[1]) - meanG) +
                                              std::abs(int(p[0]) - meanB);
                        glyphMask[size_t(y) * coverWidth + x] = departure > departureThreshold ? 1 : 0;
                    }
                }
                std::vector<unsigned char> grown = glyphMask;
                for (int y = 0; y < coverHeight; ++y)
                    for (int x = 0; x < coverWidth; ++x) {
                        if (!glyphMask[size_t(y) * coverWidth + x])
                            continue;
                        for (int dy = -2; dy <= 2; ++dy)
                            for (int dx = -2; dx <= 2; ++dx) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx >= 0 && nx < coverWidth && ny >= 0 && ny < coverHeight)
                                    grown[size_t(ny) * coverWidth + nx] = 1;
                            }
                    }
                // Start from the captured artwork pixel by pixel, then rebuild each masked stroke from its
                // nearest untouched neighbours on the same scanline. Local gradients, texture and lighting
                // survive, so the repaired row matches the surrounding scenery.
                cover.resize(size_t(coverWidth) * size_t(coverHeight) * 4);
                for (int y = 0; y < coverHeight; ++y)
                    for (int x = 0; x < coverWidth; ++x) {
                        auto* p = &cover[(size_t(y) * coverWidth + x) * 4];
                        const int sx = std::clamp(int(region.left) + x, 0, image.width - 1);
                        const int sy = std::clamp(int(region.top) + y, 0, image.height - 1);
                        const auto* src = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                        p[0] = src[0];
                        p[1] = src[1];
                        p[2] = src[2];
                        p[3] = 255;
                    }
                std::vector<unsigned char> rowFull(size_t(coverHeight), 0);
                for (int y = 0; y < coverHeight; ++y) {
                    int masked = 0;
                    for (int x = 0; x < coverWidth; ++x)
                        masked += grown[size_t(y) * coverWidth + x] ? 1 : 0;
                    rowFull[size_t(y)] = masked == coverWidth ? 1 : 0;
                    int x = 0;
                    while (x < coverWidth) {
                        if (!grown[size_t(y) * coverWidth + x]) {
                            ++x;
                            continue;
                        }
                        const int runStart = x;
                        while (x < coverWidth && grown[size_t(y) * coverWidth + x])
                            ++x;
                        const int runEnd = x;
                        const int leftX = runStart - 1, rightX = runEnd;
                        const bool hasLeft = leftX >= 0, hasRight = rightX < coverWidth;
                        if (!hasLeft && !hasRight)
                            continue;
                        const bool longRun = runEnd - runStart > 40;
                        for (int k = runStart; k < runEnd; ++k) {
                            auto* p = &cover[(size_t(y) * coverWidth + k) * 4];
                            if (longRun) {
                                // A wide run belongs to a control face or a large title, where the pixels just
                                // above and below describe the surface far better than the far ends of the row.
                                int yUp = y - 1;
                                while (yUp >= 0 && grown[size_t(yUp) * coverWidth + k])
                                    --yUp;
                                int yDown = y + 1;
                                while (yDown < coverHeight && grown[size_t(yDown) * coverWidth + k])
                                    ++yDown;
                                const auto* upPixel =
                                    yUp >= 0 ? &cover[(size_t(yUp) * coverWidth + k) * 4] : nullptr;
                                const auto* downPixel =
                                    yDown < coverHeight ? &cover[(size_t(yDown) * coverWidth + k) * 4] : nullptr;
                                if (upPixel && downPixel) {
                                    const float vt = float(y - yUp) / float(yDown - yUp);
                                    for (int c = 0; c < 3; ++c)
                                        p[c] = static_cast<unsigned char>(std::clamp(
                                            int(float(upPixel[c]) +
                                                float(int(downPixel[c]) - int(upPixel[c])) * vt + .5f),
                                            0, 255));
                                    p[3] = 255;
                                    continue;
                                }
                                if (upPixel || downPixel) {
                                    const auto* single = upPixel ? upPixel : downPixel;
                                    for (int c = 0; c < 3; ++c)
                                        p[c] = single[c];
                                    p[3] = 255;
                                    continue;
                                }
                            }
                            const auto* leftPixel = hasLeft ? &cover[(size_t(y) * coverWidth + leftX) * 4]
                                                            : &cover[(size_t(y) * coverWidth + rightX) * 4];
                            const auto* rightPixel = hasRight ? &cover[(size_t(y) * coverWidth + rightX) * 4]
                                                              : leftPixel;
                            const float t = hasLeft && hasRight ? float(k - leftX) / float(rightX - leftX)
                                                                : (hasLeft ? 0.f : 1.f);
                            for (int c = 0; c < 3; ++c)
                                p[c] = static_cast<unsigned char>(std::clamp(
                                    int(float(leftPixel[c]) + float(int(rightPixel[c]) - int(leftPixel[c])) * t +
                                        .5f),
                                    0, 255));
                            p[3] = 255;
                        }
                    }
                }
                // Smooth the repaired strokes inside the mask: scanline interpolation along a large heading
                // leaves faint vertical banding where the letter stems stood, and a short blur removes it
                // while the rim still blends into the untouched artwork.
                for (int pass = 0; pass < 3; ++pass) {
                    std::vector<unsigned char> scratch(cover);
                    for (int y = 1; y + 1 < coverHeight; ++y)
                        for (int x = 1; x + 1 < coverWidth; ++x) {
                            if (!grown[size_t(y) * coverWidth + x])
                                continue;
                            int sums[3]{};
                            for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx) {
                                    const auto* q = &cover[(size_t(y + dy) * coverWidth + (x + dx)) * 4];
                                    sums[0] += q[0];
                                    sums[1] += q[1];
                                    sums[2] += q[2];
                                }
                            auto* p = &scratch[(size_t(y) * coverWidth + x) * 4];
                            p[0] = static_cast<unsigned char>(sums[0] / 9);
                            p[1] = static_cast<unsigned char>(sums[1] / 9);
                            p[2] = static_cast<unsigned char>(sums[2] / 9);
                            p[3] = 255;
                        }
                    cover.swap(scratch);
                } // A scanline that is entirely glyph (a large heading) has no neighbour to copy, so the
                // band surface colour carries it.
                for (int y = 0; y < coverHeight; ++y) {
                    if (!rowFull[size_t(y)])
                        continue;
                    const float fy = coverHeight > 1 ? float(y) / float(coverHeight - 1) : .5f;
                    const COLORREF bandRef = surfaceRef;
                    for (int x = 0; x < coverWidth; ++x) {
                        const float fx = coverWidth > 1 ? float(x) / float(coverWidth - 1) : .5f;
                        auto* p = &cover[(size_t(y) * coverWidth + x) * 4];
                        for (int ch = 0; ch < 3; ++ch) {
                            const float topValue = float(corner[0][ch]) +
                                                   float(corner[1][ch] - corner[0][ch]) * fx;
                            const float bottomValue = float(corner[2][ch]) +
                                                      float(corner[3][ch] - corner[2][ch]) * fx;
                            const float value = topValue + (bottomValue - topValue) * fy;
                            const float surfaceValue = float((bandRef >> (ch * 8)) & 0xFF);
                            const float blended = value * .35f + surfaceValue * .65f;
                            p[2 - ch] = static_cast<unsigned char>(std::clamp(int(blended + .5f), 0, 255));
                        }
                        p[3] = 255;
                    }
                }
                // Rebuilt strokes take the flattened tone so scanline seams disappear. Every other pixel
                // keeps the captured artwork, which matches the reference product: only the glyph
                // strokes are erased, and the surface around them stays byte for byte.
                const auto blockValue = [&](int channel, int bx, int by) -> float {                    bx = std::clamp(bx, 0, blocksX - 1);
                    by = std::clamp(by, 0, blocksY - 1);
                    const size_t index = size_t(by) * size_t(blocksX) + size_t(bx);
                    switch (channel) {
                    case 0:
                        return float(blockRed[index]);
                    case 1:
                        return float(blockGreen[index]);
                    default:
                        return float(blockBlue[index]);
                    }
                };
                for (int y = 0; y < coverHeight; ++y) {
                    const float gy = (float(y) + .5f) / float(blockSize) - .5f;
                    const int y0 = int(std::floor(gy));
                    const float ty = gy - float(y0);
                    for (int x = 0; x < coverWidth; ++x) {
                        const float gx = (float(x) + .5f) / float(blockSize) - .5f;
                        const int x0 = int(std::floor(gx));
                        const float tx = gx - float(x0);
                        // Only rebuilt strokes take the flattened tone. An untouched pixel keeps the
                        // captured artwork exactly, which is how the reference implementation behaves:
                        // gradients, texture and highlight survive while the glyph strokes go away.
                        const float weight = grown[size_t(y) * coverWidth + x] ? .55f : 0.f;
                        auto* p = &cover[(size_t(y) * coverWidth + x) * 4];
                        float target[3]{};
                        for (int ch = 0; ch < 3; ++ch) {
                            const float top =
                                blockValue(ch, x0, y0) * (1 - tx) + blockValue(ch, x0 + 1, y0) * tx;
                            const float bottom =
                                blockValue(ch, x0, y0 + 1) * (1 - tx) + blockValue(ch, x0 + 1, y0 + 1) * tx;
                            target[ch] = top * (1 - ty) + bottom * ty;
                        }
                        // A large deviation means this pixel belongs to another visual surface than its
                        // block, such as the boundary between the sidebar and the page. Keeping more of
                        // the captured pixel there preserves that boundary while flat areas even out.
                        const float deviation = (std::abs(target[0] - float(p[2])) +
                                                 std::abs(target[1] - float(p[1])) +
                                                 std::abs(target[2] - float(p[0]))) /
                                                3.f;
                        const float adaptive = weight / (1.f + deviation / 24.f);
                        for (int ch = 0; ch < 3; ++ch) {
                            const float current = float(p[2 - ch]);
                            p[2 - ch] = static_cast<unsigned char>(
                                std::clamp(int(current + (target[ch] - current) * adaptive + .5f), 0, 255));
                            p[3] = 255;
                        }
                    }
                }
                return cover;
            };
            if (Valid(area)) {
                // Cover a little past the box: the returned geometry sometimes clips the last glyph row,
                // and the extra ring reaches the leftovers while the untouched pixels still carry the
                // artwork through.
                RECT padded = area;
                InflateRect(&padded, grow, grow);
                padded.left = std::max<LONG>(0, padded.left);
                padded.top = std::max<LONG>(0, padded.top);
                padded.right = std::min<LONG>(image.width, padded.right);
                padded.bottom = std::min<LONG>(image.height, padded.bottom);
                EraseRow block{padded, bg.color, bg.color, bg.color, bg.color, false, padded};
                block.cover = buildCover(padded, item.background);
                if (!block.cover.empty())
                    item.eraseLines.push_back(block);
            }
        }
        float sourcePitch = 0.f;
        if (item.detectedLines.size() >= 2) {
            std::vector<float> pitches;
            pitches.reserve(item.detectedLines.size() - 1);
            for (size_t k = 1; k < item.detectedLines.size(); ++k)
                pitches.push_back(float(item.detectedLines[k].top - item.detectedLines[k - 1].top));
            std::sort(pitches.begin(), pitches.end());
            // Side-by-side strips share their top edge. A zero median describes columns rather than rows,
            // so such a passage keeps the default line rhythm instead of tightening rows to nothing.
            if (pitches[pitches.size() / 2] > 1.f)
                sourcePitch = pitches[pitches.size() / 2];
        }
        const bool column = columns[i];
        const float width = std::max(1.f, float(Width(area)) - 2 * item.padding),
                    height = std::max(1.f, float(Height(area)) - 2 * item.padding);
        // Line pitch follows the source rhythm and scales with the fitted font so a reduced font keeps
        // its lines inside the same box. Densely set source rows keep their tight rhythm, which also
        // lets a two-row passage use the two rows the detector measured.
        const float pitchRatio = detectedHeight > 0 && sourcePitch > 0
                                     ? std::clamp(sourcePitch / detectedHeight, 1.0f, 1.8f)
                                     : 1.15f;
        auto pitchFor = [&](float fontSize) { return fontSize * pitchRatio; };
        // A passage measured on one row keeps one row. The reference product clips a single-line block
        // at its own box instead of wrapping it, because a wrapped label no longer reads as one label.
        const size_t allowedRows = item.detectedLines.empty() ? 0 : item.detectedLines.size();
        const bool wrapText = allowedRows != 1;
        auto measure = [&](const std::wstring& text, float fontSize, float lineSpacing,
                           ComPtr<IDWriteTextLayout>& layout) {
            ComPtr<IDWriteTextFormat> format;
            CheckHR(factory->CreateTextFormat(settings.font.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize,
                                              L"zh-CN", &format),
                    "layout");
            CheckHR(format->SetWordWrapping(wrapText ? DWRITE_WORD_WRAPPING_WRAP
                                                     : DWRITE_WORD_WRAPPING_NO_WRAP),
                    "layout");
            CheckHR(format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineSpacing, fontSize * .8f),
                    "layout");
            layout.Reset();
            CheckHR(factory->CreateTextLayout(text.c_str(), UINT32(text.size()), format.Get(), width,
                                              100000.f, &layout),
                    "layout");
            // A little tracking separates neighbouring CJK glyphs. The metrics below include it, so the
            // fit search accounts for the wider line instead of letting glyphs touch inside a tight box.
            ComPtr<IDWriteTextLayout1> spaced;
            if (SUCCEEDED(layout.As(&spaced)))
                spaced->SetCharacterSpacing(0.f, fontSize * .02f, 0.f,
                                            DWRITE_TEXT_RANGE{0, UINT32(text.size())});
            DWRITE_TEXT_METRICS metrics{};
            CheckHR(layout->GetMetrics(&metrics), "layout");
            return metrics;
        };
        if (coverOnly) {
            // The surface is rebuilt above; this entry carries no translation to lay out.
            output[i].bandHeight = detectedHeight;
            output[i].coreHeight = coreHeights[i];
            continue;
        }
        if (column) {
            // A manga column keeps its reading direction: one glyph per row, filled strip by strip from
            // the right. Reusing the detected strip positions is what makes a translated balloon read
            // like the original instead of one strip squeezed by its whole sentence.
            auto text = Wide(segment.translated);
            std::vector<std::wstring> glyphs;
            for (size_t at = 0; at < text.size();) {
                const wchar_t ch = text[at];
                if (ch == L'\r' || ch == L'\n' || ch == L' ' || ch == L'\t') {
                    ++at;
                    continue;
                }
                size_t length = 1;
                if (ch >= 0xD800 && ch <= 0xDBFF && at + 1 < text.size())
                    length = 2;
                glyphs.push_back(text.substr(at, length));
                at += length;
            }
            std::vector<RECT> strips;
            for (auto line : item.detectedLines)
                if (Valid(line))
                    strips.push_back(line);
            if (strips.empty())
                strips.push_back(source);
            // Room counts down to the passage bottom: a short strip of a balloon still has the balloon
            // beneath it to grow into, and the whole balloon was measured as the source rectangle.
            const float glyphSize = std::max(9.f, detectedHeight * .90f);
            const auto room = [&](float pitch, const RECT& strip) {
                return int(std::max(1.f, std::floor(float(source.bottom - strip.top) / pitch)));
            };
            const auto capacity = [&](float size) {
                const float pitch = std::max(1.f, size * 1.12f);
                size_t total = 0;
                for (auto strip : strips)
                    total += size_t(room(pitch, strip));
                return total;
            };
            float size = glyphSize;
            if (!glyphs.empty() && capacity(size) < glyphs.size()) {
                // The translation filled every strip, so the face backs off until the passage holds it.
                float low = 8.f, high = size;
                for (int step = 0; step < 14; ++step) {
                    const float mid = (low + high) / 2;
                    if (capacity(mid) >= glyphs.size())
                        low = mid;
                    else
                        high = mid;
                }
                size = low;
            }
            item.fontSize = size;
            item.bandHeight = detectedHeight;
            item.coreHeight = coreHeights[i];
            item.eraseHeight = float(Height(source));
            item.overflow = capacity(size) < glyphs.size();
            const float pitch = std::max(1.f, size * 1.12f);
            size_t at = 0;
            float contentBottom = float(source.top);
            for (auto strip : strips) {
                if (at >= glyphs.size())
                    break;
                const int count = room(pitch, strip);
                std::wstring body;
                for (int k = 0; k < count && at < glyphs.size(); ++k, ++at) {
                    if (!body.empty())
                        body += L'\n';
                    body += glyphs[at];
                }
                if (body.empty())
                    continue;
                ComPtr<IDWriteTextFormat> columnFormat;
                CheckHR(factory->CreateTextFormat(settings.font.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                                  L"zh-CN", &columnFormat),
                        "layout");
                CheckHR(columnFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "layout");
                CheckHR(columnFormat->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, pitch, size * .8f),
                        "layout");
                CheckHR(columnFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER), "layout");
                ColumnText columnText;
                columnText.rect = strip;
                CheckHR(factory->CreateTextLayout(body.c_str(), UINT32(body.size()), columnFormat.Get(),
                                                  float(std::max(1, Width(strip))) + 4.f, 100000.f,
                                                  &columnText.layout),
                        "layout");
                if (!item.layout)
                    item.layout = columnText.layout;
                item.lineHeight = pitch;
                contentBottom = std::max(contentBottom, float(strip.top) + float(count) * pitch);
                item.columnTexts.push_back(std::move(columnText));
            }
            item.contentHeight = contentBottom - float(source.top);
            if (!item.layout)
                item.suppressed = true;
            continue;
        }
        auto text = Wide(segment.translated);
        // Short source rows carry explicit breaks that the translation has to mirror: labels, list
        // items and headings each stay on their own row. Only rows that fill the box describe screen
        // wrapping, whose breaks the translated passage replaces with its own wrapping.
        const bool explicitRows = RowsBreakExplicitly(item.detectedLines, Width(item.sourceRect));
        if (!column && segment.sourceLines.size() > 1 && !explicitRows &&
            !HasSeparateTextLines(segment.original)) {
            std::erase(text, L'\r');
            // OCR line breaks describe screen wrapping, not paragraph boundaries. Keep
            // explicit blank paragraphs; let DirectWrite wrap each translated passage.
            std::wstring reflowed;
            for (size_t at = 0; at < text.size(); ++at) {
                auto ch = text[at];
                if (ch == L'\r')
                    continue;
                if (ch == L'\n' && at > 0 && at + 1 < text.size() && text[at - 1] != L'\n' &&
                    text[at + 1] != L'\n' && text[at + 1] != L'\r') {
                    auto previous = reflowed.empty() ? L' ' : reflowed.back(), next = text[at + 1];
                    if (previous < 0x2e80 && next < 0x2e80 && previous != L' ' && next != L' ')
                        reflowed += L' ';
                    continue;
                }
                reflowed += ch;
            }
            text = std::move(reflowed);
        }
        float preferred = settings.fontSize * scale;
        output[i].bandHeight = detectedHeight;
        output[i].coreHeight = coreHeights[i];
        if (settings.autoFont) {
            if (detectedHeight > 0) {
                // Two estimates bound the face size from different sides: the padded detector band
                // converted through the ink share of its characters, and the dense core of the line
                // divided by the lowercase body share. Their errors run in opposite directions, so
                // the mean is steadier than either one alone.
                const TextTraits traits = AnalyzeText(segment.original);
                // CJK detector boxes hug the glyphs, so their band needs no padding discount; Latin
                // boxes carry a wider margin around the ink.
                const float bandSize = (traits.cjk ? detectedHeight : InkBand(detectedHeight)) /
                                       GlyphBandRatio(traits);
                const float core = coreHeights[i];
                // The reference product sets the face directly from the measured ink height, so a CJK
                // band stands on its own. Latin text keeps the two-estimate mean, where the dense core
                // of the lowercase body corrects a band that ascenders and descenders inflated.
                const float emSize = traits.cjk
                                         ? bandSize
                                         : (item.detectedLines.size() <= 1 && core >= 4.f
                                                ? (bandSize + core / GlyphCoreRatio(traits)) * .5f
                                                : bandSize);
                // A short label sits inside a control face, where the source label filled the middle
                // rather than the full height.
                const bool shortLabel = item.detectedLines.size() <= 1 &&
                                        Wide(segment.translated).size() <= 8;
                preferred = std::clamp(traits.cjk || !shortLabel ? emSize : emSize * .92f, 9.f, 144.f);
                // A multi-line box reports the paragraph height, so the face is capped against the
                // box itself: a heading never outgrows the line the source text occupied.
                preferred = std::min(preferred, std::max(9.f, float(Height(area)) * 1.0f));
            } else {
                // Fit original text to its box when OCR is unavailable; paragraph height is not a font size.
                float low = 10 * scale, high = std::min(144.f, std::max(26 * scale, float(Height(source))));
                ComPtr<IDWriteTextLayout> originalLayout;
                for (int step = 0; step < 10; ++step) {
                    float mid = (low + high) / 2;
                    auto m = measure(Wide(segment.original), mid, mid * 1.25f, originalLayout);
                    if (m.height <= height && m.widthIncludingTrailingWhitespace <= width)
                        low = mid;
                    else
                        high = mid;
                }
                preferred = low;
            }
        }
        // Chinese glyphs occupy less advance than kana, so the source line height is reduced to keep the
        // translated line count close to the source; line pitch follows the source rhythm.
        item.fontSize = preferred;
        auto metrics = measure(text, item.fontSize, pitchFor(item.fontSize), item.layout);
        // Source rows bound the layout: a passage measured on one row stays on one row, and a
        // two-row passage keeps two. The row count is the strongest quality signal the detector
        // gives, so the fit search reduces the face until both the rows and the box hold it.
        const auto exceedsRows = [&](const DWRITE_TEXT_METRICS& m, float fontSize) {
            if (!allowedRows)
                return false;
            const float pitch = pitchFor(fontSize);
            const float rows = pitch > 0 ? m.height / pitch : 1.f;
            return rows > float(allowedRows) + .2f;
        };
        // The replacement may run past the source box by the documented allowance; the same bound feeds
        // the fit search and the reporting flag so the two never disagree.
        const auto holdsBox = [&](const DWRITE_TEXT_METRICS& m) {
            return m.height <= height && m.widthIncludingTrailingWhitespace <= width + kOverhangAllowance;
        };
        if (settings.autoFont &&
            (!holdsBox(metrics) || exceedsRows(metrics, item.fontSize))) {
            // Bounded reduction preserves readability; overflow remains local and can be read in full view.
            float low = std::min(preferred, std::max(12 * scale, preferred * .6f)), high = preferred;
            for (int step = 0; step < 12; ++step) {
                float mid = (low + high) / 2;
                ComPtr<IDWriteTextLayout> trial;
                auto m = measure(text, mid, pitchFor(mid), trial);
                if (holdsBox(m) && !exceedsRows(m, mid))
                    low = mid;
                else
                    high = mid;
            }
            item.fontSize = low;
            metrics = measure(text, low, pitchFor(low), item.layout);
        }
        UINT32 lines{};
        item.layout->GetLineMetrics(nullptr, 0, &lines);
        std::vector<DWRITE_LINE_METRICS> lineMetrics(lines);
        if (lines) {
            CheckHR(item.layout->GetLineMetrics(lineMetrics.data(), lines, &lines), "layout");
            item.lineHeight = lineMetrics.front().height;
        }
        item.eraseHeight = float(Height(source));
        item.contentHeight = metrics.height;
        item.overflow = metrics.height > height + .5f ||
                        metrics.widthIncludingTrailingWhitespace > width + kOverhangAllowance + .5f;
    }
    return output;
}
COLORREF ReadableTextColor(COLORREF preferred, COLORREF background) {
    auto luminance = [](COLORREF c) {
        auto linear = [](int v) {
            double s = v / 255.;
            return s <= .04045 ? s / 12.92 : std::pow((s + .055) / 1.055, 2.4);
        };
        return .2126 * linear(GetRValue(c)) + .7152 * linear(GetGValue(c)) + .0722 * linear(GetBValue(c));
    };
    auto contrast = [&](COLORREF foreground) {
        double a = luminance(foreground), b = luminance(background);
        return (std::max(a, b) + .05) / (std::min(a, b) + .05);
    };
    // The captured colour describes the passage best, so it survives whenever it separates from the
    // rebuilt surface. Large-text contrast is enough here; a stricter ratio would turn coloured
    // links and tinted captions into plain black.
    constexpr double minimumContrast = 3.0;
    if (contrast(preferred) >= minimumContrast)
        return preferred;
    // Keep the measured hue and move only its lightness until the pair separates, so a blue link
    // stays blue and a warm caption keeps its warmth. The direction follows the endpoint that
    // separates best from the surface underneath.
    const double blackContrast = contrast(RGB(0, 0, 0));
    const double whiteContrast = contrast(RGB(255, 255, 255));
    const int target = blackContrast >= whiteContrast ? 0 : 255;
    for (int step = 1; step <= 16; ++step) {
        const float t = float(step) / 16.f;
        auto mix = [&](int channel) {
            return int(std::clamp(int(float(channel) + (float(target) - float(channel)) * t + .5f), 0, 255));
        };
        const COLORREF candidate =
            RGB(mix(GetRValue(preferred)), mix(GetGValue(preferred)), mix(GetBValue(preferred)));
        if (contrast(candidate) >= minimumContrast)
            return candidate;
    }
    return target == 0 ? RGB(0, 0, 0) : RGB(255, 255, 255);
}
void DrawPositioned(ID2D1RenderTarget* target, const std::vector<PositionedText>& items, const Image* image,
                    COLORREF textColor, const Settings& settings, UINT dpi, bool diagnostics) {
    auto color = [](COLORREF c) {
        return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f);
    };
    auto rect = [](RECT r) {
        return D2D1::RectF(float(r.left), float(r.top), float(r.right), float(r.bottom));
    };
    ComPtr<ID2D1SolidColorBrush> brush;
    CheckHR(target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush), "layout");
    ComPtr<ID2D1SolidColorBrush> rimBrush;
    CheckHR(target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &rimBrush), "layout");
    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    // A downscaled copy of the capture acts as a soft backdrop under every rebuilt row: stretched back up
    // it keeps the surrounding artwork colours continuous, so the veil blends with the scene instead of
    // stamping a flat block onto it.
    ComPtr<ID2D1Bitmap> blurBitmap;
    UINT blurWidth = 0, blurHeight = 0;
    if (image && image->width >= 16 && image->height >= 16) {
        constexpr int factor = 8;
        blurWidth = UINT(std::max(1, image->width / factor));
        blurHeight = UINT(std::max(1, image->height / factor));
        std::vector<unsigned char> downscaled(size_t(blurWidth) * blurHeight * 4);
        for (UINT y = 0; y < blurHeight; ++y)
            for (UINT x = 0; x < blurWidth; ++x) {
                long long sums[3]{};
                int count = 0;
                for (int dy = 0; dy < factor; ++dy)
                    for (int dx = 0; dx < factor; ++dx) {
                        const int sx = std::min<int>(image->width - 1, int(x) * factor + dx);
                        const int sy = std::min<int>(image->height - 1, int(y) * factor + dy);
                        const auto* p = &image->bgra[(size_t(sy) * image->width + sx) * 4];
                        sums[0] += p[2];
                        sums[1] += p[1];
                        sums[2] += p[0];
                        ++count;
                    }
                auto* dst = &downscaled[(size_t(y) * blurWidth + x) * 4];
                dst[0] = static_cast<unsigned char>(sums[2] / count);
                dst[1] = static_cast<unsigned char>(sums[1] / count);
                dst[2] = static_cast<unsigned char>(sums[0] / count);
                dst[3] = 255;
            }
        target->CreateBitmap(
            D2D1::SizeU(blurWidth, blurHeight), downscaled.data(), blurWidth * 4,
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
            blurBitmap.GetAddressOf());
    }
    const auto blurredBackdrop = [&](const D2D1_RECT_F& box) {
        if (!blurBitmap)
            return;
        const float maxX = float(blurWidth), maxY = float(blurHeight);
        const D2D1_RECT_F source =
            D2D1::RectF(std::clamp(box.left / 8.f, 0.f, maxX), std::clamp(box.top / 8.f, 0.f, maxY),
                        std::clamp(box.right / 8.f, 0.f, maxX), std::clamp(box.bottom / 8.f, 0.f, maxY));
        target->DrawBitmap(blurBitmap.Get(), box, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
    };
    // The blur spreads past the rebuilt row by a margin, so the transition from rebuilt surface to the
    // untouched capture runs through a purely blurred band instead of meeting a crisp edge.
    constexpr float blurSpread = 10.f;
    const auto blurredSpread = [&](const D2D1_RECT_F& box) {
        blurredBackdrop(D2D1::RectF(box.left - blurSpread, box.top - blurSpread, box.right + blurSpread,
                                    box.bottom + blurSpread));
    };
    // A soft rounded veil replaces every rebuilt region: eight stacked rounded rectangles from a wide,
    // mostly transparent outer layer to an opaque centre. The rim fades into the artwork with round
    // corners, so the covered area never shows a rectangular edge.
    const auto softVeil = [&](const D2D1_RECT_F& box, COLORREF start, COLORREF end) {
        const float width = box.right - box.left, height = box.bottom - box.top;
        const float feather = std::min({16.f, height * .45f, width * .2f});
        const float radius = std::min({12.f, height * .4f, width * .15f});
        constexpr int layers = 8;
        const D2D1_COLOR_F startColor = color(start);
        const D2D1_COLOR_F endColor = color(end);
        for (int layer = 0; layer < layers; ++layer) {
            const float inset = feather * float(layer) / float(layers - 1);
            const float alpha = .10f + .74f * float(layer + 1) / float(layers);
            const D2D1_ROUNDED_RECT rounded{
                D2D1::RectF(box.left + inset, box.top + inset, box.right - inset, box.bottom - inset), radius,
                radius};
            ComPtr<ID2D1RoundedRectangleGeometry> geometry;
            if (FAILED(factory->CreateRoundedRectangleGeometry(rounded, geometry.GetAddressOf())))
                continue;
            const D2D1_GRADIENT_STOP stops[2] = {
                {0.f, D2D1::ColorF(startColor.r, startColor.g, startColor.b, alpha)},
                {1.f, D2D1::ColorF(endColor.r, endColor.g, endColor.b, alpha)}};
            ComPtr<ID2D1GradientStopCollection> collection;
            if (FAILED(target->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                                                            collection.GetAddressOf())))
                continue;
            ComPtr<ID2D1LinearGradientBrush> gradient;
            const auto properties = D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(box.left, box.top), D2D1::Point2F(box.right, box.top));
            if (FAILED(target->CreateLinearGradientBrush(properties, collection.Get(),
                                                          gradient.GetAddressOf())))
                continue;
            target->FillGeometry(geometry.Get(), gradient.Get());
        }
    };
    // Rebuilt surface: the row carries its own BGRA pixels, rebuilt from the artwork around it. The rim
    // fades out so the substitute meets the untouched pixels, while the core stays opaque and removes the
    // source glyphs completely.
    const auto rebuiltSurface = [&](const EraseRow& row) -> bool {
        const int width = int(row.rect.right - row.rect.left);
        const int height = int(row.rect.bottom - row.rect.top);
        if (width <= 0 || height <= 0 || row.cover.size() < size_t(width) * size_t(height) * 4)
            return false;
        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(target->CreateBitmap(
                D2D1::SizeU(UINT(width), UINT(height)), row.cover.data(), UINT(width) * 4,
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                         D2D1_ALPHA_MODE_IGNORE)),
                bitmap.GetAddressOf())))
            return false;
        target->DrawBitmap(bitmap.Get(), rect(row.rect), 1.f);
        return true;
    };
    // Pass one: erase every source glyph row and rebuild the artwork underneath from the sampled column
    // colours. Line gaps, artwork and separators stay untouched.
    for (const auto& item : items) {
        if (item.suppressed)
            continue;
        if (item.eraseLines.empty()) {
            const auto box = rect(item.sourceRect);
            blurredSpread(box);
            softVeil(box, item.background, item.background);
            continue;
        }
        for (const auto& row : item.eraseLines) {
            if (rebuiltSurface(row))
                continue;
            const auto box = rect(row.rect);
            blurredBackdrop(rect(row.spread));
            softVeil(box, row.start, row.end);
        }
    }
    // Pass two: draw every translation above all surfaces so a later patch never covers earlier text.
    for (const auto& item : items) {
        if (item.suppressed || (!item.layout && item.columnTexts.empty()))
            continue;
        auto box = rect(item.rect);
        const float pad = item.padding;
        // Keep the original glyph colour, which was readable on this very artwork; the contrast is only
        // corrected when the rebuilt surface under the row would swallow it.
        COLORREF measured = item.background;
        if (!item.eraseLines.empty()) {
            long long sums[3]{};
            long long count = 0;
            for (const auto& row : item.eraseLines) {
                for (auto sample : {row.start, row.end}) {
                    sums[0] += GetRValue(sample);
                    sums[1] += GetGValue(sample);
                    sums[2] += GetBValue(sample);
                    ++count;
                }
            }
            measured = RGB(int(sums[0] / count), int(sums[1] / count), int(sums[2] / count));
        }
        const COLORREF textFill = ReadableTextColor(item.sourceColor.value_or(textColor), measured);
        brush->SetColor(color(textFill));
        // The inverse mode derives the rim from the rendered face colour, exactly as the reading
        // panel does, so the outline keeps its contrast on a dark cover as well as a light one.
        rimBrush->SetColor(color(settings.outlineColorMode == 1
                                     ? RGB(255 - GetRValue(textFill), 255 - GetGValue(textFill),
                                           255 - GetBValue(textFill))
                                     : settings.outlineColor));
        // The clip keeps the replacement on its surface while honouring the overhang the fit search
        // allows: a label may run past its source box, and drawing must not cut the letters the layout
        // already accepted.
        target->PushAxisAlignedClip(
            D2D1::RectF(box.left + pad, box.top + pad, box.right - pad + kOverhangAllowance, box.bottom - pad),
            D2D1_ANTIALIAS_MODE_ALIASED);
        // The rim renderer strokes the glyph outline before filling it. This path used to call
        // DrawTextLayout, whose fill-only painting left the configured outline unused.
        const float scale = dpi / 96.f;
        auto renderer = [&] {
            return CreateRimRenderer(target, factory.Get(), brush.Get(), rimBrush.Get(),
                                     settings.outlineWidth * scale, settings.outlineStyle, scale);
        };
        if (!item.columnTexts.empty()) {
            // One strip per column: each layout owns the source strip it replaces, so a spoken line
            // reads down the balloon exactly where the original columns stood.
            for (const auto& column : item.columnTexts) {
                if (!column.layout)
                    continue;
                const auto strip = rect(column.rect);
                auto rim = renderer();
                CheckHR(column.layout->Draw(nullptr, rim.Get(), strip.left, strip.top - item.scroll),
                        "layout");
            }
        } else {
            auto rim = renderer();
            CheckHR(item.layout->Draw(nullptr, rim.Get(), box.left + pad, box.top + pad - item.scroll),
                    "layout");
        }
        target->PopAxisAlignedClip();
        if (item.overflow) {
            brush->SetColor(color(RGB(67, 100, 235)));
            target->FillRectangle(D2D1::RectF(std::max(box.left, box.right - 18),
                                              std::max(box.top, box.bottom - 2), box.right, box.bottom),
                                  brush.Get());
        }
    }
    if (diagnostics)
        for (const auto& item : items) {
            brush->SetColor(color(RGB(25, 170, 80)));
            for (auto line : item.detectedLines)
                target->DrawRectangle(rect(line), brush.Get(), 1);
            brush->SetColor(color(RGB(230, 140, 0)));
            target->DrawRectangle(rect(item.sourceRect), brush.Get(), 1);
            brush->SetColor(color(item.suppressed ? RGB(220, 20, 40) : RGB(35, 90, 240)));
            target->DrawRectangle(rect(item.rect), brush.Get(), 1);
            if (!item.eraseLines.empty()) {
                brush->SetColor(color(RGB(160, 40, 190)));
                for (const auto& row : item.eraseLines)
                    target->DrawRectangle(rect(row.rect), brush.Get(), 1);
            }
        }
}
} // namespace sat
