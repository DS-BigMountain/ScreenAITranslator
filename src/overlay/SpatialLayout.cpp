#include "SpatialLayout.h"
#include "TextRim.h"
#include "capture/Capture.h"
#include "common/TextMetrics.h"
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
    bool bold{};
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
    // Tight single-line boxes can have more ink than background inside the box itself; check the
    // immediate 2 px perimeter so a dense glyph run never inverts foreground and background.
    {
        std::array<size_t, 4096> ringHist{};
        size_t ringSampled = 0;
        const int x0 = std::max(0, int(box.left) - 2), x1 = std::min(image.width, int(box.right) + 2);
        const int y0 = std::max(0, int(box.top) - 2), y1 = std::min(image.height, int(box.bottom) + 2);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                if (x >= box.left && x < box.right && y >= box.top && y < box.bottom) {
                    x = int(box.right) - 1;
                    continue;
                }
                ++ringHist[key(&image.bgra[(size_t(y) * image.width + x) * 4])];
                ++ringSampled;
            }
        if (ringSampled >= 8) {
            auto ringMode = std::max_element(ringHist.begin(), ringHist.end());
            if (*ringMode >= ringSampled * .45) {
                const int rb = int(ringMode - ringHist.begin());
                const int rbr = ((rb >> 8) & 15) * 16 + 8;
                const int rbg = ((rb >> 4) & 15) * 16 + 8;
                const int rbb = (rb & 15) * 16 + 8;
                if (*mode < sampled * .45 &&
                    std::abs(br - rbr) + std::abs(bg - rbg) + std::abs(bb - rbb) > 72) {
                    background = rb;
                    br = rbr;
                    bg = rbg;
                    bb = rbb;
                }
            }
        }
    }
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
    const bool reliableBackdrop = backgroundCount >= sampled * .50;
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
        br = backgroundMedian[2];
        bg = backgroundMedian[1];
        bb = backgroundMedian[0];
    }
    const int boxWidth = int(Width(box)), boxHeight = int(Height(box));
    // Wide passages across monitor photos or gradient cards shift their backdrop smoothly from left to
    // right, so a single global mode treats half the surface as ink. Measure per-tile backdrops first.
    constexpr int kTileWidth = 96;
    const int numTiles = boxWidth > 128 ? std::max(1, (boxWidth + kTileWidth - 1) / kTileWidth) : 1;
    struct TileBackdrop {
        int r{}, g{}, b{};
        bool reliable{};
    };
    std::vector<TileBackdrop> tiles(size_t(numTiles), {br, bg, bb, reliableBackdrop});
    if (numTiles > 1) {
        std::array<size_t, 4096> tileHist{};
        for (int t = 0; t < numTiles; ++t) {
            const int tx0 = box.left + t * kTileWidth;
            const int tx1 = std::min<int>(box.right, tx0 + kTileWidth);
            tileHist.fill(0);
            size_t tSampled = 0;
            for (int y = box.top; y < box.bottom; y += step)
                for (int x = tx0; x < tx1; x += step) {
                    ++tileHist[key(&image.bgra[(size_t(y) * image.width + x) * 4])];
                    ++tSampled;
                }
            if (!tSampled)
                continue;
            const auto tMode = std::max_element(tileHist.begin(), tileHist.end());
            const int tb = int(tMode - tileHist.begin());
            const int tbr = ((tb >> 8) & 15) * 16 + 8;
            const int tbg = ((tb >> 4) & 15) * 16 + 8;
            const int tbb = (tb & 15) * 16 + 8;
            std::array<std::array<size_t, 256>, 3> tCh{};
            size_t tBgCount = 0;
            for (int y = box.top; y < box.bottom; y += step)
                for (int x = tx0; x < tx1; x += step) {
                    const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                    if (std::abs(int(p[2]) - tbr) + std::abs(int(p[1]) - tbg) + std::abs(int(p[0]) - tbb) < 48) {
                        for (int c = 0; c < 3; ++c)
                            ++tCh[c][p[c]];
                        ++tBgCount;
                    }
                }
            if (tBgCount >= tSampled * .45) {
                std::array<int, 3> tMed{};
                for (int c = 0; c < 3; ++c) {
                    size_t acc = 0;
                    for (int v = 0; v < 256; ++v) {
                        acc += tCh[c][v];
                        if (acc >= (tBgCount + 1) / 2) {
                            tMed[c] = v;
                            break;
                        }
                    }
                }
                tiles[size_t(t)] = {tMed[2], tMed[1], tMed[0], true};
            }
        }
    }
    std::array<size_t, 4096> counts{}, rs{}, gs{}, bs{};
    size_t foreground = 0;
    std::vector<RECT> rows;
    int activeTop = -1, left = box.right, right = box.left, lastRow = -1;
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
        int currentRun = 0, maxRun = 0;
        // Gradient and shaded artwork shift the backdrop across a band, so one global mode leaves the
        // row detection blind on exactly the surfaces this pass exists for. Each row measures its own
        // median backdrop there. Flat artwork keeps the global/tile mode, which a dense bold row would
        // otherwise invert when the glyphs outnumber the surface pixels.
        int rowRed = br, rowGreen = bg, rowBlue = bb;
        if (!reliableBackdrop && numTiles == 1 && boxWidth > 0) {
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
            const auto& ref = numTiles > 1 ? tiles[size_t((x - box.left) / kTileWidth)]
                                           : TileBackdrop{rowRed, rowGreen, rowBlue, true};
            int distance =
                std::abs(int(p[2]) - ref.r) + std::abs(int(p[1]) - ref.g) + std::abs(int(p[0]) - ref.b);
            if (distance < 65) {
                currentRun = 0;
                continue;
            }
            ++currentRun;
            maxRun = std::max(maxRun, currentRun);
            ++rowCount;
            rowLeft = std::min(rowLeft, x);
            rowRight = x + 1;
            if (boxWidth > 0 && boxHeight > 0)
                inkMark[size_t(y - box.top) * size_t(boxWidth) + size_t(x - box.left)] = 1;
        }
        // Exclude solid horizontal rules and underlines from ink colour sampling and row bounds.
        const bool isHorizontalRule = boxWidth >= 32 && maxRun > std::max(28, int(boxWidth * .55));
        if (!isHorizontalRule && rowCount >= 2 && rowCount < Width(box) * .85) {
            for (int x = rowLeft; x < rowRight; ++x) {
                if (boxWidth > 0 && boxHeight > 0 &&
                    !inkMark[size_t(y - box.top) * size_t(boxWidth) + size_t(x - box.left)])
                    continue;
                const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                const auto& ref = numTiles > 1 ? tiles[size_t((x - box.left) / kTileWidth)]
                                               : TileBackdrop{br, bg, bb, true};
                const int localDist =
                    std::abs(int(p[2]) - ref.r) + std::abs(int(p[1]) - ref.g) + std::abs(int(p[0]) - ref.b);
                if (localDist < 65)
                    continue;
                const auto k = key(p);
                ++counts[k];
                rs[k] += p[2];
                gs[k] += p[1];
                bs[k] += p[0];
                ++foreground;
            }
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
    if (foreground >= 2 && foreground <= size_t(Width(box)) * Height(box) * .85) {
        double maxDistance = 0;
        for (int i = 0; i < 4096; ++i)
            if (counts[i] >= 2) {
                const double distance = std::abs(double(rs[i]) / counts[i] - br) +
                                        std::abs(double(gs[i]) / counts[i] - bg) +
                                        std::abs(double(bs[i]) / counts[i] - bb);
                maxDistance = std::max(maxDistance, distance);
            }
        double best = 0;
        int selected = -1;
        for (int i = 0; i < 4096; ++i)
            if (counts[i] >= 2) {
                const double distance = std::abs(double(rs[i]) / counts[i] - br) +
                                        std::abs(double(gs[i]) / counts[i] - bg) +
                                        std::abs(double(bs[i]) / counts[i] - bb);
                if (distance < maxDistance * .58)
                    continue;
                const double weight = double(counts[i]) * distance * distance * distance;
                if (weight > best) {
                    best = weight;
                    selected = i;
                }
            }
        if (selected >= 0) {
            int cr = int(rs[selected] / counts[selected]);
            int cg = int(gs[selected] / counts[selected]);
            int cb = int(bs[selected] / counts[selected]);
            const int bgChroma = std::max({br, bg, bb}) - std::min({br, bg, bb});
            const int fgChroma = std::max({cr, cg, cb}) - std::min({cr, cg, cb});
            if (bgChroma <= 20 && fgChroma <= 24) {
                const int neutral = std::clamp(int(.299 * cr + .587 * cg + .114 * cb + .5), 0, 255);
                cr = cg = cb = neutral;
            }
            result.color = RGB(cr, cg, cb);
        }
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
    bool cjk{}, upper{}, lower{}, ascender{}, descender{}, digit{}, complexScript{}, stackedMarks{};
};
TextTraits AnalyzeText(const std::string& text) {
    TextTraits traits;
    for (size_t i = 0; i < text.size();) {
        const char32_t code = NextCodePoint(text, i);
        if ((code >= 0x2E80 && code <= 0x9FFF) || (code >= 0xAC00 && code <= 0xD7AF) ||
            (code >= 0xF900 && code <= 0xFAFF) || (code >= 0xFF00 && code <= 0xFF60)) {
            traits.cjk = true;
        } else if (code >= U'A' && code <= U'Z') {
            traits.upper = true;
        } else if (code >= U'0' && code <= U'9') {
            traits.digit = true;
        } else if (code >= U'a' && code <= U'z') {
            traits.lower = true;
            const auto letter = wchar_t(code);
            if (std::wstring_view(L"bdfhklt").find(letter) != std::wstring_view::npos)
                traits.ascender = true;
            if (std::wstring_view(L"gjpqy").find(letter) != std::wstring_view::npos)
                traits.descender = true;
        } else if ((code >= 0x00C0 && code <= 0x00D6) || (code >= 0x00D8 && code <= 0x00DE) ||
                   (code >= 0x0391 && code <= 0x03AB) || (code >= 0x0400 && code <= 0x042F)) {
            traits.upper = true;
        } else if ((code >= 0x00DF && code <= 0x024F) || (code >= 0x1E00 && code <= 0x1EFF) ||
                   (code >= 0x03AC && code <= 0x03CE) || (code >= 0x0430 && code <= 0x04FF)) {
            traits.lower = true;
            if (code >= 0x00E0 && code <= 0x00FE)
                traits.ascender = true;
            if (code == 0x00E7 || code == 0x00FE || code == 0x0434 || code == 0x0440 || code == 0x0443 ||
                code == 0x0444 || code == 0x0446 || code == 0x0449 || code == 0x03B3 || code == 0x03B7 ||
                code == 0x03BC || code == 0x03C1 || code == 0x03C6 || code == 0x03C7 || code == 0x03C8 ||
                code == 0x03C2)
                traits.descender = true;
        } else if ((code >= 0x0600 && code <= 0x06FF) || (code >= 0x0750 && code <= 0x077F) ||
                   (code >= 0x08A0 && code <= 0x08FF) || (code >= 0x0900 && code <= 0x097F) ||
                   (code >= 0x0E00 && code <= 0x0E7F)) {
            traits.complexScript = true;
            if (IsCombiningMark(code) || code == 0x0E1B || code == 0x0E1D || code == 0x0E1F ||
                code == 0x0E2C || code == 0x0E0E || code == 0x0E0F || code == 0x0E24 || code == 0x0E26)
                traits.stackedMarks = true;
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
    if (traits.complexScript)
        return traits.stackedMarks ? .92f : .80f;
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
    if (traits.complexScript)
        return .60f;
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
    if (lines.size() == 1)
        return Width(lines[0]) > 0 && Height(lines[0]) * 5 > Width(lines[0]) * 6;
    size_t tall = 0;
    LONG maxH = 0;
    for (const auto& line : lines) {
        if (Width(line) > 0 && Height(line) * 5 > Width(line) * 6)
            ++tall;
        maxH = std::max(maxH, LONG(Height(line)));
    }
    return tall >= 1 && tall * 2 >= lines.size() && maxH * 10 >= Height(box) * 6;
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
            std::wstring_view(L"。！？.!?】]؟؛۔।॥").find(text[last]) == std::wstring_view::npos)
            return false;
        ++complete;
        start = end + 1;
    }
    return complete > 0;
}
bool HasTargetScript(std::wstring_view text) {
    for (wchar_t ch : text) {
        if ((ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3400 && ch <= 0x4DBF) ||
            (ch >= 0xF900 && ch <= 0xFAFF) || (ch >= 0x3040 && ch <= 0x30FF) ||
            (ch >= 0xAC00 && ch <= 0xD7AF))
            return true;
    }
    return false;
}
std::wstring NormalizeAsciiAlnum(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        if (ch >= L'A' && ch <= L'Z')
            out.push_back(wchar_t(ch - L'A' + L'a'));
        else if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9'))
            out.push_back(ch);
    }
    return out;
}
bool ShouldKeepOriginalPixels(const Segment& s, bool cjkTarget) {
    if (!s.sourcePixels.has_value() || s.translated.empty())
        return false;
    const std::wstring wTrans = Wide(s.translated);
    if (cjkTarget && HasTargetScript(wTrans))
        return false;
    const std::wstring normTrans = NormalizeAsciiAlnum(wTrans);
    const std::wstring normOrig = NormalizeAsciiAlnum(Wide(s.original));
    if (cjkTarget && normTrans.empty())
        return true;
    if (!normTrans.empty() && normTrans == normOrig)
        return true;
    if (cjkTarget && normTrans.size() <= 2 && wTrans.size() <= 4)
        return true;
    return false;
}
DWRITE_TEXT_METRICS MeasureTextLayout(IDWriteFactory* factory, const std::wstring& font,
                                      const std::wstring& body, float fontSize, float lineSpacing,
                                      float maxWidth, bool bold, bool centerAligned, bool wrap,
                                      ComPtr<IDWriteTextLayout>& layout) {
    ComPtr<IDWriteTextFormat> format;
    const DWRITE_FONT_WEIGHT fontWeight = bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    CheckHR(factory->CreateTextFormat(font.c_str(), nullptr, fontWeight, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, fontSize, L"zh-CN", &format),
            "layout");
    CheckHR(format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP),
            "layout");
    const bool rtl = HasRtlScript(body);
    const bool complexVertical = HasComplexVerticalScript(body);
    if (rtl)
        CheckHR(format->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT), "layout");
    CheckHR(format->SetTextAlignment(centerAligned ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                   : DWRITE_TEXT_ALIGNMENT_LEADING),
            "layout");
    const float effectiveSpacing = complexVertical ? std::max(lineSpacing, fontSize * 1.35f) : lineSpacing;
    const float baseline = complexVertical ? fontSize * .95f : fontSize * .8f;
    CheckHR(format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, effectiveSpacing, baseline),
            "layout");
    layout.Reset();
    CheckHR(factory->CreateTextLayout(body.c_str(), UINT32(body.size()), format.Get(), maxWidth, 100000.f,
                                      &layout),
            "layout");
    if (!rtl && !complexVertical) {
        ComPtr<IDWriteTextLayout1> spaced;
        if (SUCCEEDED(layout.As(&spaced)))
            spaced->SetCharacterSpacing(0.f, fontSize * .02f, 0.f,
                                        DWRITE_TEXT_RANGE{0, UINT32(body.size())});
    }
    DWRITE_TEXT_METRICS metrics{};
    CheckHR(layout->GetMetrics(&metrics), "layout");
    return metrics;
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
    const bool cjkTarget = settings.screenTargetLanguage == "zh-Hans" ||
                           settings.screenTargetLanguage == "zh-Hant" ||
                           settings.screenTargetLanguage == "ja" ||
                           settings.screenTargetLanguage == "ko";
    for (size_t i = 0; i < output.size(); ++i) {
        const auto& s = segments[i];
        if (s.positionUnavailable) {
            output[i].suppressed = true;
            continue;
        }
        auto r = s.sourcePixels.value_or(MapBox(s.box, image));
        RECT clipped{};
        IntersectRect(&clipped, &r, &bounds);
        if (ShouldKeepOriginalPixels(s, cjkTarget)) {
            output[i].suppressed = true;
            if (Valid(clipped))
                protectedRegions.push_back(clipped);
            continue;
        }
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
        } else if (!columns[i] && s.sourcePixels && detectedRows &&
                   pixels.lines.size() == s.sourceLines.size() &&
                   pixels.lineHeight >= lineHeights[i] * .65f &&
                   pixels.lineHeight <= lineHeights[i] * .95f && !AnalyzeText(s.original).cjk) {
            // The detector tightens its rows to the ink, yet shaded artwork can still hide part of a
            // glyph row. A measurement inside a plausible ink share refines the band; one far outside
            // it means the pixel pass lost the row and the detector band stands.
            lineHeights[i] = pixels.lineHeight;
        }
        if (settings.matchTextColor)
            output[i].sourceColor = pixels.color;
        output[i].bold = false;
        coreHeights[i] = pixels.xHeight;
        textColors[i] = pixels.color;
        surfaceColors[i] = pixels.background;
        output[i].sourceRect = clipped;
        protectedRegions.push_back(clipped);
    }
    // Harmonize detected glyph colours across peer segments on the same surface so identical
    // paragraphs, list titles and captions never drift to different shades of grey or lose their colour.
    {
        auto colorDist = [](COLORREF a, COLORREF b) {
            return std::abs(int(GetRValue(a)) - int(GetRValue(b))) +
                   std::abs(int(GetGValue(a)) - int(GetGValue(b))) +
                   std::abs(int(GetBValue(a)) - int(GetBValue(b)));
        };
        auto lum = [](COLORREF c) {
            return int(.299 * GetRValue(c) + .587 * GetGValue(c) + .114 * GetBValue(c) + .5);
        };
        auto chroma = [](COLORREF c) {
            const int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
            return std::max({r, g, b}) - std::min({r, g, b});
        };
        std::vector<COLORREF> resolvedBg(output.size(), RGB(255, 255, 255));
        for (size_t i = 0; i < output.size(); ++i) {
            if (output[i].suppressed || !Valid(output[i].sourceRect))
                continue;
            resolvedBg[i] = surfaceColors[i].value_or(AnalyzeBackground(image, output[i].sourceRect).color);
        }
        for (size_t i = 0; i < output.size(); ++i) {
            if (output[i].suppressed || !Valid(output[i].sourceRect) || textColors[i].has_value())
                continue;
            if (segments[i].translated.empty() && segments[i].original.empty())
                continue;
            int bestDist = INT_MAX;
            std::optional<COLORREF> inherited;
            for (size_t j = 0; j < output.size(); ++j) {
                if (i == j || output[j].suppressed || !Valid(output[j].sourceRect) || !textColors[j].has_value())
                    continue;
                if (colorDist(resolvedBg[i], resolvedBg[j]) > 36)
                    continue;
                const float hi = std::max(1.f, lineHeights[i]), hj = std::max(1.f, lineHeights[j]);
                if (std::max(hi, hj) > std::min(hi, hj) * 1.45f)
                    continue;
                const int dx = std::abs(int(output[i].sourceRect.left - output[j].sourceRect.left));
                const int dy = std::abs(int(output[i].sourceRect.top - output[j].sourceRect.top));
                const int d = dx * 2 + dy;
                if (d < bestDist) {
                    bestDist = d;
                    inherited = textColors[j];
                }
            }
            textColors[i] = inherited.value_or(lum(resolvedBg[i]) >= 140 ? RGB(32, 32, 32) : RGB(242, 242, 242));
            if (settings.matchTextColor)
                output[i].sourceColor = textColors[i];
        }
        std::vector<std::optional<COLORREF>> harmonized = textColors;
        for (size_t i = 0; i < output.size(); ++i) {
            if (output[i].suppressed || !Valid(output[i].sourceRect) || !textColors[i].has_value())
                continue;
            if (segments[i].translated.empty() && segments[i].original.empty())
                continue;
            const COLORREF ci = *textColors[i];
            const int bgLumI = lum(resolvedBg[i]);
            const int lumI = lum(ci);
            const bool neutralI = chroma(ci) <= 28;
            const bool darkOnLightI = lumI < bgLumI;
            std::vector<int> rs, gs, bs;
            for (size_t j = 0; j < output.size(); ++j) {
                if (output[j].suppressed || !Valid(output[j].sourceRect) || !textColors[j].has_value())
                    continue;
                if (segments[j].translated.empty() && segments[j].original.empty())
                    continue;
                if (colorDist(resolvedBg[i], resolvedBg[j]) > 36)
                    continue;
                const float hi = std::max(1.f, lineHeights[i]), hj = std::max(1.f, lineHeights[j]);
                const bool sameTier = std::max(hi, hj) <= std::min(hi, hj) * 1.28f;
                const bool sameColumnParagraphs =
                    output[i].detectedLines.size() >= 2 && output[j].detectedLines.size() >= 2 &&
                    std::abs(int(output[i].sourceRect.left - output[j].sourceRect.left)) <= 28;
                if (!sameTier && !sameColumnParagraphs)
                    continue;
                const COLORREF cj = *textColors[j];
                const int lumJ = lum(cj);
                const bool neutralJ = chroma(cj) <= 28;
                const bool darkOnLightJ = lumJ < lum(resolvedBg[j]);
                bool compatible = false;
                if (neutralI && neutralJ && darkOnLightI == darkOnLightJ) {
                    const int maxLumDiff = sameColumnParagraphs ? 68 : 38;
                    compatible = std::abs(lumI - lumJ) <= maxLumDiff;
                } else if (!neutralI && !neutralJ) {
                    compatible = colorDist(ci, cj) <= 64;
                }
                if (compatible) {
                    rs.push_back(GetRValue(cj));
                    gs.push_back(GetGValue(cj));
                    bs.push_back(GetBValue(cj));
                }
            }
            if (rs.size() >= 2) {
                std::sort(rs.begin(), rs.end());
                std::sort(gs.begin(), gs.end());
                std::sort(bs.begin(), bs.end());
                // On light surfaces, anti-aliasing only biases dark strokes upward toward white; the
                // lower-third quantile recovers the true dark ink shared by the peer group.
                const size_t pick = (neutralI && darkOnLightI && bgLumI >= 180) ? rs.size() / 3 : rs.size() / 2;
                harmonized[i] = RGB(rs[pick], gs[pick], bs[pick]);
            }
        }
        for (size_t i = 0; i < output.size(); ++i) {
            if (harmonized[i].has_value()) {
                textColors[i] = harmonized[i];
                if (settings.matchTextColor)
                    output[i].sourceColor = harmonized[i];
            }
        }
    }
    struct SegmentPrep {
        bool active{};
        bool column{};
        bool hasCore{};
        bool isProse{};
        bool isShortLabel{};
        bool wrapText{};
        size_t allowedRows{};
        float detectedHeight{};
        float preferred{};
        float width{};
        float height{};
        float pitchRatio{1.15f};
        std::wstring text;
        std::vector<RECT> strips;
        std::vector<LONG> stripBottoms;
        std::vector<std::wstring> glyphs;
    };
    std::vector<SegmentPrep> preps(output.size());
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
        const int margin =
            detectedHeight > 0 ? pad + std::clamp(int(std::ceil(detectedHeight * .18f)), 2, 7) : pad;
        int symExpand = pad;
        if (!columns[i] && !coverOnly) {
            const std::wstring wTrans = Wide(segment.translated);
            const std::wstring wOrig = Wide(segment.original);
            constexpr std::wstring_view kProsePunct =
                L"。，、；：？！“”‘’「」『』【】（）()[].,;:?!\"؟؛،।॥";
            const bool hasProsePunct = wTrans.find_first_of(kProsePunct) != std::wstring::npos ||
                                       wOrig.find_first_of(kProsePunct) != std::wstring::npos;
            const bool isShortLabel = !hasProsePunct && !wTrans.empty() && wTrans.size() <= 14 &&
                                      wOrig.size() <= 32;
            const bool rtlSource = HasRtlScript(wOrig);
            bool hasLeftAlignedPeer = false;
            bool centerAlignedHeadingPair = false;
            for (size_t j = 0; j < output.size(); ++j) {
                if (i == j || output[j].suppressed || !Valid(output[j].sourceRect))
                    continue;
                const RECT other = output[j].sourceRect;
                const int vGap = other.top >= source.bottom
                                     ? int(other.top - source.bottom)
                                     : (source.top >= other.bottom ? int(source.top - other.bottom) : 0);
                const int edgeDiff = rtlSource ? std::abs(int(source.right - other.right))
                                               : std::abs(int(source.left - other.left));
                if (edgeDiff <= std::max(14, int(detectedHeight * .9f)) &&
                    vGap <= std::max(120, int(detectedHeight * 6.f))) {
                    bool sameSurface = true;
                    if (surfaceColors[i].has_value() && surfaceColors[j].has_value()) {
                        const COLORREF ca = *surfaceColors[i], cb = *surfaceColors[j];
                        const int cDiff = std::abs(int(GetRValue(ca)) - int(GetRValue(cb))) +
                                          std::abs(int(GetGValue(ca)) - int(GetGValue(cb))) +
                                          std::abs(int(GetBValue(ca)) - int(GetBValue(cb)));
                        sameSurface = cDiff <= 45;
                    }
                    if (sameSurface)
                        hasLeftAlignedPeer = true;
                }
                if (other.right <= source.left || other.left >= source.right)
                    continue;
                const bool startsLowercaseAscii = !wOrig.empty() && wOrig[0] >= L'a' && wOrig[0] <= L'z';
                const bool headingHeightJump =
                    (coreHeights[i] > 0.f && coreHeights[j] > 0.f)
                        ? (coreHeights[i] >= coreHeights[j] * 1.30f)
                        : (lineHeights[j] > 0.f && detectedHeight >= lineHeights[j] * 1.45f);
                if (item.detectedLines.size() == 1 && !hasProsePunct && !startsLowercaseAscii &&
                    wOrig.size() <= 40 && detectedHeight >= 22.f &&
                    other.top >= source.bottom - 2 && vGap <= std::max(20, int(detectedHeight * 1.4f)) &&
                    headingHeightJump) {
                    item.bold = true;
                }
                if (isShortLabel && item.detectedLines.size() == 1 && vGap <= std::max(18, int(detectedHeight * 1.4f))) {
                    const int centerDiff =
                        std::abs(int((source.left + source.right) - (other.left + other.right))) / 2;
                    const int leftShift = std::abs(int(source.left - other.left));
                    const int rightShift = std::abs(int(source.right - other.right));
                    const int widthDiff = std::abs(int(Width(source) - Width(other)));
                    if (centerDiff <= 4 && widthDiff >= std::max(24, int(detectedHeight * 1.6f)) &&
                        leftShift >= 12 && rightShift >= 12 && std::abs(leftShift - rightShift) <= 5) {
                        centerAlignedHeadingPair = true;
                    }
                }
            }
            bool multiLineCentered = false;
            if (isShortLabel && !hasLeftAlignedPeer && item.detectedLines.size() == 2) {
                const float boxCenter = .5f * float(source.left + source.right);
                const float boxW = float(Width(source));
                int narrowCentered = 0;
                bool allCentered = boxW > 0.f;
                for (const auto& line : item.detectedLines) {
                    const float lineCenter = .5f * float(line.left + line.right);
                    if (std::abs(lineCenter - boxCenter) > std::max(3.f, boxW * .04f)) {
                        allCentered = false;
                        break;
                    }
                    if (float(Width(line)) <= boxW * .78f) {
                        const float leftGap = float(line.left - source.left);
                        const float rightGap = float(source.right - line.right);
                        if (leftGap >= detectedHeight * .8f && rightGap >= detectedHeight * .8f &&
                            std::abs(leftGap - rightGap) <= std::max(3.f, detectedHeight * .20f))
                            ++narrowCentered;
                        else {
                            allCentered = false;
                            break;
                        }
                    }
                }
                multiLineCentered = allCentered && narrowCentered >= 1;
            }
            bool buttonCentered = false;
            if (isShortLabel && !hasLeftAlignedPeer && item.detectedLines.size() <= 1 && wTrans.size() <= 10) {
                const int y0 = std::clamp(int(source.top + Height(source) / 4), 0, image.height - 1);
                const int y1 = std::clamp(int(source.bottom - Height(source) / 4), y0 + 1, image.height);
                const int maxScan = std::max(24, int(Width(source) * 2));
                auto columnDiff = [&](int sx) -> int {
                    if (sx < 0 || sx >= image.width)
                        return 999;
                    for (size_t j = 0; j < output.size(); ++j) {
                        if (j == i || output[j].suppressed || !Valid(output[j].sourceRect))
                            continue;
                        const RECT o = output[j].sourceRect;
                        if (sx >= o.left && sx < o.right && o.bottom > source.top && o.top < source.bottom)
                            return 999;
                    }
                    int diffSum = 0, samples = 0;
                    const int stepY = std::max(1, (y1 - y0) / 6);
                    for (int sy = y0; sy < y1; sy += stepY) {
                        const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                        diffSum += std::abs(int(p[2]) - GetRValue(item.background)) +
                                   std::abs(int(p[1]) - GetGValue(item.background)) +
                                   std::abs(int(p[0]) - GetBValue(item.background));
                        ++samples;
                    }
                    return samples > 0 ? (diffSum / samples) : 999;
                };
                int leftClear = 0;
                while (leftClear < maxScan && columnDiff(int(source.left) - 2 - leftClear) <= 28)
                    ++leftClear;
                const bool leftHitBorder =
                    leftClear < maxScan && columnDiff(int(source.left) - 2 - leftClear) >= 48;
                int rightClear = 0;
                while (rightClear < maxScan && columnDiff(int(source.right) + 1 + rightClear) <= 28)
                    ++rightClear;
                const bool rightHitBorder =
                    rightClear < maxScan && columnDiff(int(source.right) + 1 + rightClear) >= 48;
                if (leftHitBorder && rightHitBorder && leftClear >= 8 && rightClear >= 8 &&
                    std::abs(leftClear - rightClear) <= std::max(3, int(.08f * float(leftClear + rightClear)))) {
                    buttonCentered = true;
                    symExpand = std::max(pad, std::min({leftClear - 2, rightClear - 2, int(kOverhangAllowance * .5f)}));
                }
            }
            item.centerAligned = !hasLeftAlignedPeer &&
                                 (multiLineCentered || centerAlignedHeadingPair || buttonCentered);
        }
        RECT area{std::max(0L, source.left - symExpand), std::max(0L, source.top - margin),
                  std::min<LONG>(image.width, source.right + symExpand),
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
                if (other.right <= source.left + 8 && other.left < source.left)
                    area.left = std::max(area.left, std::min(source.right - 1, (source.left + other.right + 1) / 2));
                if (other.left >= source.right - 8 && other.right > source.right)
                    area.right = std::min(area.right, std::max(source.left + 1, (source.right + other.left) / 2));
            }
        }
        // Do not let padding expand across table borders or cell dividers, and bound right-hand
        // overhang against adjacent columns or borders.
        const auto isHBorderRow = [&](LONG y, LONG x0, LONG x1) -> bool {
            if (y < 0 || y >= image.height || x1 - x0 < 20)
                return false;
            const int br = GetRValue(item.background), bg = GetGValue(item.background),
                      bb = GetBValue(item.background);
            int hits = 0, total = 0;
            const int step = std::max(1, int((x1 - x0) / 80));
            for (LONG x = x0; x < x1; x += step) {
                const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                if (std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb) >= 48)
                    ++hits;
                ++total;
            }
            return total > 0 && hits * 10 >= total * 7;
        };
        const auto isVBorderCol = [&](LONG x, LONG y0, LONG y1) -> bool {
            if (x < 0 || x >= image.width || y1 - y0 < 10)
                return false;
            const int br = GetRValue(item.background), bg = GetGValue(item.background),
                      bb = GetBValue(item.background);
            int hits = 0, total = 0;
            const int step = std::max(1, int((y1 - y0) / 40));
            for (LONG y = y0; y < y1; y += step) {
                const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                if (std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb) >= 48)
                    ++hits;
                ++total;
            }
            return total > 0 && hits * 10 >= total * 7;
        };
        if (!coverOnly) {
            for (LONG y = source.top - 1; y >= area.top; --y)
                if (isHBorderRow(y, source.left, source.right)) {
                    area.top = std::min(source.top, y + 1);
                    break;
                }
            for (LONG y = source.bottom; y < area.bottom; ++y)
                if (isHBorderRow(y, source.left, source.right)) {
                    area.bottom = std::max(source.bottom, y);
                    break;
                }
            for (LONG x = source.left - 1; x >= area.left; --x)
                if (isVBorderCol(x, source.top, source.bottom)) {
                    area.left = std::min(source.left, x + 1);
                    break;
                }
            for (LONG x = source.right; x < area.right; ++x)
                if (isVBorderCol(x, source.top, source.bottom)) {
                    area.right = std::max(source.right, x);
                    break;
                }
        }
        if (item.centerAligned) {
            const LONG leftMargin = source.left - area.left;
            const LONG rightMargin = area.right - source.right;
            const LONG symMargin = std::max(0L, std::min(leftMargin, rightMargin));
            area.left = source.left - symMargin;
            area.right = source.right + symMargin;
        }
        float maxOverhang = kOverhangAllowance;
        if (!coverOnly && segment.sourcePixels) {
            for (const auto& other : protectedRegions) {
                if (!Valid(other) || EqualRect(&other, &source) || Conflicts(source, other))
                    continue;
                if (other.top < area.bottom && other.bottom > area.top &&
                    other.left >= source.right - 8 && other.right > source.right)
                    maxOverhang = std::min(maxOverhang, float(std::max(0L, other.left - area.right)));
            }
            const LONG scanEnd = std::min<LONG>(image.width, area.right + LONG(kOverhangAllowance));
            for (LONG x = area.right; x < scanEnd; ++x)
                if (isVBorderCol(x, source.top, source.bottom)) {
                    maxOverhang = std::min(maxOverhang, float(std::max(0L, x - area.right)));
                    break;
                }
        }
        item.maxOverhang = maxOverhang;
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
                std::vector<int> blockSpread(size_t(blocksX) * size_t(blocksY), 4);
                const COLORREF glyphColor = textColors[i].value_or(RGB(0, 0, 0));
                const bool hasGlyphColor = textColors[i].has_value();
                {
                    std::vector<int> histogram(256);
                    std::vector<int> reds, greens, blues;
                    std::vector<int> closeReds, closeGreens, closeBlues;
                    std::vector<int> smoothReds, smoothGreens, smoothBlues;
                    for (int by = 0; by < blocksY; ++by)
                        for (int bx = 0; bx < blocksX; ++bx) {
                            reds.clear();
                            greens.clear();
                            blues.clear();
                            closeReds.clear();
                            closeGreens.clear();
                            closeBlues.clear();
                            smoothReds.clear();
                            smoothGreens.clear();
                            smoothBlues.clear();
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
                                    const int distSurface = std::abs(int(p[2]) - GetRValue(surfaceRef)) +
                                                            std::abs(int(p[1]) - GetGValue(surfaceRef)) +
                                                            std::abs(int(p[0]) - GetBValue(surfaceRef));
                                    const int distGlyph =
                                        hasGlyphColor ? (std::abs(int(p[2]) - GetRValue(glyphColor)) +
                                                         std::abs(int(p[1]) - GetGValue(glyphColor)) +
                                                         std::abs(int(p[0]) - GetBValue(glyphColor)))
                                                      : 999;
                                    if (distSurface < 96 && distGlyph >= 42) {
                                        closeReds.push_back(p[2]);
                                        closeGreens.push_back(p[1]);
                                        closeBlues.push_back(p[0]);
                                    }
                                    const auto* pl =
                                        &image.bgra[(size_t(sy) * image.width + std::max(0, sx - 1)) * 4];
                                    const auto* pr = &image.bgra[(size_t(sy) * image.width +
                                                                  std::min(image.width - 1, sx + 1)) *
                                                                 4];
                                    const auto* pu =
                                        &image.bgra[(size_t(std::max(0, sy - 1)) * image.width + sx) * 4];
                                    const auto* pd =
                                        &image.bgra[(size_t(std::min(image.height - 1, sy + 1)) * image.width +
                                                     sx) *
                                                    4];
                                    const auto diffL1 = [](const unsigned char* a, const unsigned char* b) {
                                        return std::abs(int(a[2]) - int(b[2])) +
                                               std::abs(int(a[1]) - int(b[1])) +
                                               std::abs(int(a[0]) - int(b[0]));
                                    };
                                    const int grad =
                                        std::max({diffL1(p, pl), diffL1(p, pr), diffL1(p, pu), diffL1(p, pd)});
                                    if (grad <= 24 && distSurface < 110 && distGlyph >= 54) {
                                        smoothReds.push_back(p[2]);
                                        smoothGreens.push_back(p[1]);
                                        smoothBlues.push_back(p[0]);
                                    }
                                }
                            if (smoothReds.size() >= 6) {
                                reds.swap(smoothReds);
                                greens.swap(smoothGreens);
                                blues.swap(smoothBlues);
                            } else if (closeReds.size() >= 6) {
                                reds.swap(closeReds);
                                greens.swap(closeGreens);
                                blues.swap(closeBlues);
                            } else if (closeReds.size() < 4) {
                                reds.assign(1, GetRValue(surfaceRef));
                                greens.assign(1, GetGValue(surfaceRef));
                                blues.assign(1, GetBValue(surfaceRef));
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
                            const int medR = medianOf(reds);
                            const int medG = medianOf(greens);
                            const int medB = medianOf(blues);
                            blockRed[block] = medR;
                            blockGreen[block] = medG;
                            blockBlue[block] = medB;
                            long long spreadSum = 0;
                            for (size_t k = 0; k < reds.size(); ++k)
                                spreadSum += std::abs(reds[k] - medR) + std::abs(greens[k] - medG) +
                                             std::abs(blues[k] - medB);
                            blockSpread[block] =
                                reds.empty() ? 4 : int(std::clamp<long long>(spreadSum / long(reds.size()), 1, 24));
                        }
                }
                std::vector<unsigned char> glyphMask(size_t(coverWidth) * size_t(coverHeight), 0);
                std::vector<unsigned char> rawInk(size_t(coverWidth) * size_t(coverHeight), 0);
                std::vector<unsigned char> structMask(size_t(coverWidth) * size_t(coverHeight), 0);
                // Repair covers both the detected line bands and the paragraph box itself so low-contrast
                // secondary rows or glyphs slightly outside tightened line bands never survive underneath.
                const auto insideDetectedLine = [&](int sx, int sy) {
                    if (sx >= source.left - grow && sx <= source.right + grow && sy >= source.top - grow &&
                        sy <= source.bottom + grow)
                        return true;
                    if (item.detectedLines.empty())
                        return true;
                    return std::any_of(item.detectedLines.begin(), item.detectedLines.end(),
                                       [&](RECT line) {
                                           const int hGrow = columns[i] ? 2 : grow;
                                           return sx >= line.left - hGrow && sx <= line.right + hGrow &&
                                                  sy >= line.top - (grow + 2) && sy <= line.bottom + (grow + 2);
                                       });
                };
                for (int y = 0; y < coverHeight; ++y) {
                    for (int x = 0; x < coverWidth; ++x) {
                        const int sx = std::clamp(int(region.left) + x, 0, image.width - 1);
                        const int sy = std::clamp(int(region.top) + y, 0, image.height - 1);
                        const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                        const size_t block = size_t(y / blockSize) * size_t(blocksX) + size_t(x / blockSize);
                        const int meanR = blockRed[block];
                        const int meanG = blockGreen[block];
                        const int meanB = blockBlue[block];
                        const int departure = std::abs(int(p[2]) - meanR) + std::abs(int(p[1]) - meanG) +
                                              std::abs(int(p[0]) - meanB);
                        const int threshold = std::clamp(blockSpread[block] * 3 + 16, 18, 52);
                        const bool closerToGlyph =
                            hasGlyphColor && departure > 14 &&
                            (std::abs(int(p[2]) - GetRValue(glyphColor)) +
                             std::abs(int(p[1]) - GetGValue(glyphColor)) +
                             std::abs(int(p[0]) - GetBValue(glyphColor))) < departure;
                        const unsigned char active = (departure > threshold || closerToGlyph) ? 1 : 0;
                        rawInk[size_t(y) * coverWidth + x] = active;
                        if (active && insideDetectedLine(sx, sy))
                            glyphMask[size_t(y) * coverWidth + x] = 1;
                    }
                }
                // Protect structural horizontal/vertical borders, table grid lines and form rules so
                // glyph erasure never punches holes in table outlines or underlines outside text rows.
                const auto insideOtherRegion = [&](int sx, int sy) -> bool {
                    for (const auto& other : protectedRegions) {
                        if (!Valid(other) || EqualRect(&other, &source))
                            continue;
                        if (sx >= other.left - 1 && sx < other.right + 1 &&
                            sy >= other.top - 1 && sy < other.bottom + 1)
                            return true;
                    }
                    return false;
                };
                const auto outsideImgInk = [&](int sx, int sy) -> bool {
                    if (sx < 0 || sx >= image.width || sy < 0 || sy >= image.height)
                        return false;
                    if (insideOtherRegion(sx, sy))
                        return false;
                    const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                    const int dSurf = std::abs(int(p[2]) - GetRValue(surfaceRef)) +
                                      std::abs(int(p[1]) - GetGValue(surfaceRef)) +
                                      std::abs(int(p[0]) - GetBValue(surfaceRef));
                    const int dGlyph = hasGlyphColor ? (std::abs(int(p[2]) - GetRValue(glyphColor)) +
                                                        std::abs(int(p[1]) - GetGValue(glyphColor)) +
                                                        std::abs(int(p[0]) - GetBValue(glyphColor)))
                                                     : 999;
                    return dSurf >= 36 || dGlyph < dSurf;
                };
                for (int y = 0; y < coverHeight; ++y) {
                    const int sy = int(region.top) + y;
                    const bool outsideSourceY = sy < source.top || sy >= source.bottom;
                    int x = 0;
                    while (x < coverWidth) {
                        if (!rawInk[size_t(y) * coverWidth + x]) {
                            ++x;
                            continue;
                        }
                        const int x0 = x;
                        int gap = 0;
                        while (x < coverWidth) {
                            if (rawInk[size_t(y) * coverWidth + x]) {
                                gap = 0;
                                ++x;
                            } else if (gap == 0 && x + 1 < coverWidth && rawInk[size_t(y) * coverWidth + x + 1]) {
                                gap = 1;
                                ++x;
                            } else {
                                break;
                            }
                        }
                        const int x1 = (x > x0 && !rawInk[size_t(y) * coverWidth + x - 1]) ? x - 1 : x;
                        const int runLen = x1 - x0;
                        if (runLen < 12)
                            continue;
                        int thinCols = 0;
                        for (int k = x0; k < x1; ++k) {
                            int up = y, down = y;
                            while (up > 0 && y - up < 5 && rawInk[size_t(up - 1) * coverWidth + k])
                                --up;
                            while (down + 1 < coverHeight && down - y < 5 &&
                                   rawInk[size_t(down + 1) * coverWidth + k])
                                ++down;
                            if (down - up + 1 <= 4)
                                ++thinCols;
                        }
                        if (thinCols * 10 < runLen * 7)
                            continue;
                        const bool fullSpan = runLen >= std::max(28, int(coverWidth * .60f));
                        const bool continuesLeft =
                            x0 <= 1 && region.left >= 3 &&
                            (outsideImgInk(int(region.left) - 2, sy) ||
                             outsideImgInk(int(region.left) - 3, sy));
                        const bool continuesRight =
                            x1 >= coverWidth - 1 && region.right + 2 < image.width &&
                            (outsideImgInk(int(region.right) + 1, sy) ||
                             outsideImgInk(int(region.right) + 2, sy));
                        const bool outsideRule =
                            outsideSourceY && runLen >= std::max(16, int(detectedHeight * 1.1f)) &&
                            !insideOtherRegion(int(region.left) + (x0 + x1) / 2, sy);
                        if (fullSpan || continuesLeft || continuesRight || outsideRule) {
                            for (int k = x0; k < x1; ++k) {
                                int up = y, down = y;
                                while (up > 0 && y - up < 4 && rawInk[size_t(up - 1) * coverWidth + k])
                                    --up;
                                while (down + 1 < coverHeight && down - y < 4 &&
                                       rawInk[size_t(down + 1) * coverWidth + k])
                                    ++down;
                                if (down - up + 1 <= 4) {
                                    for (int yy = up; yy <= down; ++yy)
                                        structMask[size_t(yy) * coverWidth + k] = 1;
                                } else if (rawInk[size_t(y) * coverWidth + k]) {
                                    structMask[size_t(y) * coverWidth + k] = 1;
                                }
                            }
                        }
                    }
                }
                for (int x = 0; x < coverWidth; ++x) {
                    const int sx = int(region.left) + x;
                    const bool outsideSourceX = sx < source.left || sx >= source.right;
                    int y = 0;
                    while (y < coverHeight) {
                        if (!rawInk[size_t(y) * coverWidth + x]) {
                            ++y;
                            continue;
                        }
                        const int y0 = y;
                        int gap = 0;
                        while (y < coverHeight) {
                            if (rawInk[size_t(y) * coverWidth + x]) {
                                gap = 0;
                                ++y;
                            } else if (gap == 0 && y + 1 < coverHeight && rawInk[size_t(y + 1) * coverWidth + x]) {
                                gap = 1;
                                ++y;
                            } else {
                                break;
                            }
                        }
                        const int y1 = (y > y0 && !rawInk[size_t(y - 1) * coverWidth + x]) ? y - 1 : y;
                        const int runLen = y1 - y0;
                        if (runLen < 12)
                            continue;
                        int thinRows = 0, clearRows = 0;
                        for (int k = y0; k < y1; ++k) {
                            int l = x, r = x;
                            while (l > 0 && x - l < 5 && rawInk[size_t(k) * coverWidth + (l - 1)])
                                --l;
                            while (r + 1 < coverWidth && r - x < 5 && rawInk[size_t(k) * coverWidth + (r + 1)])
                                ++r;
                            if (r - l + 1 <= 4) {
                                ++thinRows;
                                const bool clearL = (l < 2) || !rawInk[size_t(k) * coverWidth + (l - 2)];
                                const bool clearR =
                                    (r + 2 >= coverWidth) || !rawInk[size_t(k) * coverWidth + (r + 2)];
                                if (clearL && clearR)
                                    ++clearRows;
                            }
                        }
                        if (thinRows * 10 < runLen * 7)
                            continue;
                        const int syMid = int(region.top) + (y0 + y1) / 2;
                        if (insideOtherRegion(sx, syMid))
                            continue;
                        const bool continuesTop =
                            y0 <= 1 && region.top >= 3 &&
                            (outsideImgInk(sx, int(region.top) - 2) ||
                             outsideImgInk(sx, int(region.top) - 3));
                        const bool continuesBottom =
                            y1 >= coverHeight - 1 && region.bottom + 2 < image.height &&
                            (outsideImgInk(sx, int(region.bottom) + 1) ||
                             outsideImgInk(sx, int(region.bottom) + 2));
                        // A genuine vertical divider or table rule either continues past the cover
                        // bounds (outside neighboring text boxes) or spans the full cover height
                        // outside source text; glyph stems ("l", "I", "1", "m", "d") never qualify.
                        const bool continuousDivider =
                            (continuesTop || continuesBottom) && runLen >= std::max(14, int(coverHeight * .65f));
                        const bool edgeFrameBorder =
                            outsideSourceX && runLen >= std::max(14, coverHeight - 2) &&
                            clearRows * 10 >= runLen * 8;
                        if (continuousDivider || edgeFrameBorder) {
                            for (int k = y0; k < y1; ++k) {
                                int l = x, r = x;
                                while (l > 0 && x - l < 4 && rawInk[size_t(k) * coverWidth + (l - 1)])
                                    --l;
                                while (r + 1 < coverWidth && r - x < 4 && rawInk[size_t(k) * coverWidth + (r + 1)])
                                    ++r;
                                if (r - l + 1 <= 4) {
                                    for (int xx = l; xx <= r; ++xx)
                                        structMask[size_t(k) * coverWidth + xx] = 1;
                                } else if (rawInk[size_t(k) * coverWidth + x]) {
                                    structMask[size_t(k) * coverWidth + x] = 1;
                                }
                            }
                        }
                    }
                }
                for (size_t idx = 0; idx < glyphMask.size(); ++idx)
                    if (structMask[idx])
                        glyphMask[idx] = 0;
                std::vector<unsigned char> grown = glyphMask;
                for (int y = 0; y < coverHeight; ++y)
                    for (int x = 0; x < coverWidth; ++x) {
                        if (!glyphMask[size_t(y) * coverWidth + x])
                            continue;
                        for (int dy = -3; dy <= 3; ++dy)
                            for (int dx = -3; dx <= 3; ++dx) {
                                if (dx * dx + dy * dy > 10)
                                    continue;
                                const int nx = x + dx, ny = y + dy;
                                if (nx >= 0 && nx < coverWidth && ny >= 0 && ny < coverHeight &&
                                    !structMask[size_t(ny) * coverWidth + nx])
                                    grown[size_t(ny) * coverWidth + nx] = 1;
                            }
                    }
                std::vector<unsigned char> haloMask(size_t(coverWidth) * size_t(coverHeight), 0);
                for (int y = 0; y < coverHeight; ++y)
                    for (int x = 0; x < coverWidth; ++x) {
                        if (!grown[size_t(y) * coverWidth + x])
                            continue;
                        for (int dy = -2; dy <= 2; ++dy)
                            for (int dx = -2; dx <= 2; ++dx) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx >= 0 && nx < coverWidth && ny >= 0 && ny < coverHeight &&
                                    !grown[size_t(ny) * coverWidth + nx] &&
                                    !structMask[size_t(ny) * coverWidth + nx])
                                    haloMask[size_t(ny) * coverWidth + nx] = 1;
                            }
                    }
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
                        int leftX = runStart - 1, rightX = runEnd;
                        if (leftX > 0 && !grown[size_t(y) * coverWidth + (leftX - 1)] &&
                            !structMask[size_t(y) * coverWidth + (leftX - 1)])
                            --leftX;
                        if (rightX + 1 < coverWidth && !grown[size_t(y) * coverWidth + (rightX + 1)] &&
                            !structMask[size_t(y) * coverWidth + (rightX + 1)])
                            ++rightX;
                        const bool hasLeft = leftX >= 0 && !structMask[size_t(y) * coverWidth + leftX],
                                   hasRight = rightX < coverWidth && !structMask[size_t(y) * coverWidth + rightX];
                        for (int k = runStart; k < runEnd; ++k) {
                            auto* p = &cover[(size_t(y) * coverWidth + k) * 4];
                            int yUp = y - 1;
                            while (yUp >= 0 && (grown[size_t(yUp) * coverWidth + k] ||
                                                structMask[size_t(yUp) * coverWidth + k]))
                                --yUp;
                            if (yUp > 0 && !grown[size_t(yUp - 1) * coverWidth + k] &&
                                !structMask[size_t(yUp - 1) * coverWidth + k])
                                --yUp;
                            int yDown = y + 1;
                            while (yDown < coverHeight && (grown[size_t(yDown) * coverWidth + k] ||
                                                           structMask[size_t(yDown) * coverWidth + k]))
                                ++yDown;
                            if (yDown + 1 < coverHeight && !grown[size_t(yDown + 1) * coverWidth + k] &&
                                !structMask[size_t(yDown + 1) * coverWidth + k])
                                ++yDown;
                            const auto* upPixel =
                                yUp >= 0 ? &cover[(size_t(yUp) * coverWidth + k) * 4] : nullptr;
                            const auto* downPixel =
                                yDown < coverHeight ? &cover[(size_t(yDown) * coverWidth + k) * 4] : nullptr;
                            const auto* leftPixel =
                                hasLeft ? &cover[(size_t(y) * coverWidth + leftX) * 4] : nullptr;
                            const auto* rightPixel =
                                hasRight ? &cover[(size_t(y) * coverWidth + rightX) * 4] : nullptr;
                            const float vBias = (runEnd - runStart > 8) ? 2.2f : 1.4f;
                            float sumW = 0.f, accum[3]{};
                            auto addSample = [&](const unsigned char* sample, float dist, float factor) {
                                if (!sample)
                                    return;
                                const float w = factor / std::max(1.f, dist);
                                accum[0] += float(sample[0]) * w;
                                accum[1] += float(sample[1]) * w;
                                accum[2] += float(sample[2]) * w;
                                sumW += w;
                            };
                            addSample(upPixel, float(y - yUp), vBias);
                            addSample(downPixel, float(yDown - y), vBias);
                            addSample(leftPixel, float(k - leftX), 1.f);
                            addSample(rightPixel, float(rightX - k), 1.f);
                            if (sumW > 0.f) {
                                for (int c = 0; c < 3; ++c)
                                    p[c] = static_cast<unsigned char>(
                                        std::clamp(int(accum[c] / sumW + .5f), 0, 255));
                                p[3] = 255;
                            }
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
                            if (!grown[size_t(y) * coverWidth + x] || structMask[size_t(y) * coverWidth + x])
                                continue;
                            int sums[3]{}, cnt = 0;
                            for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx) {
                                    if (structMask[size_t(y + dy) * coverWidth + (x + dx)])
                                        continue;
                                    const auto* q = &cover[(size_t(y + dy) * coverWidth + (x + dx)) * 4];
                                    sums[0] += q[0];
                                    sums[1] += q[1];
                                    sums[2] += q[2];
                                    ++cnt;
                                }
                            if (cnt <= 0)
                                continue;
                            auto* p = &scratch[(size_t(y) * coverWidth + x) * 4];
                            p[0] = static_cast<unsigned char>(sums[0] / cnt);
                            p[1] = static_cast<unsigned char>(sums[1] / cnt);
                            p[2] = static_cast<unsigned char>(sums[2] / cnt);
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
                        if (structMask[size_t(y) * coverWidth + x])
                            continue;
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
                const auto blockValue = [&](int channel, int bx, int by) -> float {
                    bx = std::clamp(bx, 0, blocksX - 1);
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
                        const size_t idx = size_t(y) * coverWidth + x;
                        const size_t blockIdx =
                            size_t(std::clamp(y / blockSize, 0, blocksY - 1)) * size_t(blocksX) +
                            size_t(std::clamp(x / blockSize, 0, blocksX - 1));
                        const bool smoothSurface = blockSpread[blockIdx] <= 8;
                        const float weight = structMask[idx] ? 0.f
                                             : grown[idx]    ? (smoothSurface ? .86f : .58f)
                                             : haloMask[idx] ? (smoothSurface ? .45f : .22f)
                                                             : 0.f;
                        if (weight <= 0.f)
                            continue;
                        auto* p = &cover[idx * 4];
                        float target[3]{};
                        for (int ch = 0; ch < 3; ++ch) {
                            const float top =
                                blockValue(ch, x0, y0) * (1 - tx) + blockValue(ch, x0 + 1, y0) * tx;
                            const float bottom =
                                blockValue(ch, x0, y0 + 1) * (1 - tx) + blockValue(ch, x0 + 1, y0 + 1) * tx;
                            target[ch] = top * (1 - ty) + bottom * ty;
                        }
                        const float deviation = (std::abs(target[0] - float(p[2])) +
                                                 std::abs(target[1] - float(p[1])) +
                                                 std::abs(target[2] - float(p[0]))) /
                                                3.f;
                        const float falloff = smoothSurface ? 36.f : 24.f;
                        const float adaptive = weight / (1.f + deviation / falloff);
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
                if (segment.sourcePixels) {
                    for (const auto& other : protectedRegions) {
                        if (!Valid(other) || EqualRect(&other, &source) || Conflicts(source, other))
                            continue;
                        if (other.top < padded.bottom && other.bottom > padded.top) {
                            if (other.right <= source.left)
                                padded.left = std::max(padded.left, (source.left + other.right + 1) / 2);
                            else if (other.right <= source.left + 8 && other.left < source.left)
                                padded.left = std::max(padded.left, source.left);
                            if (other.left >= source.right)
                                padded.right = std::min(padded.right, (source.right + other.left) / 2);
                            else if (other.left >= source.right - 8 && other.right > source.right)
                                padded.right = std::min(padded.right, source.right);
                        }
                        if (other.left < padded.right && other.right > padded.left) {
                            if (other.bottom <= source.top)
                                padded.top = std::max(padded.top, (source.top + other.bottom + 1) / 2);
                            else if (other.bottom <= source.top + 8 && other.top < source.top)
                                padded.top = std::max(padded.top, source.top);
                            if (other.top >= source.bottom)
                                padded.bottom = std::min(padded.bottom, (source.bottom + other.top) / 2);
                            else if (other.top >= source.bottom - 8 && other.bottom > source.bottom)
                                padded.bottom = std::min(padded.bottom, source.bottom);
                        }
                    }
                }
                EraseRow block{padded, item.background, item.background, item.background, item.background, false, padded};
                block.cover = buildCover(padded, item.background);
                if (!block.cover.empty())
                    item.eraseLines.push_back(block);
            }
        }
        if (coverOnly) {
            // The surface is rebuilt above; this entry carries no translation to lay out.
            output[i].bandHeight = detectedHeight;
            output[i].coreHeight = coreHeights[i];
            continue;
        }
        auto& prep = preps[i];
        prep.active = !item.suppressed;
        prep.column = columns[i];
        prep.detectedHeight = detectedHeight;
        prep.width = std::max(1.f, float(Width(area)) - 2 * item.padding);
        prep.height = std::max(1.f, float(Height(area)) - 2 * item.padding);
        output[i].bandHeight = detectedHeight;
        output[i].coreHeight = coreHeights[i];
        output[i].eraseHeight = float(Height(source));
        if (columns[i]) {
            auto text = Wide(segment.translated);
            for (size_t at = 0; at < text.size();) {
                const wchar_t ch = text[at];
                if (ch == L'\r' || ch == L'\n' || ch == L' ' || ch == L'\t') {
                    ++at;
                    continue;
                }
                size_t length = 1;
                if (ch >= 0xD800 && ch <= 0xDBFF && at + 1 < text.size())
                    length = 2;
                prep.glyphs.push_back(text.substr(at, length));
                at += length;
            }
            for (auto line : item.detectedLines)
                if (Valid(line))
                    prep.strips.push_back(line);
            if (prep.strips.empty())
                prep.strips.push_back(source);
            prep.stripBottoms.reserve(prep.strips.size());
            for (const auto& strip : prep.strips) {
                LONG limitY = source.bottom;
                if (surfaceColors[i].has_value() && Width(strip) >= 4) {
                    const COLORREF sbg = *surfaceColors[i];
                    const int sbr = GetRValue(sbg), sbgG = GetGValue(sbg), sbb = GetBValue(sbg);
                    const LONG x0 = strip.left + Width(strip) / 4;
                    const LONG x1 = std::max(x0 + 1, strip.right - Width(strip) / 4);
                    limitY = std::clamp(strip.bottom, source.top, source.bottom);
                    for (LONG y = limitY; y < source.bottom; ++y) {
                        int nonBg = 0, total = 0;
                        for (LONG x = x0; x < x1; ++x) {
                            const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                            if (std::abs(int(p[2]) - sbr) + std::abs(int(p[1]) - sbgG) +
                                    std::abs(int(p[0]) - sbb) >=
                                64)
                                ++nonBg;
                            ++total;
                        }
                        if (total > 0 && nonBg * 100 >= total * 35)
                            break;
                        limitY = y + 1;
                    }
                    limitY = std::max(limitY, std::min(source.bottom, strip.bottom));
                }
                prep.stripBottoms.push_back(limitY);
            }
            prep.preferred = settings.autoFont ? std::max(9.f, detectedHeight * .90f)
                                               : std::max(9.f, settings.fontSize * scale);
            continue;
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
        prep.pitchRatio = detectedHeight > 0 && sourcePitch > 0
                              ? std::clamp(sourcePitch / detectedHeight, 1.0f, 1.8f)
                              : 1.15f;
        prep.allowedRows = item.detectedLines.empty() ? 0 : item.detectedLines.size();
        prep.wrapText = prep.allowedRows != 1;
        auto text = Wide(segment.translated);
        const bool explicitRows = RowsBreakExplicitly(item.detectedLines, Width(item.sourceRect));
        if (segment.sourceLines.size() > 1 && !explicitRows && !HasSeparateTextLines(segment.original)) {
            std::erase(text, L'\r');
            std::wstring reflowed;
            for (size_t at = 0; at < text.size(); ++at) {
                auto ch = text[at];
                if (ch == L'\r')
                    continue;
                if (ch == L'\n' && at > 0 && at + 1 < text.size() && text[at - 1] != L'\n' &&
                    text[at + 1] != L'\n' && text[at + 1] != L'\r') {
                    auto previous = reflowed.empty() ? L' ' : reflowed.back(), next = text[at + 1];
                    auto noBreakSpaceScript = [](wchar_t c) {
                        return c >= 0x2E80 || (c >= 0x0E00 && c <= 0x0EFF) || (c >= 0x1780 && c <= 0x17FF);
                    };
                    if (!noBreakSpaceScript(previous) && !noBreakSpaceScript(next) &&
                        previous != L' ' && next != L' ')
                        reflowed += L' ';
                    continue;
                }
                reflowed += ch;
            }
            text = std::move(reflowed);
        }
        prep.text = std::move(text);
        constexpr std::wstring_view kProsePunct = L"。，、；：？！“”‘’「」『』【】（）()[].,;:?!\"؟؛،।॥";
        const std::wstring wOrig = Wide(segment.original);
        const bool hasProsePunct = prep.text.find_first_of(kProsePunct) != std::wstring::npos ||
                                   wOrig.find_first_of(kProsePunct) != std::wstring::npos;
        prep.isProse = hasProsePunct || item.detectedLines.size() >= 2 || prep.text.size() >= 14;
        prep.isShortLabel =
            !hasProsePunct && item.detectedLines.size() <= 1 && prep.text.size() <= 8;
        auto measure = [&](const std::wstring& body, float fontSize, float lineSpacing,
                           ComPtr<IDWriteTextLayout>& layout, bool wrap) {
            return MeasureTextLayout(factory.Get(), settings.font, body, fontSize, lineSpacing,
                                     prep.width, item.bold, item.centerAligned, wrap, layout);
        };
        float preferred = settings.fontSize * scale;
        if (settings.autoFont) {
            if (detectedHeight > 0) {
                const TextTraits traits = AnalyzeText(segment.original);
                const float bandSize =
                    (traits.cjk ? detectedHeight : InkBand(detectedHeight)) / GlyphBandRatio(traits);
                const float core = coreHeights[i];
                const bool validCore = core >= 4.f && core < detectedHeight * .85f;
                prep.hasCore = validCore;
                // Use the dense lowercase core alongside the outer band across both single-line and
                // multi-line Latin passages so lines without descenders and multi-line paragraphs land on
                // the same typographic scale.
                const float emSize =
                    traits.cjk ? bandSize
                               : (validCore ? (bandSize + core / GlyphCoreRatio(traits)) * .5f : bandSize);
                preferred = std::clamp(
                    traits.cjk || !prep.isShortLabel ? emSize : emSize * .92f, 9.f, 144.f);
                const float rowCap =
                    prep.allowedRows >= 2 ? float(Height(area)) / float(prep.allowedRows) * 1.15f
                                          : float(Height(area)) * 1.0f;
                preferred = std::min(preferred, std::max(9.f, rowCap));
            } else {
                float low = 10 * scale, high = std::min(144.f, std::max(26 * scale, float(Height(source))));
                ComPtr<IDWriteTextLayout> originalLayout;
                for (int step = 0; step < 10; ++step) {
                    float mid = (low + high) / 2;
                    auto m = measure(wOrig, mid, mid * 1.25f, originalLayout, prep.wrapText);
                    if (m.height <= prep.height && m.widthIncludingTrailingWhitespace <= prep.width)
                        low = mid;
                    else
                        high = mid;
                }
                preferred = low;
            }
        }
        prep.preferred = preferred;
    }

    auto colorDist = [](COLORREF a, COLORREF b) {
        return std::abs(int(GetRValue(a)) - int(GetRValue(b))) +
               std::abs(int(GetGValue(a)) - int(GetGValue(b))) +
               std::abs(int(GetBValue(a)) - int(GetBValue(b)));
    };
    auto rectGap = [](RECT a, RECT b) {
        const LONG dx = std::max(0L, std::max(a.left, b.left) - std::min(a.right, b.right));
        const LONG dy = std::max(0L, std::max(a.top, b.top) - std::min(a.bottom, b.bottom));
        return std::max(dx, dy);
    };

    // Pass A: Harmonize preferred font sizes across peer segments on the same surface so body
    // paragraphs, greetings, list items and adjacent manga dialogue bubbles share one typographic tier.
    std::vector<float> harmonizedPreferred(output.size(), 0.f);
    for (size_t i = 0; i < output.size(); ++i)
        harmonizedPreferred[i] = preps[i].preferred;
    if (settings.autoFont) {
        for (size_t i = 0; i < output.size(); ++i) {
            if (!preps[i].active || preps[i].preferred <= 0.f)
                continue;
            const float pi = preps[i].preferred;
            const RECT ri = output[i].sourceRect;
            std::vector<float> group;
            for (size_t j = 0; j < output.size(); ++j) {
                if (!preps[j].active || preps[j].preferred <= 0.f || preps[j].column != preps[i].column)
                    continue;
                if (colorDist(output[i].background, output[j].background) > 45)
                    continue;
                const float pj = preps[j].preferred;
                if (std::max(pi, pj) > std::min(pi, pj) * 1.24f)
                    continue;
                const RECT rj = output[j].sourceRect;
                bool related = (i == j);
                if (!related && preps[i].column) {
                    related = rectGap(ri, rj) <= std::max<LONG>(220, LONG(pi * 12.f)) ||
                              std::max(pi, pj) <= std::min(pi, pj) * 1.18f;
                } else if (!related) {
                    const LONG vGap = rj.top >= ri.bottom
                                          ? (rj.top - ri.bottom)
                                          : (ri.top >= rj.bottom ? (ri.top - rj.bottom) : 0);
                    const LONG hGap = rj.left >= ri.right
                                          ? (rj.left - ri.right)
                                          : (ri.left >= rj.right ? (ri.left - rj.right) : 0);
                    const LONG cxI = (ri.left + ri.right) / 2, cxJ = (rj.left + rj.right) / 2;
                    const bool aligned =
                        std::abs(ri.left - rj.left) <= std::max<LONG>(24, LONG(pi * 1.6f)) ||
                        std::abs(ri.right - rj.right) <= std::max<LONG>(24, LONG(pi * 1.6f)) ||
                        std::abs(cxI - cxJ) <= std::max<LONG>(20, LONG(pi * 1.3f));
                    if (aligned && vGap <= std::max<LONG>(240, LONG(pi * 14.f)))
                        related = true;
                    else if (preps[i].isProse && preps[j].isProse &&
                             std::max(pi, pj) <= std::min(pi, pj) * 1.18f)
                        related = true;
                    else if (vGap <= std::max<LONG>(60, LONG(pi * 3.5f)) &&
                             hGap <= std::max<LONG>(160, LONG(pi * 8.f)) &&
                             std::max(pi, pj) <= std::min(pi, pj) * 1.16f)
                        related = true;
                }
                if (related) {
                    group.push_back(pj);
                    if (output[j].detectedLines.size() >= 2 || preps[j].hasCore)
                        group.push_back(pj);
                }
            }
            if (group.size() >= 2) {
                std::sort(group.begin(), group.end());
                harmonizedPreferred[i] = group[group.size() / 2];
            }
        }
    }

    const auto layoutItem = [&](size_t i, float targetPreferred, bool allowShrink) {
        auto& item = output[i];
        auto& prep = preps[i];
        const RECT source = item.sourceRect;
        if (prep.column) {
            item.columnTexts.clear();
            item.layout.Reset();
            const auto room = [&](float pitch, size_t k) {
                return int(std::max(1.f, std::floor(float(prep.stripBottoms[k] - prep.strips[k].top) / pitch)));
            };
            const auto capacity = [&](float size) {
                const float pitch = std::max(1.f, size * 1.12f);
                size_t total = 0;
                for (size_t k = 0; k < prep.strips.size(); ++k)
                    total += size_t(room(pitch, k));
                return total;
            };
            float size = targetPreferred;
            if (allowShrink && !prep.glyphs.empty() && capacity(size) < prep.glyphs.size()) {
                float low = 8.f, high = size;
                for (int step = 0; step < 14; ++step) {
                    const float mid = (low + high) / 2;
                    if (capacity(mid) >= prep.glyphs.size())
                        low = mid;
                    else
                        high = mid;
                }
                size = low;
            }
            item.fontSize = size;
            item.overflow = capacity(size) < prep.glyphs.size();
            const float pitch = std::max(1.f, size * 1.12f);
            size_t at = 0;
            float contentBottom = float(source.top);
            for (size_t k = 0; k < prep.strips.size(); ++k) {
                const auto& strip = prep.strips[k];
                if (at >= prep.glyphs.size())
                    break;
                const int count = room(pitch, k);
                std::wstring body;
                for (int c = 0; c < count && at < prep.glyphs.size(); ++c, ++at) {
                    if (!body.empty())
                        body += L'\n';
                    body += prep.glyphs[at];
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
            return;
        }
        auto pitchFor = [&](float fontSize) { return fontSize * prep.pitchRatio; };
        auto measure = [&](const std::wstring& body, float fontSize, float lineSpacing,
                           ComPtr<IDWriteTextLayout>& layout, bool wrap) {
            return MeasureTextLayout(factory.Get(), settings.font, body, fontSize, lineSpacing,
                                     prep.width, item.bold, item.centerAligned, wrap, layout);
        };
        const auto exceedsRows = [&](const DWRITE_TEXT_METRICS& m, float fontSize, size_t maxRows) {
            if (!maxRows)
                return false;
            const float pitch = pitchFor(fontSize);
            const float rows = pitch > 0 ? m.height / pitch : 1.f;
            return rows > float(maxRows) + .2f;
        };
        const auto visualBoxHeight = [&](const DWRITE_TEXT_METRICS& m, float fontSize) {
            if (m.lineCount == 0)
                return 0.f;
            const float pitch = pitchFor(fontSize);
            return std::min(m.height, float(m.lineCount - 1) * pitch + fontSize * 1.02f);
        };
        const auto holdsBox = [&](const DWRITE_TEXT_METRICS& m, float fontSize) {
            return visualBoxHeight(m, fontSize) <= prep.height + .5f &&
                   m.widthIncludingTrailingWhitespace <= prep.width + item.maxOverhang;
        };
        bool wrap = prep.wrapText;
        size_t maxRows = prep.allowedRows;
        item.fontSize = targetPreferred;
        auto metrics = measure(prep.text, item.fontSize, pitchFor(item.fontSize), item.layout, wrap);
        if (allowShrink && settings.autoFont &&
            (!holdsBox(metrics, item.fontSize) || exceedsRows(metrics, item.fontSize, maxRows))) {
            // If a single-line prose sentence has plenty of free vertical space inside its own area and
            // would otherwise shrink heavily just to stay on one line, allow wrapping to keep body size uniform.
            if (!wrap && prep.isProse && !prep.isShortLabel && prep.height >= targetPreferred * 2.15f) {
                ComPtr<IDWriteTextLayout> wrappedTrial;
                auto wm = measure(prep.text, targetPreferred, pitchFor(targetPreferred), wrappedTrial, true);
                if (holdsBox(wm, targetPreferred) && wm.lineCount <= 2) {
                    wrap = true;
                    maxRows = 2;
                    metrics = wm;
                    item.layout = std::move(wrappedTrial);
                }
            }
            if (!holdsBox(metrics, item.fontSize) || exceedsRows(metrics, item.fontSize, maxRows)) {
                float low = std::min(targetPreferred, std::max(12 * scale, targetPreferred * .6f)),
                      high = targetPreferred;
                for (int step = 0; step < 12; ++step) {
                    float mid = (low + high) / 2;
                    ComPtr<IDWriteTextLayout> trial;
                    auto m = measure(prep.text, mid, pitchFor(mid), trial, wrap);
                    if (holdsBox(m, mid) && !exceedsRows(m, mid, maxRows))
                        low = mid;
                    else
                        high = mid;
                }
                item.fontSize = low;
                metrics = measure(prep.text, low, pitchFor(low), item.layout, wrap);
            }
        }
        UINT32 lines{};
        item.layout->GetLineMetrics(nullptr, 0, &lines);
        std::vector<DWRITE_LINE_METRICS> lineMetrics(lines);
        if (lines) {
            CheckHR(item.layout->GetLineMetrics(lineMetrics.data(), lines, &lines), "layout");
            item.lineHeight = lineMetrics.front().height;
        }
        item.contentHeight = metrics.height;
        item.overflow = visualBoxHeight(metrics, item.fontSize) > prep.height + 1.f ||
                        metrics.widthIncludingTrailingWhitespace > prep.width + item.maxOverhang + .5f;
    };

    for (size_t i = 0; i < output.size(); ++i) {
        if (!preps[i].active)
            continue;
        layoutItem(i, harmonizedPreferred[i], settings.autoFont);
    }

    // Pass B: When one passage in a tight dialogue/article cluster shrinks slightly to fit its box,
    // bring closely coupled peer passages down to the same face so adjacent bubbles and paragraphs read
    // as one coherent block.
    if (settings.autoFont) {
        std::vector<float> finalSizes(output.size(), 0.f);
        for (size_t i = 0; i < output.size(); ++i)
            finalSizes[i] = output[i].fontSize;
        for (size_t i = 0; i < output.size(); ++i) {
            if (!preps[i].active || output[i].fontSize <= 0.f)
                continue;
            const float hpI = harmonizedPreferred[i];
            float minPeer = output[i].fontSize;
            for (size_t j = 0; j < output.size(); ++j) {
                if (i == j || !preps[j].active || preps[j].column != preps[i].column)
                    continue;
                if (colorDist(output[i].background, output[j].background) > 45)
                    continue;
                const float hpJ = harmonizedPreferred[j];
                if (std::max(hpI, hpJ) > std::min(hpI, hpJ) * 1.15f)
                    continue;
                if (output[j].fontSize >= hpJ - .5f)
                    continue;
                const RECT ri = output[i].sourceRect, rj = output[j].sourceRect;
                bool coupled = false;
                if (preps[i].column) {
                    coupled = rectGap(ri, rj) <= std::max<LONG>(140, LONG(hpI * 7.f));
                } else {
                    const bool roomyPeer = preps[j].width >= hpJ * 4.5f || output[j].detectedLines.size() >= 2;
                    if (!roomyPeer)
                        continue;
                    const LONG vGap = rj.top >= ri.bottom
                                          ? (rj.top - ri.bottom)
                                          : (ri.top >= rj.bottom ? (ri.top - rj.bottom) : 0);
                    const LONG cxI = (ri.left + ri.right) / 2, cxJ = (rj.left + rj.right) / 2;
                    const bool aligned =
                        std::abs(ri.left - rj.left) <= std::max<LONG>(28, LONG(hpI * 1.8f)) ||
                        std::abs(cxI - cxJ) <= std::max<LONG>(24, LONG(hpI * 1.5f));
                    if (preps[i].isProse && preps[j].isProse && aligned &&
                        vGap <= std::max<LONG>(100, LONG(hpI * 5.f)))
                        coupled = true;
                    else if (rectGap(ri, rj) <= std::max<LONG>(48, LONG(hpI * 2.5f)))
                        coupled = true;
                }
                if (coupled && output[j].fontSize >= std::max(12 * scale, hpI * .65f))
                    minPeer = std::min(minPeer, output[j].fontSize);
            }
            if (output[i].fontSize > minPeer * 1.06f)
                finalSizes[i] = minPeer;
        }
        for (size_t i = 0; i < output.size(); ++i) {
            if (preps[i].active && finalSizes[i] < output[i].fontSize - .25f)
                layoutItem(i, finalSizes[i], false);
        }
    }
    return output;
}
COLORREF ReadableTextColor(COLORREF preferred, COLORREF background, bool preservePolarity) {
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
    const double blackContrast = contrast(RGB(0, 0, 0));
    const double whiteContrast = contrast(RGB(255, 255, 255));
    int target = blackContrast >= whiteContrast ? 0 : 255;
    double requiredContrast = minimumContrast;
    // When preferred comes directly from the original UI glyph (e.g. white text on iMessage blue,
    // iOS green, or coral orange buttons), preserve its light/dark polarity whenever that endpoint
    // offers viable separation (>= 2.0) instead of flipping white UI text to dark grey.
    if (preservePolarity) {
        const bool lighterPref = luminance(preferred) >= luminance(background);
        const double sameEndContrast = lighterPref ? whiteContrast : blackContrast;
        if (sameEndContrast >= 2.0) {
            if (contrast(preferred) >= 2.15)
                return preferred;
            target = lighterPref ? 255 : 0;
            requiredContrast = std::min(minimumContrast, sameEndContrast - .02);
        }
    }
    // Keep the measured hue and move only its lightness until the pair separates, so a blue link
    // stays blue and a warm caption keeps its warmth.
    for (int step = 1; step <= 16; ++step) {
        const float t = float(step) / 16.f;
        auto mix = [&](int channel) {
            return int(std::clamp(int(float(channel) + (float(target) - float(channel)) * t + .5f), 0, 255));
        };
        const COLORREF candidate =
            RGB(mix(GetRValue(preferred)), mix(GetGValue(preferred)), mix(GetBValue(preferred)));
        if (contrast(candidate) >= requiredContrast)
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
        const COLORREF textFill =
            ReadableTextColor(item.sourceColor.value_or(textColor), measured, item.sourceColor.has_value());
        brush->SetColor(color(textFill));
        // The inverse mode derives the rim from the rendered face colour, exactly as the reading
        // panel does, so the outline keeps its contrast on a dark cover as well as a light one.
        // When matchTextColor places dark or coloured ink on a light rebuilt surface while the
        // outline remains at the default black (paired with default white text), align the rim with
        // the surface so grey captions and coloured links do not acquire a dark crust.
        // On saturated mid-tone UI surfaces (e.g. blue/green chat bubbles or buttons) carrying light
        // text, a softened darker tint of the surface keeps strokes crisp without a black crust.
        const float surfLum =
            .299f * GetRValue(measured) + .587f * GetGValue(measured) + .114f * GetBValue(measured);
        const int surfChroma =
            std::max({int(GetRValue(measured)), int(GetGValue(measured)), int(GetBValue(measured))}) -
            std::min({int(GetRValue(measured)), int(GetGValue(measured)), int(GetBValue(measured))});
        const float fillLum =
            .299f * GetRValue(textFill) + .587f * GetGValue(textFill) + .114f * GetBValue(textFill);
        const bool vibrantLightTextSurface =
            surfLum >= 95.f && surfLum <= 190.f && surfChroma >= 60 && fillLum >= 210.f;
        const COLORREF shadedSurface =
            RGB(int(GetRValue(measured) * .45f), int(GetGValue(measured) * .45f),
                int(GetBValue(measured) * .45f));
        COLORREF rimColor = settings.outlineColor;
        if (settings.outlineColorMode == 1) {
            if (surfLum >= 140.f && fillLum < surfLum)
                rimColor = measured;
            else if (vibrantLightTextSurface)
                rimColor = shadedSurface;
            else
                rimColor = RGB(255 - GetRValue(textFill), 255 - GetGValue(textFill),
                               255 - GetBValue(textFill));
        } else if (settings.matchTextColor && item.sourceColor.has_value() &&
                   settings.outlineColor == RGB(0, 0, 0) && surfLum >= 140.f && fillLum < surfLum) {
            rimColor = measured;
        } else if (settings.matchTextColor && item.sourceColor.has_value() &&
                   settings.outlineColor == RGB(0, 0, 0) && vibrantLightTextSurface) {
            rimColor = shadedSurface;
        }
        rimBrush->SetColor(color(rimColor));
        // The clip keeps the replacement on its surface while honouring the overhang the fit search
        // allows: a label may run past its source box, and drawing must not cut the letters the layout
        // already accepted.
        const float clipLeft =
            item.centerAligned ? box.left + pad - item.maxOverhang * .5f : box.left + pad;
        const float clipRight =
            item.centerAligned ? box.right - pad + item.maxOverhang * .5f : box.right - pad + item.maxOverhang;
        target->PushAxisAlignedClip(
            D2D1::RectF(clipLeft, box.top, clipRight, box.bottom),
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
            const float innerTop = box.top + pad;
            const float innerBottom = box.bottom - pad;
            float drawY = innerTop - item.scroll;
            if (item.scroll == 0.f && !item.overflow) {
                DWRITE_TEXT_METRICS m{};
                item.layout->GetMetrics(&m);
                const unsigned lineCount = std::max(1u, m.lineCount);
                const bool hasPitch = item.fontSize > 0.f && item.lineHeight > 0.f;
                const float inkTop = hasPitch ? .04f * item.fontSize : 0.f;
                const float inkBottom =
                    hasPitch ? std::min(item.contentHeight,
                                        float(lineCount - 1) * item.lineHeight + item.fontSize * 1.02f)
                             : item.contentHeight;
                const float visualCenter = .5f * (inkTop + inkBottom);
                float targetCenter = (innerTop + innerBottom) * .5f;
                if (!item.detectedLines.empty()) {
                    LONG minTop = item.detectedLines.front().top;
                    LONG maxBottom = item.detectedLines.front().bottom;
                    for (const auto& line : item.detectedLines) {
                        if (!Valid(line))
                            continue;
                        minTop = std::min(minTop, line.top);
                        maxBottom = std::max(maxBottom, line.bottom);
                    }
                    if (maxBottom > minTop)
                        targetCenter = (float(minTop) + float(maxBottom)) * .5f;
                } else if (Valid(item.sourceRect)) {
                    targetCenter = (float(item.sourceRect.top) + float(item.sourceRect.bottom)) * .5f;
                }
                const float minDrawY = innerTop - inkTop;
                const float maxDrawY = std::max(minDrawY, innerBottom - inkBottom);
                drawY = std::clamp(targetCenter - visualCenter, minDrawY, maxDrawY);
            }
            auto rim = renderer();
            CheckHR(item.layout->Draw(nullptr, rim.Get(), box.left + pad, drawY), "layout");
        }
        target->PopAxisAlignedClip();
        if (item.overflow) {
            brush->SetColor(color(RGB(76, 141, 246)));
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
