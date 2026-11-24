#include "SpatialLayout.h"
#include "TextRim.h"
#include "capture/Capture.h"
#include "common/TextMetrics.h"
#include <dwrite_1.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
    bool hasStroke{};
    std::optional<COLORREF> strokeColor;
    std::vector<RECT> lines;
    float lineHeight{};
    // Height of the dense ink core of a line. Ascenders, descenders and neighbouring icons inflate the
    // full band, while the core tracks the lowercase body that dominates a line of running text.
    float xHeight{};
    // Optical FWHM horizontal stem width (px) and active-column ink fill ratio for weight detection.
    float stemWidth{};
    float inkDensity{};
    // Trimmed horizontal bounds (image X) excluding isolated leading/trailing non-text icons.
    int trimLeft{-1};
    int trimRight{-1};
    bool bold{};
};
PixelText InspectTextPixels(const Image& image, RECT box, std::string_view originalHint = {}) {
    PixelText result;
    if (!Valid(box))
        return result;
    std::array<size_t, 4096> histogram{};
    auto key = [](const unsigned char* p) {
        return (int(p[2] >> 4) << 8) | (int(p[1] >> 4) << 4) | (p[0] >> 4);
    };
    const int boxWidth = int(Width(box)), boxHeight = int(Height(box));
    const int step = std::max(1, int(std::sqrt(double(boxWidth) * boxHeight / 500000.)));
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
    int extRingR = br, extRingG = bg, extRingB = bb;
    bool hasExtRing = false;
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
            const int rb = int(ringMode - ringHist.begin());
            const int rbr = ((rb >> 8) & 15) * 16 + 8;
            const int rbg = ((rb >> 4) & 15) * 16 + 8;
            const int rbb = (rb & 15) * 16 + 8;
            extRingR = rbr;
            extRingG = rbg;
            extRingB = rbb;
            hasExtRing = true;
            size_t ringCluster = 0;
            for (int k = 0; k < 4096; ++k) {
                if (!ringHist[size_t(k)])
                    continue;
                const int kr = ((k >> 8) & 15) * 16 + 8;
                const int kg = ((k >> 4) & 15) * 16 + 8;
                const int kb = (k & 15) * 16 + 8;
                if (std::abs(kr - rbr) + std::abs(kg - rbg) + std::abs(kb - rbb) <= 48)
                    ringCluster += ringHist[size_t(k)];
            }
            if ((ringCluster >= ringSampled * .45 ||
                 ((br + bg + bb) >= 660 && (rbr + rbg + rbb) <= 540)) &&
                std::abs(br - rbr) + std::abs(bg - rbg) + std::abs(bb - rbb) > 72) {
                bool flipToRing = *mode < sampled * .45;
                if (!flipToRing && *mode < sampled * .62 &&
                    ((br + bg + bb) >= 660 || (br + bg + bb) <= 105)) {
                    size_t ringInBox = 0;
                    for (int y = box.top; y < box.bottom; y += step)
                        for (int x = box.left; x < box.right; x += step) {
                            const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                            if (std::abs(int(p[2]) - rbr) + std::abs(int(p[1]) - rbg) +
                                    std::abs(int(p[0]) - rbb) <=
                                64)
                                ++ringInBox;
                        }
                    if (ringInBox >= sampled * .10)
                        flipToRing = true;
                }
                if (flipToRing) {
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
    bool reliableBackdrop = backgroundCount >= sampled * .50;
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
    } else if (std::min(boxWidth, boxHeight) <= 24 && std::max(boxWidth, boxHeight) >= 20) {
        // Narrow colored strips (e.g. a 15 px date ribbon) split across bins under dense text; recover
        // the strip's own interior surface from low-gradient pixels before falling back to exterior rings.
        auto diffL1 = [](const unsigned char* a, const unsigned char* b) {
            return std::abs(int(a[2]) - int(b[2])) + std::abs(int(a[1]) - int(b[1])) +
                   std::abs(int(a[0]) - int(b[0]));
        };
        std::array<size_t, 4096> smoothHist{};
        size_t smoothTotal = 0;
        for (int y = box.top; y < box.bottom; ++y)
            for (int x = box.left; x < box.right; ++x) {
                const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                const auto* pl = &image.bgra[(size_t(y) * image.width + std::max(0, x - 1)) * 4];
                const auto* pr = &image.bgra[(size_t(y) * image.width + std::min(image.width - 1, x + 1)) * 4];
                const auto* pu = &image.bgra[(size_t(std::max(0, y - 1)) * image.width + x) * 4];
                const auto* pd =
                    &image.bgra[(size_t(std::min(image.height - 1, y + 1)) * image.width + x) * 4];
                const int grad = std::max({diffL1(p, pl), diffL1(p, pr), diffL1(p, pu), diffL1(p, pd)});
                if (grad <= 28) {
                    const bool rim = (y < box.top + 2 || y >= box.bottom - 2 ||
                                      x < box.left + 2 || x >= box.right - 2);
                    const size_t w = rim ? 2 : 1;
                    smoothHist[key(p)] += w;
                    smoothTotal += w;
                }
            }
        if (smoothTotal >= 24) {
            const auto smMode = std::max_element(smoothHist.begin(), smoothHist.end());
            const int sb = int(smMode - smoothHist.begin());
            const int sr = ((sb >> 8) & 15) * 16 + 8;
            const int sg = ((sb >> 4) & 15) * 16 + 8;
            const int sbl = (sb & 15) * 16 + 8;
            std::array<std::array<size_t, 256>, 3> smCh{};
            size_t smCount = 0;
            for (int y = box.top; y < box.bottom; ++y)
                for (int x = box.left; x < box.right; ++x) {
                    const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                    if (std::abs(int(p[2]) - sr) + std::abs(int(p[1]) - sg) + std::abs(int(p[0]) - sbl) <= 56) {
                        for (int c = 0; c < 3; ++c)
                            ++smCh[c][p[c]];
                        ++smCount;
                    }
                }
            if (smCount >= 16 && smCount * 100 >= size_t(boxWidth) * size_t(boxHeight) * 20) {
                std::array<int, 3> smMed{};
                for (int c = 0; c < 3; ++c) {
                    size_t acc = 0;
                    for (int v = 0; v < 256; ++v) {
                        acc += smCh[c][v];
                        if (acc >= (smCount + 1) / 2) {
                            smMed[c] = v;
                            break;
                        }
                    }
                }
                result.background = RGB(smMed[2], smMed[1], smMed[0]);
                br = smMed[2];
                bg = smMed[1];
                bb = smMed[0];
                reliableBackdrop = true;
            }
        }
    }
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
            if (tBgCount >= tSampled * .45 &&
                !(hasExtRing && (tbr + tbg + tbb) >= 660 && (extRingR + extRingG + extRingB) <= 540)) {
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
    std::vector<unsigned char> inkMark(size_t(boxWidth) * size_t(boxHeight), 0);
    std::vector<uint16_t> distMap(size_t(boxWidth) * size_t(boxHeight), 0);
    std::vector<int> rowCounts(size_t(boxHeight), 0);
    std::vector<int> rowLefts(size_t(boxHeight), box.right);
    std::vector<int> rowRights(size_t(boxHeight), box.left);
    std::vector<unsigned char> rowRule(size_t(boxHeight), 0);
    for (int y = box.top; y < box.bottom; ++y) {
        const size_t ry = size_t(y - box.top);
        int rowCount = 0, rowLeft = box.right, rowRight = box.left;
        int currentRun = 0, maxRun = 0;
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
            const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
            const auto& ref = numTiles > 1 ? tiles[size_t((x - box.left) / kTileWidth)]
                                           : TileBackdrop{rowRed, rowGreen, rowBlue, true};
            const int distance =
                std::abs(int(p[2]) - ref.r) + std::abs(int(p[1]) - ref.g) + std::abs(int(p[0]) - ref.b);
            const size_t idx = ry * size_t(boxWidth) + size_t(x - box.left);
            distMap[idx] = static_cast<uint16_t>(std::min(distance, 765));
            if (distance < 65) {
                currentRun = 0;
                continue;
            }
            ++currentRun;
            maxRun = std::max(maxRun, currentRun);
            ++rowCount;
            rowLeft = std::min(rowLeft, x);
            rowRight = x + 1;
            inkMark[idx] = 1;
        }
        rowCounts[ry] = rowCount;
        rowLefts[ry] = rowLeft;
        rowRights[ry] = rowRight;
        rowRule[ry] = (boxWidth >= 32 && maxRun > std::max(28, int(boxWidth * .55))) ? 1 : 0;
    }
    // Filter 1 px perimeter dotted focus-rectangle rows (e.g. Windows checkbox focus borders) so
    // they neither inflate measured lineHeight nor pollute ink statistics.
    std::vector<unsigned char> dottedRow(size_t(boxHeight), 0);
    if (boxWidth >= 40 && boxHeight >= 8) {
        for (int ry = 0; ry < boxHeight; ++ry) {
            if (ry > 2 && ry < boxHeight - 3)
                continue;
            const int rc = rowCounts[size_t(ry)];
            if (rc < 10 || rc >= boxWidth * 7 / 10)
                continue;
            const int span = rowRights[size_t(ry)] - rowLefts[size_t(ry)];
            if (span < boxWidth * 55 / 100)
                continue;
            int shortRuns = 0, isolatedVert = 0;
            int x = 0;
            while (x < boxWidth) {
                if (!inkMark[size_t(ry) * size_t(boxWidth) + size_t(x)]) {
                    ++x;
                    continue;
                }
                const int x0 = x;
                while (x < boxWidth && inkMark[size_t(ry) * size_t(boxWidth) + size_t(x)]) {
                    const bool up = ry > 0 && inkMark[size_t(ry - 1) * size_t(boxWidth) + size_t(x)];
                    const bool dn =
                        ry + 1 < boxHeight && inkMark[size_t(ry + 1) * size_t(boxWidth) + size_t(x)];
                    if (!up && !dn)
                        ++isolatedVert;
                    ++x;
                }
                if (x - x0 <= 2)
                    ++shortRuns;
            }
            if (shortRuns >= 6 && isolatedVert * 10 >= rc * 7) {
                dottedRow[size_t(ry)] = 1;
                for (int k = 0; k < boxWidth; ++k)
                    inkMark[size_t(ry) * size_t(boxWidth) + size_t(k)] = 0;
            }
        }
    }
    std::vector<RECT> rows;
    int activeTop = -1, left = box.right, right = box.left, lastRow = -1;
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
        const size_t ry = size_t(y - box.top);
        if (!dottedRow[ry] && !rowRule[ry] && rowCounts[ry] >= 2 && rowCounts[ry] < boxWidth * .85) {
            if (activeTop < 0)
                activeTop = y;
            lastRow = y;
            left = std::min(left, rowLefts[ry]);
            right = std::max(right, rowRights[ry]);
        } else if (activeTop >= 0 && y - lastRow > 1) {
            flush();
        }
    }
    flush();

    // Isolate compact leading/trailing non-text icons (checkboxes, avatars, category badges, chevrons)
    // on single-line horizontal UI boxes so they are neither erased nor allowed to hijack glyph color.
    int voteLeft = 0, voteRight = boxWidth;
    if (rows.size() == 1 && boxWidth > boxHeight * 2 && !originalHint.empty()) {
        const int top = int(std::max<LONG>(0, rows[0].top - box.top));
        const int bottom = int(std::min<LONG>(LONG(boxHeight), rows[0].bottom - box.top));
        const int lineH = bottom - top;
        if (lineH >= 7) {
            std::vector<int> colInk(size_t(boxWidth), 0);
            for (int y = top; y < bottom; ++y)
                for (int x = 0; x < boxWidth; ++x)
                    colInk[size_t(x)] += inkMark[size_t(y) * size_t(boxWidth) + size_t(x)];
            std::vector<std::pair<int, int>> clusters;
            int cStart = -1, gap = 0;
            for (int x = 0; x < boxWidth; ++x) {
                if (colInk[size_t(x)] > 0) {
                    if (cStart < 0)
                        cStart = x;
                    gap = 0;
                } else if (cStart >= 0 && ++gap > 2) {
                    clusters.emplace_back(cStart, x - gap + 1);
                    cStart = -1;
                }
            }
            if (cStart >= 0) {
                int endX = boxWidth;
                while (endX > cStart && colInk[size_t(endX - 1)] == 0)
                    --endX;
                if (endX > cStart)
                    clusters.emplace_back(cStart, endX);
            }
            if (clusters.size() >= 2) {
                // Parse leading/trailing token structure from originalHint.
                size_t hs = 0, he = originalHint.size();
                while (hs < he && (originalHint[hs] == ' ' || originalHint[hs] == '\t' ||
                                   originalHint[hs] == '\r' || originalHint[hs] == '\n'))
                    ++hs;
                while (he > hs && (originalHint[he - 1] == ' ' || originalHint[he - 1] == '\t' ||
                                   originalHint[he - 1] == '\r' || originalHint[he - 1] == '\n'))
                    --he;
                const std::string_view trimmed = originalHint.substr(hs, he - hs);
                size_t cpIdx = 0;
                const char32_t firstCp = trimmed.empty() ? 0 : NextCodePoint(trimmed, cpIdx);
                const bool isCjkHint = [&] {
                    size_t k = 0;
                    while (k < trimmed.size()) {
                        const char32_t c = NextCodePoint(trimmed, k);
                        if ((c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
                            (c >= 0xF900 && c <= 0xFAFF))
                            return true;
                    }
                    return false;
                }();
                auto isAsciiAlnum = [](char c) {
                    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
                };
                const bool startsWithSymbol =
                    firstCp != 0 &&
                    !((firstCp >= U'A' && firstCp <= U'Z') || (firstCp >= U'a' && firstCp <= U'z') ||
                      (firstCp >= U'0' && firstCp <= U'9') || (firstCp >= 0x00C0 && firstCp <= 0x024F) ||
                      (firstCp >= 0x2E80 && firstCp <= 0x9FFF) || (firstCp >= 0xAC00 && firstCp <= 0xD7AF));
                size_t firstAsciiWordLen = 0;
                while (firstAsciiWordLen < trimmed.size() && isAsciiAlnum(trimmed[firstAsciiWordLen]))
                    ++firstAsciiWordLen;

                auto clusterStats = [&](int x0, int x1, int& cy0, int& cy1, int& inkCnt, int& mr, int& mg,
                                        int& mb, bool& hollowSquare) {
                    cy0 = bottom;
                    cy1 = top;
                    inkCnt = 0;
                    long long sr = 0, sg = 0, sb = 0;
                    for (int y = top; y < bottom; ++y)
                        for (int x = x0; x < x1; ++x)
                            if (inkMark[size_t(y) * size_t(boxWidth) + size_t(x)]) {
                                cy0 = std::min(cy0, y);
                                cy1 = std::max(cy1, y + 1);
                                const auto* p = &image.bgra[(size_t(box.top + y) * image.width +
                                                             size_t(box.left + x)) *
                                                            4];
                                sr += p[2];
                                sg += p[1];
                                sb += p[0];
                                ++inkCnt;
                            }
                    if (inkCnt > 0) {
                        mr = int(sr / inkCnt);
                        mg = int(sg / inkCnt);
                        mb = int(sb / inkCnt);
                    }
                    const int cw = x1 - x0, ch = cy1 - cy0;
                    hollowSquare = false;
                    if (cw >= 8 && ch >= 8) {
                        if (std::abs(cw - ch) <= std::max(3, cw / 4)) {
                            int topHit = 0, botHit = 0, leftHit = 0, rightHit = 0;
                            for (int x = x0; x < x1; ++x) {
                                if (inkMark[size_t(cy0) * size_t(boxWidth) + size_t(x)] ||
                                    (cy0 + 1 < cy1 && inkMark[size_t(cy0 + 1) * size_t(boxWidth) + size_t(x)]))
                                    ++topHit;
                                if (inkMark[size_t(cy1 - 1) * size_t(boxWidth) + size_t(x)] ||
                                    (cy1 - 2 >= cy0 && inkMark[size_t(cy1 - 2) * size_t(boxWidth) + size_t(x)]))
                                    ++botHit;
                            }
                            for (int y = cy0; y < cy1; ++y) {
                                if (inkMark[size_t(y) * size_t(boxWidth) + size_t(x0)] ||
                                    (x0 + 1 < x1 && inkMark[size_t(y) * size_t(boxWidth) + size_t(x0 + 1)]))
                                    ++leftHit;
                                if (inkMark[size_t(y) * size_t(boxWidth) + size_t(x1 - 1)] ||
                                    (x1 - 2 >= x0 && inkMark[size_t(y) * size_t(boxWidth) + size_t(x1 - 2)]))
                                    ++rightHit;
                            }
                            int innerInk = 0, innerArea = 0;
                            for (int y = cy0 + 2; y < cy1 - 2; ++y)
                                for (int x = x0 + 2; x < x1 - 2; ++x) {
                                    ++innerArea;
                                    innerInk += inkMark[size_t(y) * size_t(boxWidth) + size_t(x)];
                                }
                            if (topHit * 10 >= cw * 6 && botHit * 10 >= cw * 6 &&
                                leftHit * 10 >= ch * 6 && rightHit * 10 >= ch * 6 &&
                                innerArea > 0 && innerInk * 100 <= innerArea * 45)
                                hollowSquare = true;
                        }
                    }
                };
                // Check leading cluster: only strip heterochromatic isolated badges/icons or non-CJK checkboxes.
                {
                    const auto [c0, c1] = clusters.front();
                    const int nextStart = clusters[1].first;
                    const int cw = c1 - c0;
                    const int cGap = nextStart - c1;
                    if (!startsWithSymbol && c0 <= 4 && cGap >= 4 &&
                        cw >= 6 && cw <= int(std::ceil(lineH * 1.85f)) &&
                        (boxWidth - nextStart) >= std::max(cw * 2, 18)) {
                        int cy0 = 0, cy1 = 0, ic = 0, ir = 0, ig = 0, ib = 0;
                        bool hollowSq = false;
                        clusterStats(c0, c1, cy0, cy1, ic, ir, ig, ib, hollowSq);
                        const int ch = cy1 - cy0;
                        int ty0 = 0, ty1 = 0, tc = 0, tr = 0, tg = 0, tb = 0;
                        bool d1 = false;
                        clusterStats(nextStart, boxWidth, ty0, ty1, tc, tr, tg, tb, d1);
                        const int colorDiff =
                            (ic > 0 && tc > 0) ? (std::abs(ir - tr) + std::abs(ig - tg) + std::abs(ib - tb)) : 0;
                        const bool iconAspect = ch >= 6 && cw * 10 <= ch * 19 && ch * 10 <= cw * 19;
                        const bool condHeteroIcon =
                            colorDiff >= 95 && cw <= int(std::lround(lineH * 1.35f)) && cGap >= 4 &&
                            (firstAsciiWordLen >= 2 || isCjkHint);
                        const bool condCheckbox =
                            !isCjkHint && firstAsciiWordLen >= 2 && cw >= 10 && cw <= 22 &&
                            ch >= 10 && ch <= 22 && std::abs(cw - ch) <= 2 && cGap >= 5 &&
                            (hollowSq || (ib - ir >= 50 && ib >= 140));
                        if (iconAspect && (condHeteroIcon || condCheckbox)) {
                            voteLeft = nextStart;
                            result.trimLeft = box.left + std::max(c1 + 2, nextStart - 1);
                        }
                    }
                }
            }
        }
    }

    // Strip isolated narrow or heterochromatic top/bottom scanlines (e.g. chart data points or pill border
    // ticks caught in a single-line box) so they neither inflate lineHeight nor get erased by buildCover.
    if (rows.size() == 1 && (rows[0].bottom - rows[0].top) >= 14) {
        const int rTop = int(std::max<LONG>(0, rows[0].top - box.top));
        const int rBot = int(std::min<LONG>(LONG(boxHeight), rows[0].bottom - box.top));
        const int rH = rBot - rTop;
        if (rH >= 14 && voteRight > voteLeft) {
            std::vector<int> rCnt(size_t(rH), 0), rSpan(size_t(rH), 0);
            std::vector<int> rR(size_t(rH), 0), rG(size_t(rH), 0), rB(size_t(rH), 0);
            int peakCnt = 0, peakSpan = 0;
            for (int y = rTop; y < rBot; ++y) {
                const size_t idxY = size_t(y - rTop);
                int cnt = 0, minX = voteRight, maxX = voteLeft;
                long long sr = 0, sg = 0, sb = 0;
                for (int x = voteLeft; x < voteRight; ++x) {
                    if (inkMark[size_t(y) * size_t(boxWidth) + size_t(x)]) {
                        ++cnt;
                        minX = std::min(minX, x);
                        maxX = std::max(maxX, x + 1);
                        const auto* p =
                            &image.bgra[(size_t(box.top + y) * image.width + size_t(box.left + x)) * 4];
                        sr += p[2];
                        sg += p[1];
                        sb += p[0];
                    }
                }
                rCnt[idxY] = cnt;
                rSpan[idxY] = cnt > 0 ? (maxX - minX) : 0;
                if (cnt > 0) {
                    rR[idxY] = int(sr / cnt);
                    rG[idxY] = int(sg / cnt);
                    rB[idxY] = int(sb / cnt);
                }
                peakCnt = std::max(peakCnt, cnt);
                peakSpan = std::max(peakSpan, rSpan[idxY]);
            }
            if (peakCnt >= 4 && peakSpan >= 16) {
                long long mr = 0, mg = 0, mb = 0, mc = 0;
                for (int i = 0; i < rH; ++i)
                    if (rCnt[size_t(i)] * 2 >= peakCnt) {
                        mr += static_cast<long long>(rR[size_t(i)]) * rCnt[size_t(i)];
                        mg += static_cast<long long>(rG[size_t(i)]) * rCnt[size_t(i)];
                        mb += static_cast<long long>(rB[size_t(i)]) * rCnt[size_t(i)];
                        mc += rCnt[size_t(i)];
                    }
                const int mainR = mc > 0 ? int(mr / mc) : 0;
                const int mainG = mc > 0 ? int(mg / mc) : 0;
                const int mainB = mc > 0 ? int(mb / mc) : 0;
                auto isOutlierScanline = [&](int i) {
                    if (rCnt[size_t(i)] == 0)
                        return true;
                    const int colDiff = std::abs(rR[size_t(i)] - mainR) + std::abs(rG[size_t(i)] - mainG) +
                                        std::abs(rB[size_t(i)] - mainB);
                    return rSpan[size_t(i)] * 100 < peakSpan * 28 &&
                           (colDiff >= 60 || rCnt[size_t(i)] * 5 < peakCnt);
                };
                int trimT = 0;
                while (trimT < rH / 2 && isOutlierScanline(trimT))
                    ++trimT;
                int trimB = rH;
                while (trimB > trimT + 7 && (rH - trimB) < rH / 2 && isOutlierScanline(trimB - 1))
                    --trimB;
                if ((trimT > 0 || trimB < rH) && (trimB - trimT) >= 7) {
                    rows[0].top = box.top + rTop + trimT;
                    rows[0].bottom = box.top + rTop + trimB;
                }
            }
        }
    }

    // The dense core of each line is the lowercase body: rows within half the peak ink density. The
    // column projection first isolates the run of text from icons and list markers, because a narrow
    // glyph-shaped element next to the line would otherwise hold the core open to its full height.
    std::vector<std::pair<int, int>> rowCoreSpans;
    rowCoreSpans.reserve(rows.size());
    if (boxWidth > 0 && boxHeight > 0) {
        std::vector<float> cores;
        std::vector<int> columnInk(size_t(boxWidth), 0);
        std::vector<int> lineInk;
        for (const auto& line : rows) {
            const int top = int(std::max<LONG>(0, line.top - box.top));
            const int bottom = int(std::min<LONG>(LONG(boxHeight), line.bottom - box.top));
            if (bottom - top < 3) {
                rowCoreSpans.emplace_back(voteLeft, voteRight);
                continue;
            }
            std::fill(columnInk.begin(), columnInk.end(), 0);
            for (int y = top; y < bottom; ++y)
                for (int x = voteLeft; x < voteRight; ++x)
                    columnInk[size_t(x)] += inkMark[size_t(y) * size_t(boxWidth) + size_t(x)];
            int bestLeft = voteLeft, bestRight = voteLeft;
            int runStart = -1, gap = 0;
            auto closeRun = [&](int end) {
                if (runStart >= 0 && end - runStart > bestRight - bestLeft) {
                    bestLeft = runStart;
                    bestRight = end;
                }
                runStart = -1;
            };
            for (int x = voteLeft; x < voteRight; ++x) {
                if (columnInk[size_t(x)] > 0) {
                    if (runStart < 0)
                        runStart = x;
                    gap = 0;
                } else if (runStart >= 0 && ++gap > 2) {
                    closeRun(x - gap + 1);
                }
            }
            closeRun(voteRight);
            // A run narrower than twice the line height is a marker, so the whole line stays scanned.
            if (bestRight - bestLeft < (bottom - top) * 2 || bestRight <= bestLeft) {
                bestLeft = voteLeft;
                bestRight = voteRight;
            }
            rowCoreSpans.emplace_back(bestLeft, bestRight);
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

    // Accumulate foreground color histogram over the core horizontal text span using each pixel's
    // own local backdrop distance so marginal icons and background vignettes cannot corrupt text color.
    std::array<size_t, 4096> counts{}, rs{}, gs{}, bs{};
    std::array<unsigned long long, 4096> distSums{};
    size_t foreground = 0;
    for (size_t ri = 0; ri < rows.size(); ++ri) {
        const int top = int(std::max<LONG>(0, rows[ri].top - box.top));
        const int bottom = int(std::min<LONG>(LONG(boxHeight), rows[ri].bottom - box.top));
        const int sx0 = ri < rowCoreSpans.size() ? rowCoreSpans[ri].first : voteLeft;
        const int sx1 = ri < rowCoreSpans.size() ? rowCoreSpans[ri].second : voteRight;
        for (int y = top; y < bottom; ++y) {
            if (dottedRow[size_t(y)] || rowRule[size_t(y)])
                continue;
            for (int x = sx0; x < sx1; ++x) {
                const size_t idx = size_t(y) * size_t(boxWidth) + size_t(x);
                if (!inkMark[idx])
                    continue;
                const int localDist = int(distMap[idx]);
                if (localDist < 65)
                    continue;
                const auto* p = &image.bgra[(size_t(box.top + y) * image.width + size_t(box.left + x)) * 4];
                const auto k = key(p);
                ++counts[k];
                rs[k] += p[2];
                gs[k] += p[1];
                bs[k] += p[0];
                distSums[k] += static_cast<unsigned long long>(localDist);
                ++foreground;
            }
        }
    }

    // Detect bimodal outlined text (e.g. manga black fill with crisp white outline on mid-gray):
    // true backdrop adjacency identifies the outer rim while tight opposite-polarity wrapping identifies the fill.
    const int bgLumGlobal = int(.299 * br + .587 * bg + .114 * bb + .5);
    double selectedFgDist = 200.0;
    if (foreground >= 24 && reliableBackdrop && bgLumGlobal >= 70 && bgLumGlobal <= 195) {
        size_t lightCnt = 0, darkCnt = 0;
        size_t lightBgAdj = 0, darkBgAdj = 0;
        size_t lightOppAdj = 0, darkOppAdj = 0;
        long long lR = 0, lG = 0, lB = 0, dR = 0, dG = 0, dB = 0;
        const int lightLumMin = std::max(204, bgLumGlobal + 20);
        const int darkLumMax = std::min(65, bgLumGlobal - 20);
        std::vector<signed char> pol(size_t(boxWidth) * size_t(boxHeight), 0);
        for (int y = 0; y < boxHeight; ++y) {
            for (int x = voteLeft; x < voteRight; ++x) {
                const size_t idx = size_t(y) * size_t(boxWidth) + size_t(x);
                if (distMap[idx] < 42)
                    continue;
                const auto* p = &image.bgra[(size_t(box.top + y) * image.width + size_t(box.left + x)) * 4];
                const int lum = int(.299 * p[2] + .587 * p[1] + .114 * p[0] + .5);
                if (lum >= lightLumMin) {
                    pol[idx] = 1;
                    ++lightCnt;
                    lR += p[2];
                    lG += p[1];
                    lB += p[0];
                } else if (lum <= darkLumMax) {
                    pol[idx] = -1;
                    ++darkCnt;
                    dR += p[2];
                    dG += p[1];
                    dB += p[0];
                }
            }
        }
        const size_t bimodalTotal = lightCnt + darkCnt;
        if (lightCnt >= std::max<size_t>(14, bimodalTotal * 22 / 100) &&
            darkCnt >= std::max<size_t>(14, bimodalTotal * 22 / 100)) {
            // Mark true exterior backdrop pixels (close to background and NOT a 1px black-to-white transition).
            std::vector<unsigned char> trueBg(size_t(boxWidth) * size_t(boxHeight), 0);
            for (int y = 0; y < boxHeight; ++y)
                for (int x = 0; x < boxWidth; ++x) {
                    const size_t idx = size_t(y) * size_t(boxWidth) + size_t(x);
                    if (pol[idx] != 0 || distMap[idx] > 42)
                        continue;
                    bool hasL = false, hasD = false;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && nx < boxWidth && ny >= 0 && ny < boxHeight) {
                                const signed char np = pol[size_t(ny) * size_t(boxWidth) + size_t(nx)];
                                if (np > 0)
                                    hasL = true;
                                else if (np < 0)
                                    hasD = true;
                            }
                        }
                    if (!(hasL && hasD))
                        trueBg[idx] = 1;
                }
            for (int y = 0; y < boxHeight; ++y) {
                for (int x = voteLeft; x < voteRight; ++x) {
                    const size_t idx = size_t(y) * size_t(boxWidth) + size_t(x);
                    const signed char pl = pol[idx];
                    if (pl == 0)
                        continue;
                    bool touchBg = false;
                    for (int dy = -1; dy <= 1 && !touchBg; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && nx < boxWidth && ny >= 0 && ny < boxHeight &&
                                trueBg[size_t(ny) * size_t(boxWidth) + size_t(nx)]) {
                                touchBg = true;
                                break;
                            }
                        }
                    bool touchOpp = false;
                    for (int dy = -2; dy <= 2 && !touchOpp; ++dy)
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && nx < boxWidth && ny >= 0 && ny < boxHeight &&
                                pol[size_t(ny) * size_t(boxWidth) + size_t(nx)] == -pl) {
                                touchOpp = true;
                                break;
                            }
                        }
                    if (pl > 0) {
                        if (touchBg)
                            ++lightBgAdj;
                        if (touchOpp)
                            ++lightOppAdj;
                    } else {
                        if (touchBg)
                            ++darkBgAdj;
                        if (touchOpp)
                            ++darkOppAdj;
                    }
                }
            }
            const double lightBgRate = double(lightBgAdj) / double(lightCnt);
            const double darkBgRate = double(darkBgAdj) / double(darkCnt);
            const double lightOppRate = double(lightOppAdj) / double(lightCnt);
            const double darkOppRate = double(darkOppAdj) / double(darkCnt);
            auto avgCol = [](long long r, long long g, long long b, size_t n, bool snapHigh) {
                const long long denom = static_cast<long long>(n);
                int cr = int(r / denom), cg = int(g / denom), cb = int(b / denom);
                if (std::max({cr, cg, cb}) - std::min({cr, cg, cb}) <= 24) {
                    const int neutral = std::clamp(int(.299 * cr + .587 * cg + .114 * cb + .5), 0, 255);
                    cr = cg = cb = neutral;
                }
                if (snapHigh && cr >= 225 && cg >= 225 && cb >= 225)
                    cr = cg = cb = 255;
                return RGB(cr, cg, cb);
            };
            if (darkOppRate >= 0.55 && lightOppRate >= 0.45) {
                if (lightBgRate > darkBgRate * 1.25 && lightBgAdj >= 8) {
                    result.color = avgCol(dR, dG, dB, darkCnt, false);
                    result.hasStroke = true;
                    result.strokeColor = avgCol(lR, lG, lB, lightCnt, true);
                    selectedFgDist =
                        double(std::abs(int(GetRValue(*result.color)) - br) +
                               std::abs(int(GetGValue(*result.color)) - bg) +
                               std::abs(int(GetBValue(*result.color)) - bb));
                } else if (darkBgRate > lightBgRate * 1.25 && darkBgAdj >= 8) {
                    result.color = avgCol(lR, lG, lB, lightCnt, true);
                    result.hasStroke = true;
                    result.strokeColor = avgCol(dR, dG, dB, darkCnt, false);
                    selectedFgDist =
                        double(std::abs(int(GetRValue(*result.color)) - br) +
                               std::abs(int(GetGValue(*result.color)) - bg) +
                               std::abs(int(GetBValue(*result.color)) - bb));
                }
            }
        }
    }

    // Standard foreground voting with 95th-percentile distance gate (>= 1% bins) and d^1.5 weighting.
    const size_t boxArea = size_t(boxWidth) * size_t(boxHeight);
    if (!result.color.has_value() && foreground >= 2 && foreground <= boxArea * 85 / 100) {
        struct BinEntry {
            int idx;
            size_t count;
            double dist;
        };
        std::vector<BinEntry> bins;
        const size_t minBinCount = std::max<size_t>(2, foreground / 100);
        for (int i = 0; i < 4096; ++i)
            if (counts[i] >= minBinCount)
                bins.push_back({i, counts[i], double(distSums[i]) / double(counts[i])});
        if (bins.empty()) {
            for (int i = 0; i < 4096; ++i)
                if (counts[i] >= 2)
                    bins.push_back({i, counts[i], double(distSums[i]) / double(counts[i])});
        }
        if (!bins.empty()) {
            std::sort(bins.begin(), bins.end(),
                      [](const BinEntry& a, const BinEntry& b) { return a.dist < b.dist; });
            size_t totalBinPx = 0;
            for (const auto& b : bins)
                totalBinPx += b.count;
            const size_t p95Target = std::max<size_t>(1, totalBinPx * 95 / 100);
            size_t acc = 0;
            double p95Dist = bins.back().dist;
            for (const auto& b : bins) {
                acc += b.count;
                if (acc >= p95Target) {
                    p95Dist = b.dist;
                    break;
                }
            }
            double best = 0;
            int selected = -1;
            for (int i = 0; i < 4096; ++i)
                if (counts[i] >= 2) {
                    const double distance = double(distSums[i]) / double(counts[i]);
                    if (distance < p95Dist * .58)
                        continue;
                    const double weight = double(counts[i]) * distance * std::sqrt(distance);
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
                const int neutral = std::clamp(int(.299 * cr + .587 * cg + .114 * cb + .5), 0, 255);
                if (bgChroma <= 20 &&
                    (fgChroma <= 24 || (bgLumGlobal >= 200 && neutral <= 85 && fgChroma <= 52))) {
                    cr = cg = cb = neutral;
                } else if (bgChroma >= 45 && std::max({cr, cg, cb}) >= 238 &&
                           std::min({cr, cg, cb}) >= 200) {
                    // Subpixel anti-aliasing on saturated UI bubbles tints pure white glyphs cyan/mint.
                    cr = cg = cb = 255;
                }
                result.color = RGB(cr, cg, cb);
                selectedFgDist = double(distSums[selected]) / double(counts[selected]);
            }
        }
    }

    // Fallback for dense or glowing display text (where outer glow pushes row ink >= 85% of box width):
    // sample the highest-contrast interior core pixels relative to the exterior perimeter.
    if (!result.color.has_value() && boxWidth >= 6 && boxHeight >= 6) {
        const int refR = hasExtRing ? extRingR : br;
        const int refG = hasExtRing ? extRingG : bg;
        const int refB = hasExtRing ? extRingB : bb;
        const int y0 = int(box.top) + boxHeight / 5;
        const int y1 = std::max(y0 + 1, int(box.bottom) - boxHeight / 5);
        const int x0 = int(box.left) + voteLeft;
        const int x1 = std::max(x0 + 1, int(box.left) + voteRight);
        std::vector<int> coreDists;
        coreDists.reserve(size_t(y1 - y0) * size_t(x1 - x0));
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                const int d = std::abs(int(p[2]) - refR) + std::abs(int(p[1]) - refG) +
                              std::abs(int(p[0]) - refB);
                if (d >= 65)
                    coreDists.push_back(d);
            }
        if (coreDists.size() >= 8) {
            const size_t p80Idx = coreDists.size() * 80 / 100;
            std::nth_element(coreDists.begin(), coreDists.begin() + p80Idx, coreDists.end());
            const int dThresh = std::max(65, coreDists[p80Idx]);
            std::array<size_t, 4096> fbCnt{}, fbR{}, fbG{}, fbB{};
            int fbBest = -1;
            size_t fbBestCnt = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                    const int d = std::abs(int(p[2]) - refR) + std::abs(int(p[1]) - refG) +
                                  std::abs(int(p[0]) - refB);
                    if (d < dThresh)
                        continue;
                    const int k = key(p);
                    ++fbCnt[k];
                    fbR[k] += p[2];
                    fbG[k] += p[1];
                    fbB[k] += p[0];
                    if (fbCnt[k] > fbBestCnt) {
                        fbBestCnt = fbCnt[k];
                        fbBest = k;
                    }
                }
            if (fbBest >= 0 && fbBestCnt >= 3) {
                int cr = int(fbR[fbBest] / fbCnt[fbBest]);
                int cg = int(fbG[fbBest] / fbCnt[fbBest]);
                int cb = int(fbB[fbBest] / fbCnt[fbBest]);
                const int bgChroma = std::max({refR, refG, refB}) - std::min({refR, refG, refB});
                const int fgChroma = std::max({cr, cg, cb}) - std::min({cr, cg, cb});
                if (bgChroma <= 20 && fgChroma <= 24) {
                    const int neutral = std::clamp(int(.299 * cr + .587 * cg + .114 * cb + .5), 0, 255);
                    cr = cg = cb = neutral;
                } else if (std::max({cr, cg, cb}) >= 238 && std::min({cr, cg, cb}) >= 200) {
                    cr = cg = cb = 255;
                }
                result.color = RGB(cr, cg, cb);
                selectedFgDist = double(dThresh);
            }
        }
    }

    // Measure optical FWHM horizontal stroke stem width and active-column ink density across dense
    // core rows so bold weight is driven by actual stroke thickness rather than neighbor height jumps.
    if (!rows.empty() && boxWidth > 0 && boxHeight > 0) {
        const int fwhmThresh = std::clamp(int(selectedFgDist * 0.55), 85, 420);
        std::vector<int> stemRuns;
        size_t fwhmPixels = 0, activePixels = 0;
        for (size_t ri = 0; ri < rows.size(); ++ri) {
            const int top = int(std::max<LONG>(0, rows[ri].top - box.top));
            const int bottom = int(std::min<LONG>(LONG(boxHeight), rows[ri].bottom - box.top));
            const int lineH = bottom - top;
            if (lineH < 5)
                continue;
            const int sx0 = ri < rowCoreSpans.size() ? rowCoreSpans[ri].first : voteLeft;
            const int sx1 = ri < rowCoreSpans.size() ? rowCoreSpans[ri].second : voteRight;
            if (sx1 - sx0 < 6)
                continue;
            std::vector<int> lInk(size_t(lineH), 0);
            std::vector<unsigned char> activeCol(size_t(sx1 - sx0), 0);
            int peak = 0;
            for (int y = top; y < bottom; ++y) {
                int c = 0;
                for (int x = sx0; x < sx1; ++x) {
                    if (inkMark[size_t(y) * size_t(boxWidth) + size_t(x)]) {
                        ++c;
                        activeCol[size_t(x - sx0)] = 1;
                    }
                }
                lInk[size_t(y - top)] = c;
                peak = std::max(peak, c);
            }
            if (peak < 3)
                continue;
            size_t rowActiveCols = 0;
            for (unsigned char ac : activeCol)
                rowActiveCols += ac;
            const int maxStem = std::max(3, int(std::ceil(float(lineH) * 0.42f)));
            for (int y = top; y < bottom; ++y) {
                if (lInk[size_t(y - top)] * 2 < peak)
                    continue;
                activePixels += rowActiveCols;
                int x = sx0;
                while (x < sx1) {
                    if (int(distMap[size_t(y) * size_t(boxWidth) + size_t(x)]) < fwhmThresh) {
                        ++x;
                        continue;
                    }
                    const int x0 = x;
                    while (x < sx1 && int(distMap[size_t(y) * size_t(boxWidth) + size_t(x)]) >= fwhmThresh)
                        ++x;
                    const int len = x - x0;
                    fwhmPixels += size_t(len);
                    if (len <= maxStem)
                        stemRuns.push_back(len);
                }
            }
        }
        if (stemRuns.size() >= 6) {
            std::sort(stemRuns.begin(), stemRuns.end());
            const size_t lo = stemRuns.size() / 5;
            const size_t hi = std::max(lo + 1, stemRuns.size() - stemRuns.size() / 5);
            double sum = 0;
            for (size_t k = lo; k < hi; ++k)
                sum += double(stemRuns[k]);
            result.stemWidth = float(sum / double(hi - lo));
            result.inkDensity = float(fwhmPixels) / float(std::max<size_t>(1, activePixels));
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
// Unrefined OCR boxes around small CJK lines carry detector padding similar to Latin lines; calibrate
// the ink share smoothly so 12 px CJK and Latin peers land on the same typographic scale while large
// CJK headings keep their full visual cap height.
float InkBandCjk(float band) {
    const float share = .78f + .16f * std::clamp((band - 14.f) / 24.f, 0.f, 1.f);
    return band * share;
}
// Measure the main kanji column's horizontal dense-ink core width inside a vertical manga strip so
// narrow ruby/furigana sitting beside the column does not inflate the bubble's font size.
float MeasureVerticalStripCoreWidth(const Image& image, RECT strip, std::optional<COLORREF> bgHint) {
    strip.left = std::clamp<LONG>(strip.left, 0, image.width);
    strip.top = std::clamp<LONG>(strip.top, 0, image.height);
    strip.right = std::clamp<LONG>(strip.right, 0, image.width);
    strip.bottom = std::clamp<LONG>(strip.bottom, 0, image.height);
    const int w = int(Width(strip)), h = int(Height(strip));
    if (w < 14 || h < 18)
        return float(std::max(1, w));
    int br = 255, bg = 255, bb = 255;
    if (bgHint.has_value()) {
        br = GetRValue(*bgHint);
        bg = GetGValue(*bgHint);
        bb = GetBValue(*bgHint);
    } else {
        std::array<size_t, 4096> hist{};
        for (int y = strip.top; y < strip.bottom; ++y)
            for (int x = strip.left; x < strip.right; ++x) {
                const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                ++hist[(int(p[2] >> 4) << 8) | (int(p[1] >> 4) << 4) | (p[0] >> 4)];
            }
        const int mb = int(std::max_element(hist.begin(), hist.end()) - hist.begin());
        br = ((mb >> 8) & 15) * 16 + 8;
        bg = ((mb >> 4) & 15) * 16 + 8;
        bb = (mb & 15) * 16 + 8;
    }
    std::vector<int> colInk(size_t(w), 0);
    int peak = 0;
    for (int y = strip.top; y < strip.bottom; ++y)
        for (int x = strip.left; x < strip.right; ++x) {
            const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
            if (std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb) >= 65) {
                const int v = ++colInk[size_t(x - strip.left)];
                peak = std::max(peak, v);
            }
        }
    if (peak < 5)
        return float(w);
    struct Run {
        int start, end;
        long long mass;
    };
    std::vector<Run> runs;
    int rStart = -1;
    long long rMass = 0;
    for (int x = 0; x < w; ++x) {
        if (colInk[size_t(x)] * 100 >= peak * 18) {
            if (rStart < 0) {
                rStart = x;
                rMass = 0;
            }
            rMass += colInk[size_t(x)];
        } else if (rStart >= 0) {
            runs.push_back({rStart, x, rMass});
            rStart = -1;
        }
    }
    if (rStart >= 0)
        runs.push_back({rStart, w, rMass});
    if (runs.size() < 2)
        return float(w);
    size_t mainIdx = 0;
    for (size_t k = 1; k < runs.size(); ++k)
        if (runs[k].mass > runs[mainIdx].mass)
            mainIdx = k;
    const auto& mainRun = runs[mainIdx];
    long long otherMaxMass = 0;
    for (size_t k = 0; k < runs.size(); ++k)
        if (k != mainIdx)
            otherMaxMass = std::max(otherMaxMass, runs[k].mass);
    const int mainW = mainRun.end - mainRun.start;
    if (mainW >= std::max(8, w * 48 / 100) && mainW <= w - 3 && otherMaxMass * 100 <= mainRun.mass * 48)
        return std::clamp(float(mainW + 2), float(w) * 0.55f, float(w));
    return float(w);
}
bool HasSentencePunctForCenter(std::wstring_view s) {
    // Exclude inline numeric decimals (digit '.' digit) and short balanced label parentheses (...)/(...)
    // so centered metrics ("928.8k") and labeled tabs ("公式X(Twitter)") remain eligible for centering.
    constexpr std::wstring_view kStrictPunct = L"。，、；：？！“”‘’「」『』【】[].,;:?!\"؟؛،।॥";
    auto isDigit = [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= 0xFF10 && c <= 0xFF19);
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t ch = s[i];
        if (ch == L'.' && i > 0 && i + 1 < s.size() && isDigit(s[i - 1]) && isDigit(s[i + 1]))
            continue;
        if (kStrictPunct.find(ch) != std::wstring_view::npos)
            return true;
        if (ch == L'(' || ch == L'（') {
            const wchar_t close = (ch == L'(') ? L')' : L'）';
            const size_t end = s.find(close, i + 1);
            if (end != std::wstring_view::npos && end - i <= 18) {
                i = end;
                continue;
            }
            return true;
        }
        if (ch == L')' || ch == L'）')
            return true;
    }
    return false;
}
wchar_t VerticalPunctuationForm(wchar_t ch) {
    switch (ch) {
    case L'…':
        return L'\xFE19'; // ︙
    case L'—':
    case L'―':
    case L'─':
    case L'ー':
        return L'\xFE31'; // ︱
    case L'「':
        return L'\xFE41'; // ﹁
    case L'」':
        return L'\xFE42'; // ﹂
    case L'『':
        return L'\xFE43'; // ﹃
    case L'』':
        return L'\xFE44'; // ﹄
    case L'（':
    case L'(':
        return L'\xFE35'; // ︵
    case L'）':
    case L')':
        return L'\xFE36'; // ︶
    case L'【':
        return L'\xFE3B'; // ︻
    case L'】':
        return L'\xFE3C'; // ︼
    default:
        return ch;
    }
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
    const std::wstring wOrig = Wide(s.original);
    const std::wstring normTrans = NormalizeAsciiAlnum(wTrans);
    const std::wstring normOrig = NormalizeAsciiAlnum(wOrig);
    if (cjkTarget && normTrans.empty())
        return true;
    if (!normTrans.empty() && normTrans == normOrig)
        return true;
    const bool origAsciiOnly = std::all_of(wOrig.begin(), wOrig.end(), [](wchar_t ch) {
        return ch <= 0x007F;
    });
    if (cjkTarget && origAsciiOnly && !normOrig.empty() && normTrans.size() <= 2 && wTrans.size() <= 4)
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
    const size_t translatedSegmentCount = segments.size();
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
    std::vector<float> stemWidths(segments.size());
    std::vector<float> inkDensities(segments.size());
    std::vector<bool> pixelRefined(segments.size());
    std::vector<std::optional<COLORREF>> surfaceColors(segments.size());
    std::vector<std::optional<COLORREF>> textColors(segments.size());
    std::vector<bool> columns(segments.size());
    std::vector<RECT> suppressedRects(segments.size());
    const RECT bounds{0, 0, image.width, image.height};
    const float scale = dpi / 96.f;
    const bool cjkTarget = settings.screenTargetLanguage == "zh-Hans" ||
                           settings.screenTargetLanguage == "zh-Hant" ||
                           settings.screenTargetLanguage == "ja" ||
                           settings.screenTargetLanguage == "ko";
    auto measureVerticalMedianWidth = [&](const std::vector<RECT>& lines, RECT fallback,
                                          std::optional<COLORREF> bgHint) {
        std::vector<float> widths;
        for (const auto& line : lines)
            if (Width(line) > 0)
                widths.push_back(MeasureVerticalStripCoreWidth(image, line, bgHint));
        if (widths.empty())
            widths.push_back(MeasureVerticalStripCoreWidth(image, fallback, bgHint));
        std::sort(widths.begin(), widths.end());
        return std::max(1.f, widths[widths.size() / 2]);
    };
    for (size_t i = 0; i < output.size(); ++i) {
        const auto& s = segments[i];
        if (s.positionUnavailable || s.untranslated) {
            output[i].suppressed = true;
            continue;
        }
        auto r = s.sourcePixels.value_or(MapBox(s.box, image));
        RECT clipped{};
        IntersectRect(&clipped, &r, &bounds);
        if (!Valid(clipped)) {
            output[i].suppressed = true;
            continue;
        }
        // Empty-text detections originating from recognized segments (not uncoveredLines) or tiny
        // square checkbox/icon symbols must never run coverOnly erasure and destroy non-text UI glyphs.
        const bool emptyText = s.translated.empty() && s.original.empty();
        if (emptyText) {
            const int cw = Width(clipped), ch = Height(clipped);
            const bool tinySquareIcon =
                cw <= 24 && ch <= 24 && cw * 10 >= ch * 7 && ch * 10 >= cw * 7;
            if (i < translatedSegmentCount || tinySquareIcon) {
                output[i].suppressed = true;
                suppressedRects[i] = clipped;
                continue;
            }
        }
        if (ShouldKeepOriginalPixels(s, cjkTarget)) {
            output[i].suppressed = true;
            suppressedRects[i] = clipped;
            continue;
        }
        auto pixels = InspectTextPixels(image, clipped, s.original);
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
        // Trim isolated leading/trailing non-text icons (checkboxes, avatars, badges, chevrons) from
        // single-line horizontal UI boxes so buildCover preserves the icon and text starts cleanly beside it.
        if (!IsVerticalSource(s.sourceLines, clipped) && output[i].detectedLines.size() <= 1) {
            if (pixels.trimLeft > clipped.left && pixels.trimLeft < clipped.right - 8) {
                clipped.left = pixels.trimLeft;
                if (!output[i].detectedLines.empty())
                    output[i].detectedLines[0].left =
                        std::min(output[i].detectedLines[0].right - 1, LONG(pixels.trimLeft));
            }
            if (pixels.trimRight > clipped.left + 8 && pixels.trimRight < clipped.right) {
                clipped.right = pixels.trimRight;
                if (!output[i].detectedLines.empty())
                    output[i].detectedLines[0].right =
                        std::max(output[i].detectedLines[0].left + 1, LONG(pixels.trimRight));
            }
        }
        // A vertical column carries its run length in the band and its glyph size in the strip width. The
        // glyph size is what the face follows, so the column takes its own measurement: one strip, or the
        // median strip of a balloon that wraps across several of them (excluding attached furigana).
        if (IsVerticalSource(s.sourceLines, clipped)) {
            columns[i] = true;
            lineHeights[i] = measureVerticalMedianWidth(s.sourceLines, clipped, pixels.background);
        }
        // AI boxes often touch the captured edge, so partial glyph rows still describe the line rhythm.
        // Accept every detected row set with a plausible line height instead of requiring a fully
        // enclosed paragraph.
        const bool detectedRows = !pixels.lines.empty() && pixels.lineHeight >= 8;
        if (!s.sourcePixels && (measuredParagraph || detectedRows)) {
            // A model rectangle carries no row geometry, so the measured rows describe the passage.
            output[i].detectedLines = pixels.lines;
            lineHeights[i] = pixels.lineHeight;
            pixelRefined[i] = true;
        } else if (!columns[i] && s.sourcePixels && detectedRows &&
                   pixels.lines.size() == s.sourceLines.size() &&
                   pixels.lineHeight >= lineHeights[i] * .60f &&
                   pixels.lineHeight <= lineHeights[i] * .95f) {
            // Refine both Latin and CJK padded detector bands using measured pixel-row ink heights so
            // small CJK and Latin peers share the same typographic scale, and update single-line vertical
            // bounds when an isolated top/bottom plot dot was stripped.
            lineHeights[i] = pixels.lineHeight;
            pixelRefined[i] = true;
            if (output[i].detectedLines.size() == 1 && pixels.lines.size() == 1 &&
                Valid(pixels.lines[0])) {
                output[i].detectedLines[0].top =
                    std::max(output[i].detectedLines[0].top, pixels.lines[0].top);
                output[i].detectedLines[0].bottom =
                    std::min(output[i].detectedLines[0].bottom, pixels.lines[0].bottom);
            }
        }
        if (settings.matchTextColor)
            output[i].sourceColor = pixels.color;
        output[i].hasStroke = pixels.hasStroke;
        output[i].strokeColor = pixels.strokeColor;
        output[i].bold = false;
        coreHeights[i] = pixels.xHeight;
        stemWidths[i] = pixels.stemWidth;
        inkDensities[i] = pixels.inkDensity;
        textColors[i] = pixels.color;
        surfaceColors[i] = pixels.background;
        output[i].sourceRect = clipped;
    }

    // Page-level vertical manga pass: on vertical-dominant pages (>= 50% vertical CJK columns) or
    // directly adjacent to a vertical column in the same speech bubble, lower the vertical aspect
    // threshold to Height >= Width so short 2-char columns render vertically without clipping.
    {
        size_t cjkTotal = 0, cjkVertical = 0;
        for (size_t i = 0; i < translatedSegmentCount; ++i) {
            if (output[i].suppressed || !Valid(output[i].sourceRect))
                continue;
            if (AnalyzeText(segments[i].original).cjk) {
                ++cjkTotal;
                if (columns[i])
                    ++cjkVertical;
            }
        }
        const bool verticalDominantPage = cjkVertical >= 2 && cjkVertical * 2 >= cjkTotal;
        for (size_t i = 0; i < translatedSegmentCount; ++i) {
            if (output[i].suppressed || columns[i] || !Valid(output[i].sourceRect))
                continue;
            if (!AnalyzeText(segments[i].original).cjk || output[i].detectedLines.size() > 1)
                continue;
            const RECT box = output[i].sourceRect;
            if (Width(box) <= 0 || Height(box) < Width(box))
                continue;
            const std::wstring wOrig = Wide(segments[i].original);
            if (wOrig.size() > 4)
                continue;
            bool adjacentVertical = verticalDominantPage;
            if (!adjacentVertical) {
                for (size_t j = 0; j < translatedSegmentCount; ++j) {
                    if (i == j || output[j].suppressed || !columns[j] || !Valid(output[j].sourceRect))
                        continue;
                    const RECT o = output[j].sourceRect;
                    const LONG vOv = std::min(box.bottom, o.bottom) - std::max(box.top, o.top);
                    const LONG hGap = std::max(0L, std::max(box.left, o.left) - std::min(box.right, o.right));
                    if (vOv * 10 >= std::min(Height(box), Height(o)) * 4 &&
                        hGap <= std::max<LONG>(24, Width(box))) {
                        adjacentVertical = true;
                        break;
                    }
                }
            }
            if (adjacentVertical) {
                columns[i] = true;
                lineHeights[i] =
                    measureVerticalMedianWidth(output[i].detectedLines, box, surfaceColors[i]);
            }
        }
    }

    // Resolve overlapping-fragment precedence (Conflicts) before building protectedRegions so small
    // suppressed fragments contained inside an active passage are cleanly erased instead of protected.
    for (size_t i = 0; i < output.size(); ++i) {
        if (output[i].suppressed || !Valid(output[i].sourceRect))
            continue;
        const auto& segment = segments[i];
        const bool coverOnly =
            (i >= translatedSegmentCount) && segment.translated.empty() && segment.original.empty();
        if (coverOnly || !segment.sourcePixels)
            continue;
        const RECT source = output[i].sourceRect;
        const long long own = static_cast<long long>(Width(source)) * Height(source);
        for (size_t j = 0; j < output.size(); ++j) {
            if (i == j || !Valid(output[j].sourceRect))
                continue;
            if (!Conflicts(source, output[j].sourceRect))
                continue;
            const long long other = static_cast<long long>(Width(output[j].sourceRect)) *
                                    Height(output[j].sourceRect);
            if (output[j].suppressed && other < own)
                continue;
            if (other >= own) {
                output[i].suppressed = true;
                suppressedRects[i] = source;
                break;
            }
        }
    }

    // Build protectedRegions: include all active segments plus only those suppressed regions that are
    // NOT geometrically contained (>= 70% area) inside an active non-coverOnly segment.
    std::vector<RECT> protectedRegions = result.protectedRegions;
    for (size_t i = 0; i < output.size(); ++i) {
        if (!output[i].suppressed && Valid(output[i].sourceRect))
            protectedRegions.push_back(output[i].sourceRect);
    }
    for (size_t i = 0; i < output.size(); ++i) {
        if (!output[i].suppressed || !Valid(suppressedRects[i]))
            continue;
        const RECT sr = suppressedRects[i];
        const long long srArea = static_cast<long long>(Width(sr)) * Height(sr);
        bool containedInActive = false;
        for (size_t j = 0; j < translatedSegmentCount; ++j) {
            if (output[j].suppressed || !Valid(output[j].sourceRect))
                continue;
            RECT inter{};
            if (IntersectRect(&inter, &sr, &output[j].sourceRect)) {
                const long long interArea = static_cast<long long>(Width(inter)) * Height(inter);
                if (srArea > 0 && interArea * 100 >= srArea * 70) {
                    containedInActive = true;
                    break;
                }
            }
        }
        if (!containedInActive)
            protectedRegions.push_back(sr);
    }

    auto colorDist = [](COLORREF a, COLORREF b) {
        return std::abs(int(GetRValue(a)) - int(GetRValue(b))) +
               std::abs(int(GetGValue(a)) - int(GetGValue(b))) +
               std::abs(int(GetBValue(a)) - int(GetBValue(b)));
    };
    // Harmonize detected glyph colours and outlines across peer segments on the same surface so
    // identical paragraphs, list titles, captions and outlined manga bubbles never drift.
    {
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
        // Propagate detected bimodal outline (e.g. manga black-fill + white-rim) to same-surface peers.
        for (size_t i = 0; i < translatedSegmentCount; ++i) {
            if (output[i].suppressed || !Valid(output[i].sourceRect) || output[i].hasStroke)
                continue;
            int strokePeers = 0, bestDist = INT_MAX;
            size_t bestPeer = 0;
            for (size_t j = 0; j < translatedSegmentCount; ++j) {
                if (i == j || output[j].suppressed || !output[j].hasStroke ||
                    !output[j].strokeColor.has_value() || columns[i] != columns[j])
                    continue;
                if (colorDist(resolvedBg[i], resolvedBg[j]) > 32)
                    continue;
                const float hi = std::max(1.f, lineHeights[i]), hj = std::max(1.f, lineHeights[j]);
                if (std::max(hi, hj) > std::min(hi, hj) * 1.45f)
                    continue;
                const int d = std::abs(int(output[i].sourceRect.left - output[j].sourceRect.left)) +
                              std::abs(int(output[i].sourceRect.top - output[j].sourceRect.top));
                ++strokePeers;
                if (d < bestDist) {
                    bestDist = d;
                    bestPeer = j;
                }
            }
            if (strokePeers >= 2 || (strokePeers == 1 && bestDist <= 180)) {
                output[i].hasStroke = true;
                output[i].strokeColor = output[bestPeer].strokeColor;
                if (textColors[bestPeer].has_value()) {
                    textColors[i] = textColors[bestPeer];
                    if (settings.matchTextColor)
                        output[i].sourceColor = textColors[bestPeer];
                }
            }
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
                if (output[i].hasStroke != output[j].hasStroke)
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

    const auto isHBorderRow = [&](COLORREF bgCol, LONG y, LONG x0, LONG x1) -> bool {
        if (y < 0 || y >= image.height || x1 - x0 < 10)
            return false;
        const int br = GetRValue(bgCol), bg = GetGValue(bgCol), bb = GetBValue(bgCol);
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
    const auto isVBorderCol = [&](COLORREF bgCol, LONG x, LONG y0, LONG y1) -> bool {
        if (x < 0 || x >= image.width || y1 - y0 < 6)
            return false;
        const int br = GetRValue(bgCol), bg = GetGValue(bgCol), bb = GetBValue(bgCol);
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

    struct SegmentPrep {
        bool active{};
        bool column{};
        bool hasCore{};
        bool isProse{};
        bool isShortLabel{};
        bool lowercaseOnly{};
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
    std::vector<Background> bgSamples(output.size());
    std::vector<bool> hasLeftPeerFlag(output.size(), false);
    std::vector<bool> shortCenterCandidate(output.size(), false);
    const int pad = std::max(1, int(std::ceil(2.0 * scale)));

    // Loop A: Determine surface color, centering alignment, and initial non-border-crossing area rects.
    for (size_t i = 0; i < output.size(); ++i) {
        auto& item = output[i];
        const auto& segment = segments[i];
        const RECT source = item.sourceRect;
        if (item.suppressed || !Valid(source) || segment.untranslated) {
            item.suppressed = true;
            continue;
        }
        const bool coverOnly =
            (i >= translatedSegmentCount) && segment.translated.empty() && segment.original.empty();
        auto bg = AnalyzeBackground(image, source);
        bgSamples[i] = bg;
        const float detectedHeight = lineHeights[i];
        const bool hasSurface = surfaceColors[i].has_value();
        item.background = hasSurface ? surfaceColors[i].value_or(bg.color) : bg.color;
        const int margin =
            detectedHeight > 0 ? pad + std::clamp(int(std::ceil(detectedHeight * .18f)), 2, 7) : pad;
        int symExpand = pad;
        if (!columns[i] && !coverOnly) {
            const std::wstring wTrans = Wide(segment.translated);
            const std::wstring wOrig = Wide(segment.original);
            const bool hasProsePunct = HasSentencePunctForCenter(wTrans) || HasSentencePunctForCenter(wOrig);
            const bool isShortLabel = !hasProsePunct && !wTrans.empty() && wTrans.size() <= 16 &&
                                      wOrig.size() <= 36;
            shortCenterCandidate[i] = isShortLabel && item.detectedLines.size() <= 1;
            const bool rtlSource = HasRtlScript(wOrig);
            bool hasLeftAlignedPeer = false;
            bool centerAlignedHeadingPair = false;
            const int leftTol = std::min(12, std::max(4, int(detectedHeight * .35f)));
            for (size_t j = 0; j < translatedSegmentCount; ++j) {
                if (i == j || output[j].suppressed || !Valid(output[j].sourceRect))
                    continue;
                const RECT other = output[j].sourceRect;
                const int vGap = other.top >= source.bottom
                                     ? int(other.top - source.bottom)
                                     : (source.top >= other.bottom ? int(source.top - other.bottom) : 0);
                const int edgeDiff = rtlSource ? std::abs(int(source.right - other.right))
                                               : std::abs(int(source.left - other.left));
                const int oppEdgeDiff = rtlSource ? std::abs(int(source.left - other.left))
                                                  : std::abs(int(source.right - other.right));
                const int centerDiff =
                    std::abs(int((source.left + source.right) - (other.left + other.right))) / 2;
                const int widthDiff = std::abs(int(Width(source) - Width(other)));
                const bool centeredStackPair = centerDiff < edgeDiff && widthDiff >= 12 &&
                                               std::abs(edgeDiff - oppEdgeDiff) <= std::max(8, centerDiff + 6);
                if (!centeredStackPair && edgeDiff <= leftTol &&
                    vGap <= std::max(120, int(detectedHeight * 6.f))) {
                    bool sameSurface = true;
                    if (surfaceColors[i].has_value() && surfaceColors[j].has_value()) {
                        sameSurface = colorDist(*surfaceColors[i], *surfaceColors[j]) <= 45;
                    }
                    if (sameSurface)
                        hasLeftAlignedPeer = true;
                }
                if (other.right <= source.left || other.left >= source.right)
                    continue;
                if (item.detectedLines.size() == 1 &&
                    vGap <= std::max(36, int(detectedHeight * 2.4f))) {
                    const int leftShift = std::abs(int(source.left - other.left));
                    const int rightShift = std::abs(int(source.right - other.right));
                    const int maxCenterTol = isShortLabel ? std::max(6, int(detectedHeight * .22f)) : 6;
                    if (centerDiff <= maxCenterTol &&
                        widthDiff >= std::max(16, int(detectedHeight * .65f)) &&
                        leftShift >= 8 && rightShift >= 8 &&
                        centerDiff * 2 < leftShift + rightShift &&
                        std::abs(leftShift - rightShift) <= std::max(8, centerDiff * 2 + 4)) {
                        centerAlignedHeadingPair = true;
                    }
                }
            }
            hasLeftPeerFlag[i] = hasLeftAlignedPeer;
            bool multiLineCentered = false;
            if (!hasLeftAlignedPeer && item.detectedLines.size() >= 2) {
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
                    const float leftGap = float(line.left - source.left);
                    const float rightGap = float(source.right - line.right);
                    if (leftGap >= std::max(5.f, detectedHeight * .40f) ||
                        rightGap >= std::max(5.f, detectedHeight * .40f)) {
                        if (leftGap >= std::max(5.f, detectedHeight * .40f) &&
                            rightGap >= std::max(5.f, detectedHeight * .40f) &&
                            std::abs(leftGap - rightGap) <= std::max(3.f, detectedHeight * .22f))
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
            if (isShortLabel && !hasLeftAlignedPeer && item.detectedLines.size() <= 1 && wTrans.size() <= 14) {
                const int y0 = std::clamp(int(source.top + Height(source) / 4), 0, image.height - 1);
                const int y1 = std::clamp(int(source.bottom - Height(source) / 4), y0 + 1, image.height);
                const int maxScan = std::max(28, int(Width(source) * 2));
                auto columnDiff = [&](int sx) -> int {
                    if (sx < 0 || sx >= image.width)
                        return 999;
                    for (size_t j = 0; j < translatedSegmentCount; ++j) {
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
                while (leftClear < maxScan && columnDiff(int(source.left) - 2 - leftClear) <= 42)
                    ++leftClear;
                const bool leftHitBorder =
                    leftClear < maxScan && columnDiff(int(source.left) - 2 - leftClear) >= 48;
                int rightClear = 0;
                while (rightClear < maxScan && columnDiff(int(source.right) + 1 + rightClear) <= 42)
                    ++rightClear;
                const bool rightHitBorder =
                    rightClear < maxScan && columnDiff(int(source.right) + 1 + rightClear) >= 48;
                if (leftHitBorder && rightHitBorder && leftClear >= 6 && rightClear >= 6 &&
                    std::abs(leftClear - rightClear) <=
                        std::max(4, int(.14f * float(leftClear + rightClear)))) {
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
        // Share only the whitespace between source regions, including shallow vertical overlaps.
        for (auto other : protectedRegions) {
            if (!Valid(other) || EqualRect(&other, &source))
                continue;
            if (Conflicts(source, other))
                continue;
            if (segment.sourcePixels && other.left < area.right && other.right > area.left) {
                if (other.bottom <= source.top) {
                    area.top = std::max(area.top, (source.top + other.bottom + 1) / 2);
                } else if (other.top < source.top && other.bottom < source.bottom) {
                    const LONG mid = (source.top + other.bottom + 1) / 2;
                    const LONG maxTop = source.bottom - std::max(1L, LONG(Height(source) * 6 / 10));
                    area.top = std::max(area.top, std::min(mid, maxTop));
                }
                if (other.top >= source.bottom) {
                    area.bottom = std::min(area.bottom, (source.bottom + other.top) / 2);
                } else if (other.bottom > source.bottom && other.top > source.top) {
                    const LONG mid = (source.bottom + other.top) / 2;
                    const LONG minBottom = source.top + std::max(1L, LONG(Height(source) * 6 / 10));
                    area.bottom = std::min(area.bottom, std::max(mid, minBottom));
                }
            }
            if (segment.sourcePixels && other.top < area.bottom && other.bottom > area.top) {
                if (other.right <= source.left + 8 && other.left < source.left)
                    area.left = std::max(area.left, std::min(source.right - 1, (source.left + other.right + 1) / 2));
                if (other.left >= source.right - 8 && other.right > source.right)
                    area.right = std::min(area.right, std::max(source.left + 1, (source.right + other.left) / 2));
            }
        }
        if (!coverOnly && (bg.simple || hasSurface)) {
            for (LONG y = source.top - 1; y >= area.top; --y)
                if (isHBorderRow(item.background, y, source.left, source.right)) {
                    area.top = std::min(source.top, y + 1);
                    break;
                }
            for (LONG y = source.bottom; y < area.bottom; ++y)
                if (isHBorderRow(item.background, y, source.left, source.right)) {
                    area.bottom = std::max(source.bottom, y);
                    break;
                }
            for (LONG x = source.left - 1; x >= area.left; --x)
                if (isVBorderCol(item.background, x, source.top, source.bottom)) {
                    area.left = std::min(source.left, x + 1);
                    break;
                }
            for (LONG x = source.right; x < area.right; ++x)
                if (isVBorderCol(item.background, x, source.top, source.bottom)) {
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
        item.rect = area;
    }

    // Harmonize centerAligned across same-row button/pill/tab peers (e.g. horizontal pill bars and
    // tab strips where most tabs center-detected cleanly and one had icons or active-tab contrast).
    {
        std::vector<bool> updatedCenter(output.size(), false);
        for (size_t i = 0; i < translatedSegmentCount; ++i)
            updatedCenter[i] = output[i].centerAligned;
        for (size_t i = 0; i < translatedSegmentCount; ++i) {
            if (output[i].suppressed || columns[i] || updatedCenter[i] ||
                !shortCenterCandidate[i] || hasLeftPeerFlag[i] || !Valid(output[i].sourceRect))
                continue;
            const RECT ri = output[i].sourceRect;
            int centeredRowPeers = 0;
            for (size_t j = 0; j < translatedSegmentCount; ++j) {
                if (i == j || output[j].suppressed || columns[j] || !output[j].centerAligned ||
                    !shortCenterCandidate[j] || !Valid(output[j].sourceRect))
                    continue;
                const RECT rj = output[j].sourceRect;
                const LONG vOv = std::min(ri.bottom, rj.bottom) - std::max(ri.top, rj.top);
                if (vOv * 10 < std::min(Height(ri), Height(rj)) * 6)
                    continue;
                const float hi = std::max(1.f, lineHeights[i]), hj = std::max(1.f, lineHeights[j]);
                if (std::max(hi, hj) > std::min(hi, hj) * 1.45f)
                    continue;
                ++centeredRowPeers;
            }
            if (centeredRowPeers >= 2)
                updatedCenter[i] = true;
        }
        for (size_t i = 0; i < translatedSegmentCount; ++i) {
            if (updatedCenter[i] && !output[i].centerAligned) {
                output[i].centerAligned = true;
                const RECT s = output[i].sourceRect;
                RECT& a = output[i].rect;
                const LONG symMargin = std::max(0L, std::min(s.left - a.left, a.right - s.right));
                a.left = s.left - symMargin;
                a.right = s.right + symMargin;
            }
        }
    }

    // Post-layout pairwise overlap-elimination pass over all active output[i].rect pairs so adjacent
    // boxes that had touching or slightly overlapping OCR source rects never collide on screen.
    for (int pass = 0; pass < 3; ++pass) {
        bool anyOverlap = false;
        for (size_t i = 0; i < translatedSegmentCount; ++i) {
            if (output[i].suppressed || !Valid(output[i].rect))
                continue;
            for (size_t j = i + 1; j < translatedSegmentCount; ++j) {
                if (output[j].suppressed || !Valid(output[j].rect))
                    continue;
                RECT inter{};
                if (!IntersectRect(&inter, &output[i].rect, &output[j].rect) || !Valid(inter))
                    continue;
                anyOverlap = true;
                RECT& ra = output[i].rect;
                RECT& rb = output[j].rect;
                const RECT& sa = output[i].sourceRect;
                const RECT& sb = output[j].sourceRect;
                const long long ox = Width(inter), oy = Height(inter);
                const long long minW = std::max<long long>(1, std::min(Width(ra), Width(rb)));
                const long long minH = std::max<long long>(1, std::min(Height(ra), Height(rb)));
                if (oy * minW <= ox * minH) {
                    const bool aFirst =
                        (sa.top + sa.bottom) <= (sb.top + sb.bottom);
                    RECT& topR = aFirst ? ra : rb;
                    RECT& botR = aFirst ? rb : ra;
                    const RECT& topS = aFirst ? sa : sb;
                    const RECT& botS = aFirst ? sb : sa;
                    LONG split = (topS.bottom <= botS.top)
                                     ? (topS.bottom + botS.top) / 2
                                     : (topR.bottom + botR.top) / 2;
                    split = std::clamp(split, topR.top + 1, std::max(topR.top + 1, botR.bottom - 1));
                    topR.bottom = std::min(topR.bottom, split);
                    botR.top = std::max(botR.top, split);
                    if (topR.bottom > botR.top)
                        botR.top = topR.bottom;
                } else {
                    const bool aFirst =
                        (sa.left + sa.right) <= (sb.left + sb.right);
                    RECT& leftR = aFirst ? ra : rb;
                    RECT& rightR = aFirst ? rb : ra;
                    const RECT& leftS = aFirst ? sa : sb;
                    const RECT& rightS = aFirst ? sb : sa;
                    LONG split = (leftS.right <= rightS.left)
                                     ? (leftS.right + rightS.left) / 2
                                     : (leftR.right + rightR.left) / 2;
                    split = std::clamp(split, leftR.left + 1, std::max(leftR.left + 1, rightR.right - 1));
                    leftR.right = std::min(leftR.right, split);
                    rightR.left = std::max(rightR.left, split);
                    if (leftR.right > rightR.left)
                        rightR.left = leftR.right;
                }
            }
        }
        if (!anyOverlap)
            break;
    }

    // Recompute padding and symmetric left/right overhang bounds against finalized neighbor rects and borders.
    for (size_t i = 0; i < output.size(); ++i) {
        auto& item = output[i];
        const auto& segment = segments[i];
        if (item.suppressed || !Valid(item.rect) || !Valid(item.sourceRect))
            continue;
        const RECT source = item.sourceRect;
        const RECT area = item.rect;
        const bool coverOnly =
            (i >= translatedSegmentCount) && segment.translated.empty() && segment.original.empty();
        float maxRightOverhang = std::min(kOverhangAllowance, float(std::max<LONG>(0, image.width - area.right)));
        float maxLeftOverhang = std::min(kOverhangAllowance, float(std::max<LONG>(0, area.left)));
        if (!coverOnly && segment.sourcePixels) {
            for (size_t j = 0; j < output.size(); ++j) {
                if (i == j || output[j].suppressed || !Valid(output[j].rect))
                    continue;
                const RECT other = output[j].rect;
                if (other.top < area.bottom && other.bottom > area.top) {
                    if (other.left >= area.right - 2 && other.right > area.right)
                        maxRightOverhang = std::min(maxRightOverhang, float(std::max(0L, other.left - area.right)));
                    if (other.right <= area.left + 2 && other.left < area.left)
                        maxLeftOverhang = std::min(maxLeftOverhang, float(std::max(0L, area.left - other.right)));
                }
            }
            for (const auto& other : protectedRegions) {
                if (!Valid(other) || EqualRect(&other, &source) || Conflicts(source, other))
                    continue;
                if (other.top < area.bottom && other.bottom > area.top) {
                    if (other.left >= source.right - 8 && other.right > source.right)
                        maxRightOverhang = std::min(maxRightOverhang, float(std::max(0L, other.left - area.right)));
                    if (other.right <= source.left + 8 && other.left < source.left)
                        maxLeftOverhang = std::min(maxLeftOverhang, float(std::max(0L, area.left - other.right)));
                }
            }
            const LONG vy0 = std::max<LONG>(0, source.top - 4);
            const LONG vy1 = std::min<LONG>(image.height, source.bottom + 4);
            const LONG scanRightEnd = std::min<LONG>(image.width, area.right + LONG(kOverhangAllowance));
            for (LONG x = area.right; x < scanRightEnd; ++x)
                if (isVBorderCol(item.background, x, vy0, vy1)) {
                    maxRightOverhang = std::min(maxRightOverhang, float(std::max(0L, x - area.right)));
                    break;
                }
            const LONG scanLeftStart = std::max<LONG>(0, area.left - LONG(kOverhangAllowance));
            for (LONG x = area.left - 1; x >= scanLeftStart; --x)
                if (isVBorderCol(item.background, x, vy0, vy1)) {
                    maxLeftOverhang = std::min(maxLeftOverhang, float(std::max(0L, area.left - (x + 1))));
                    break;
                }
        }
        item.maxOverhang = maxRightOverhang;
        item.maxLeftOverhang = maxLeftOverhang;
        const int minDim = std::min(Width(area), Height(area));
        const int effPad = (minDim <= 16 || (item.detectedLines.size() <= 1 && Height(area) <= 20)) ? 1 : pad;
        item.padding = float(std::max(0, std::min(effPad, (minDim - 1) / 2)));
    }

    // Loop B: Rebuild pixel covers using finalized non-overlapping areas, then prepare typography entries.
    for (size_t i = 0; i < output.size(); ++i) {
        auto& item = output[i];
        const auto& segment = segments[i];
        const RECT source = item.sourceRect;
        const RECT area = item.rect;
        if (item.suppressed || !Valid(source) || !Valid(area))
            continue;
        const bool coverOnly =
            (i >= translatedSegmentCount) && segment.translated.empty() && segment.original.empty();
        const auto& bg = bgSamples[i];
        const float detectedHeight = lineHeights[i];
        {
            // Repair the whole translated box at pixel level: every pixel keeps the captured artwork, and
            // only the glyph strokes are replaced with the local backdrop. Nothing is pasted on top, so the
            // replacement reads as native scenery and no border, veil or blurred band ever appears.
            const int grow = std::max(4, int(std::ceil((detectedHeight > 0 ? detectedHeight : 12.f) * .12f)));
            const auto buildCover = [&](const RECT& region, COLORREF surfaceRef) {
                std::vector<unsigned char> cover;
                const int coverWidth = int(region.right - region.left);
                const int coverHeight = int(region.bottom - region.top);
                if (coverWidth <= 0 || coverHeight <= 0 || !Valid(region))
                    return cover;
                // Estimate each 24x24 block's backdrop from locally smooth non-glyph pixels without
                // snapping vignetted/gradient blocks to a single global surfaceRef; blocks filled by
                // strokes propagate from valid neighbor blocks before ever falling back to surfaceRef.
                constexpr int blockSize = 24;
                const int blocksX = (coverWidth + blockSize - 1) / blockSize;
                const int blocksY = (coverHeight + blockSize - 1) / blockSize;
                const size_t totalBlocks = size_t(blocksX) * size_t(blocksY);
                std::vector<int> blockRed(totalBlocks, GetRValue(surfaceRef));
                std::vector<int> blockGreen(totalBlocks, GetGValue(surfaceRef));
                std::vector<int> blockBlue(totalBlocks, GetBValue(surfaceRef));
                std::vector<int> blockSpread(totalBlocks, 4);
                std::vector<unsigned char> blockValid(totalBlocks, 0);
                const COLORREF glyphColor = textColors[i].value_or(RGB(0, 0, 0));
                const bool hasGlyphColor = textColors[i].has_value();
                const COLORREF strokeCol = item.strokeColor.value_or(RGB(255, 255, 255));
                const bool hasStrokeCol = item.hasStroke && item.strokeColor.has_value();
                {
                    std::vector<int> histogram(256);
                    std::vector<int> localSmoothR, localSmoothG, localSmoothB;
                    std::vector<int> closeSmoothR, closeSmoothG, closeSmoothB;
                    std::vector<int> closeR, closeG, closeB;
                    std::vector<int> mildR, mildG, mildB;
                    const auto diffL1 = [](const unsigned char* a, const unsigned char* b) {
                        return std::abs(int(a[2]) - int(b[2])) +
                               std::abs(int(a[1]) - int(b[1])) +
                               std::abs(int(a[0]) - int(b[0]));
                    };
                    const auto medianOf = [&histogram](const std::vector<int>& values) -> int {
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
                    for (int by = 0; by < blocksY; ++by)
                        for (int bx = 0; bx < blocksX; ++bx) {
                            localSmoothR.clear();
                            localSmoothG.clear();
                            localSmoothB.clear();
                            closeSmoothR.clear();
                            closeSmoothG.clear();
                            closeSmoothB.clear();
                            closeR.clear();
                            closeG.clear();
                            closeB.clear();
                            mildR.clear();
                            mildG.clear();
                            mildB.clear();
                            const int x0 = bx * blockSize, x1 = std::min(coverWidth, x0 + blockSize);
                            const int y0 = by * blockSize, y1 = std::min(coverHeight, y0 + blockSize);
                            for (int y = y0; y < y1; ++y)
                                for (int x = x0; x < x1; ++x) {
                                    const int sx = std::clamp(int(region.left) + x, 0, image.width - 1);
                                    const int sy = std::clamp(int(region.top) + y, 0, image.height - 1);
                                    const auto* p = &image.bgra[(size_t(sy) * image.width + sx) * 4];
                                    const int distSurface = std::abs(int(p[2]) - GetRValue(surfaceRef)) +
                                                            std::abs(int(p[1]) - GetGValue(surfaceRef)) +
                                                            std::abs(int(p[0]) - GetBValue(surfaceRef));
                                    const int distGlyph =
                                        hasGlyphColor ? (std::abs(int(p[2]) - GetRValue(glyphColor)) +
                                                         std::abs(int(p[1]) - GetGValue(glyphColor)) +
                                                         std::abs(int(p[0]) - GetBValue(glyphColor)))
                                                      : 999;
                                    const int distStroke =
                                        hasStrokeCol ? (std::abs(int(p[2]) - GetRValue(strokeCol)) +
                                                        std::abs(int(p[1]) - GetGValue(strokeCol)) +
                                                        std::abs(int(p[0]) - GetBValue(strokeCol)))
                                                     : 999;
                                    const bool notInk = distGlyph >= 48 && distStroke >= 48;
                                    if (distSurface < 96 && notInk) {
                                        closeR.push_back(p[2]);
                                        closeG.push_back(p[1]);
                                        closeB.push_back(p[0]);
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
                                    const int grad =
                                        std::max({diffL1(p, pl), diffL1(p, pr), diffL1(p, pu), diffL1(p, pd)});
                                    if (grad <= 24 && notInk) {
                                        localSmoothR.push_back(p[2]);
                                        localSmoothG.push_back(p[1]);
                                        localSmoothB.push_back(p[0]);
                                        if (distSurface < 110) {
                                            closeSmoothR.push_back(p[2]);
                                            closeSmoothG.push_back(p[1]);
                                            closeSmoothB.push_back(p[0]);
                                        }
                                    } else if (grad <= 48 && distGlyph >= 54 && distStroke >= 48) {
                                        mildR.push_back(p[2]);
                                        mildG.push_back(p[1]);
                                        mildB.push_back(p[0]);
                                    }
                                }
                            const std::vector<int>* chosenR = nullptr;
                            const std::vector<int>* chosenG = nullptr;
                            const std::vector<int>* chosenB = nullptr;
                            if (localSmoothR.size() >= 6) {
                                if (closeSmoothR.size() >= 6 &&
                                    closeSmoothR.size() * 100 >= localSmoothR.size() * 45) {
                                    chosenR = &closeSmoothR;
                                    chosenG = &closeSmoothG;
                                    chosenB = &closeSmoothB;
                                } else {
                                    chosenR = &localSmoothR;
                                    chosenG = &localSmoothG;
                                    chosenB = &localSmoothB;
                                }
                            } else if (closeR.size() >= 6) {
                                chosenR = &closeR;
                                chosenG = &closeG;
                                chosenB = &closeB;
                            } else if (mildR.size() >= 8) {
                                chosenR = &mildR;
                                chosenG = &mildG;
                                chosenB = &mildB;
                            }
                            const size_t block = size_t(by) * size_t(blocksX) + size_t(bx);
                            if (chosenR) {
                                const int medR = medianOf(*chosenR);
                                const int medG = medianOf(*chosenG);
                                const int medB = medianOf(*chosenB);
                                blockRed[block] = medR;
                                blockGreen[block] = medG;
                                blockBlue[block] = medB;
                                blockValid[block] = 1;
                                long long spreadSum = 0;
                                for (size_t k = 0; k < chosenR->size(); ++k)
                                    spreadSum += std::abs((*chosenR)[k] - medR) +
                                                 std::abs((*chosenG)[k] - medG) +
                                                 std::abs((*chosenB)[k] - medB);
                                blockSpread[block] = int(
                                    std::clamp<long long>(spreadSum / long(chosenR->size()), 1, 24));
                            }
                        }
                    // Iterative neighbor block propagation for blocks heavily covered by glyph strokes.
                    for (int iter = 0; iter < 16; ++iter) {
                        bool changed = false;
                        std::vector<unsigned char> nextValid = blockValid;
                        std::vector<int> nextR = blockRed, nextG = blockGreen, nextB = blockBlue,
                                         nextS = blockSpread;
                        for (int by = 0; by < blocksY; ++by)
                            for (int bx = 0; bx < blocksX; ++bx) {
                                const size_t idx = size_t(by) * size_t(blocksX) + size_t(bx);
                                if (blockValid[idx])
                                    continue;
                                int sr = 0, sg = 0, sb = 0, ss = 0, cnt = 0;
                                const int dx4[4] = {-1, 1, 0, 0}, dy4[4] = {0, 0, -1, 1};
                                for (int d = 0; d < 4; ++d) {
                                    const int nx = bx + dx4[d], ny = by + dy4[d];
                                    if (nx < 0 || nx >= blocksX || ny < 0 || ny >= blocksY)
                                        continue;
                                    const size_t nIdx = size_t(ny) * size_t(blocksX) + size_t(nx);
                                    if (blockValid[nIdx]) {
                                        sr += blockRed[nIdx];
                                        sg += blockGreen[nIdx];
                                        sb += blockBlue[nIdx];
                                        ss += blockSpread[nIdx];
                                        ++cnt;
                                    }
                                }
                                if (cnt == 0) {
                                    const int dxD[4] = {-1, 1, -1, 1}, dyD[4] = {-1, -1, 1, 1};
                                    for (int d = 0; d < 4; ++d) {
                                        const int nx = bx + dxD[d], ny = by + dyD[d];
                                        if (nx < 0 || nx >= blocksX || ny < 0 || ny >= blocksY)
                                            continue;
                                        const size_t nIdx = size_t(ny) * size_t(blocksX) + size_t(nx);
                                        if (blockValid[nIdx]) {
                                            sr += blockRed[nIdx];
                                            sg += blockGreen[nIdx];
                                            sb += blockBlue[nIdx];
                                            ss += blockSpread[nIdx];
                                            ++cnt;
                                        }
                                    }
                                }
                                if (cnt > 0) {
                                    nextR[idx] = sr / cnt;
                                    nextG[idx] = sg / cnt;
                                    nextB[idx] = sb / cnt;
                                    nextS[idx] = std::clamp(ss / cnt, 4, 24);
                                    nextValid[idx] = 1;
                                    changed = true;
                                }
                            }
                        blockRed.swap(nextR);
                        blockGreen.swap(nextG);
                        blockBlue.swap(nextB);
                        blockSpread.swap(nextS);
                        blockValid.swap(nextValid);
                        if (!changed)
                            break;
                    }
                }
                std::vector<unsigned char> glyphMask(size_t(coverWidth) * size_t(coverHeight), 0);
                std::vector<unsigned char> rawInk(size_t(coverWidth) * size_t(coverHeight), 0);
                std::vector<unsigned char> structMask(size_t(coverWidth) * size_t(coverHeight), 0);
                // Restrict erasure to detected line bands when available so empty corners of staggered
                // multi-column bubbles or chart callouts never smear underlying artwork.
                const int bandPx = detectedHeight > 0 ? int(std::ceil(detectedHeight)) : 12;
                const int vLinePad = (bg.simple || surfaceColors[i].has_value())
                                         ? std::clamp(bandPx / 6, 2, 4)
                                         : 1;
                const int hLinePad = columns[i] ? 2 : std::min(grow, std::max(2, bandPx / 6));
                const auto insideDetectedLine = [&](int sx, int sy) {
                    if (item.detectedLines.empty())
                        return sx >= source.left - grow && sx <= source.right + grow &&
                               sy >= source.top - grow && sy <= source.bottom + grow;
                    return std::any_of(item.detectedLines.begin(), item.detectedLines.end(),
                                       [&](RECT line) {
                                           return sx >= line.left - hLinePad && sx <= line.right + hLinePad &&
                                                  sy >= line.top - vLinePad && sy <= line.bottom + vLinePad;
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
                        const bool closerToStroke =
                            hasStrokeCol && departure > 14 &&
                            (std::abs(int(p[2]) - GetRValue(strokeCol)) +
                             std::abs(int(p[1]) - GetGValue(strokeCol)) +
                             std::abs(int(p[0]) - GetBValue(strokeCol))) < departure;
                        const unsigned char active =
                            (departure > threshold || closerToGlyph || closerToStroke) ? 1 : 0;
                        rawInk[size_t(y) * coverWidth + x] = active;
                        if (active && insideDetectedLine(sx, sy))
                            glyphMask[size_t(y) * coverWidth + x] = 1;
                    }
                }
                // Protect structural horizontal/vertical borders, table grid lines, dashed chart gridlines
                // and form rules without falsely protecting kanji strokes, dotted focus boxes or underlines.
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
                        int thinCols = 0, solidCols = 0;
                        for (int k = x0; k < x1; ++k) {
                            if (rawInk[size_t(y) * coverWidth + k])
                                ++solidCols;
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
                        // Reject 1px-on/1px-off dotted focus rectangles and dotted link underlines.
                        const bool isSolid = solidCols * 10 >= runLen * 8;
                        if (!isSolid)
                            continue;
                        const bool continuesLeft =
                            x0 <= 1 && region.left >= 3 &&
                            (outsideImgInk(int(region.left) - 2, sy) ||
                             outsideImgInk(int(region.left) - 3, sy));
                        const bool continuesRight =
                            x1 >= coverWidth - 1 && region.right + 2 < image.width &&
                            (outsideImgInk(int(region.right) + 1, sy) ||
                             outsideImgInk(int(region.right) + 2, sy));
                        const bool touchesImageEdge =
                            (x0 <= 1 && region.left < 3) ||
                            (x1 >= coverWidth - 1 && region.right + 2 >= image.width);
                        const bool fullSpan =
                            !columns[i] && runLen >= std::max(28, int(coverWidth * .60f)) &&
                            (outsideSourceY || continuesLeft || continuesRight || touchesImageEdge ||
                             (coverWidth >= 120 && runLen >= int(coverWidth * .85f) &&
                              (y <= 4 || y >= coverHeight - 5)));
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
                    // Periodic horizontal dashed-line detector (e.g. chart dashed gridlines with 2-9 px
                    // dashes and 2-9 px gaps that continue past the cover bounds).
                    if (!columns[i] && coverWidth >= 48) {
                        struct Dash {
                            int x0, x1;
                        };
                        std::vector<Dash> dashes;
                        int dx = 0;
                        while (dx < coverWidth) {
                            if (!rawInk[size_t(y) * coverWidth + dx]) {
                                ++dx;
                                continue;
                            }
                            const int ds = dx;
                            while (dx < coverWidth && rawInk[size_t(y) * coverWidth + dx])
                                ++dx;
                            const int len = dx - ds;
                            if (len >= 2 && len <= 10) {
                                int thin = 0;
                                for (int k = ds; k < dx; ++k) {
                                    int up = y, dn = y;
                                    while (up > 0 && y - up < 4 && rawInk[size_t(up - 1) * coverWidth + k])
                                        --up;
                                    while (dn + 1 < coverHeight && dn - y < 4 &&
                                           rawInk[size_t(dn + 1) * coverWidth + k])
                                        ++dn;
                                    if (dn - up + 1 <= 3)
                                        ++thin;
                                }
                                if (thin * 4 >= len * 3)
                                    dashes.push_back({ds, dx});
                            }
                        }
                        if (dashes.size() >= 4) {
                            std::vector<int> dLens, dGaps;
                            for (size_t k = 0; k < dashes.size(); ++k) {
                                dLens.push_back(dashes[k].x1 - dashes[k].x0);
                                if (k > 0) {
                                    const int g = dashes[k].x0 - dashes[k - 1].x1;
                                    if (g >= 2 && g <= 10)
                                        dGaps.push_back(g);
                                }
                            }
                            if (dGaps.size() >= 3) {
                                std::sort(dLens.begin(), dLens.end());
                                std::sort(dGaps.begin(), dGaps.end());
                                const int medD = dLens[dLens.size() / 2];
                                const int medG = dGaps[dGaps.size() / 2];
                                int regularD = 0, regularG = 0;
                                for (int dl : dLens)
                                    if (std::abs(dl - medD) <= 2)
                                        ++regularD;
                                for (int dg : dGaps)
                                    if (std::abs(dg - medG) <= 2)
                                        ++regularG;
                                auto hasExteriorDashes = [&](int startX, int dir) -> bool {
                                    int extDashes = 0, run = 0;
                                    for (int step = 1; step <= 32; ++step) {
                                        const int ex = startX + dir * step;
                                        if (ex < 0 || ex >= image.width)
                                            break;
                                        const bool ink = outsideImgInk(ex, sy);
                                        if (ink) {
                                            ++run;
                                        } else {
                                            if (run >= 2 && run <= 10)
                                                ++extDashes;
                                            run = 0;
                                        }
                                    }
                                    if (run >= 2 && run <= 10)
                                        ++extDashes;
                                    return extDashes >= 2;
                                };
                                const bool extLeft = hasExteriorDashes(int(region.left), -1);
                                const bool extRight = hasExteriorDashes(int(region.right) - 1, 1);
                                if (regularD * 10 >= int(dLens.size()) * 6 &&
                                    regularG * 10 >= int(dGaps.size()) * 6 &&
                                    (extLeft || extRight)) {
                                    for (const auto& d : dashes) {
                                        for (int k = d.x0; k < d.x1; ++k) {
                                            int up = y, dn = y;
                                            while (up > 0 && y - up < 3 &&
                                                   rawInk[size_t(up - 1) * coverWidth + k])
                                                --up;
                                            while (dn + 1 < coverHeight && dn - y < 3 &&
                                                   rawInk[size_t(dn + 1) * coverWidth + k])
                                                ++dn;
                                            if (dn - up + 1 <= 3) {
                                                for (int yy = up; yy <= dn; ++yy)
                                                    structMask[size_t(yy) * coverWidth + k] = 1;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                for (int x = 0; x < coverWidth; ++x) {
                    const int sx = int(region.left) + x;
                    const bool outsideSourceX = sx < source.left || sx >= source.right;
                    const int maxThick = outsideSourceX ? 6 : 4;
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
                            while (l > 0 && x - l < maxThick + 1 && rawInk[size_t(k) * coverWidth + (l - 1)])
                                --l;
                            while (r + 1 < coverWidth && r - x < maxThick + 1 &&
                                   rawInk[size_t(k) * coverWidth + (r + 1)])
                                ++r;
                            if (r - l + 1 <= maxThick) {
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
                        const bool continuousDivider =
                            (continuesTop || continuesBottom) && runLen >= std::max(14, int(coverHeight * .65f));
                        const bool edgeFrameBorder =
                            outsideSourceX && runLen >= std::max(14, coverHeight - 2) &&
                            clearRows * 10 >= runLen * 8;
                        if (continuousDivider || edgeFrameBorder) {
                            for (int k = y0; k < y1; ++k) {
                                int l = x, r = x;
                                while (l > 0 && x - l < maxThick && rawInk[size_t(k) * coverWidth + (l - 1)])
                                    --l;
                                while (r + 1 < coverWidth && r - x < maxThick &&
                                       rawInk[size_t(k) * coverWidth + (r + 1)])
                                    ++r;
                                if (r - l + 1 <= maxThick) {
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
                const bool allowRowFull = bg.simple || surfaceColors[i].has_value();
                for (int y = 0; y < coverHeight; ++y) {
                    int masked = 0;
                    for (int x = 0; x < coverWidth; ++x)
                        masked += grown[size_t(y) * coverWidth + x] ? 1 : 0;
                    rowFull[size_t(y)] = (allowRowFull && masked == coverWidth) ? 1 : 0;
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
                // Smooth the repaired strokes inside the mask to remove scanline banding.
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
                }
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
                const auto sampleBlockRgb = [&](int x, int y, float outRgb[3]) {
                    const float gy = (float(y) + .5f) / float(blockSize) - .5f;
                    const int y0 = int(std::floor(gy));
                    const float ty = gy - float(y0);
                    const float gx = (float(x) + .5f) / float(blockSize) - .5f;
                    const int x0 = int(std::floor(gx));
                    const float tx = gx - float(x0);
                    for (int ch = 0; ch < 3; ++ch) {
                        const float top =
                            blockValue(ch, x0, y0) * (1 - tx) + blockValue(ch, x0 + 1, y0) * tx;
                        const float bottom =
                            blockValue(ch, x0, y0 + 1) * (1 - tx) + blockValue(ch, x0 + 1, y0 + 1) * tx;
                        outRgb[ch] = top * (1 - ty) + bottom * ty;
                    }
                };
                // For rows completely spanned by glyph mask, interpolate vertically between the nearest
                // non-full in-region scanlines (falling back to local block backdrop instead of foreign corners).
                for (int y = 0; y < coverHeight; ++y) {
                    if (!rowFull[size_t(y)])
                        continue;
                    for (int x = 0; x < coverWidth; ++x) {
                        if (structMask[size_t(y) * coverWidth + x])
                            continue;
                        int yUp = y - 1;
                        while (yUp >= 0 && (rowFull[size_t(yUp)] || structMask[size_t(yUp) * coverWidth + x]))
                            --yUp;
                        int yDown = y + 1;
                        while (yDown < coverHeight &&
                               (rowFull[size_t(yDown)] || structMask[size_t(yDown) * coverWidth + x]))
                            ++yDown;
                        auto* p = &cover[(size_t(y) * coverWidth + x) * 4];
                        if (yUp >= 0 && yDown < coverHeight) {
                            const auto* upP = &cover[(size_t(yUp) * coverWidth + x) * 4];
                            const auto* dnP = &cover[(size_t(yDown) * coverWidth + x) * 4];
                            const float t = float(y - yUp) / float(yDown - yUp);
                            for (int c = 0; c < 3; ++c)
                                p[c] = static_cast<unsigned char>(
                                    std::clamp(int(float(upP[c]) + (float(dnP[c]) - float(upP[c])) * t + .5f),
                                               0, 255));
                        } else {
                            float blk[3]{};
                            sampleBlockRgb(x, y, blk);
                            const unsigned char* singleP =
                                yUp >= 0 ? &cover[(size_t(yUp) * coverWidth + x) * 4]
                                         : (yDown < coverHeight ? &cover[(size_t(yDown) * coverWidth + x) * 4]
                                                                : nullptr);
                            for (int ch = 0; ch < 3; ++ch) {
                                const float val = singleP ? (float(singleP[2 - ch]) * .65f + blk[ch] * .35f)
                                                          : blk[ch];
                                p[2 - ch] = static_cast<unsigned char>(std::clamp(int(val + .5f), 0, 255));
                            }
                        }
                        p[3] = 255;
                    }
                }
                // Rebuilt strokes take the flattened local block tone so scanline seams disappear. Every
                // untouched pixel keeps the original capture byte for byte.
                for (int y = 0; y < coverHeight; ++y) {
                    for (int x = 0; x < coverWidth; ++x) {
                        const size_t idx = size_t(y) * coverWidth + x;
                        const size_t blockIdx =
                            size_t(std::clamp(y / blockSize, 0, blocksY - 1)) * size_t(blocksX) +
                            size_t(std::clamp(x / blockSize, 0, blocksX - 1));
                        const bool smoothSurface = (bg.simple || surfaceColors[i].has_value()) &&
                                                   blockValid[blockIdx] && blockSpread[blockIdx] <= 8;
                        const bool complexArt = !bg.simple && !surfaceColors[i].has_value() &&
                                                (!blockValid[blockIdx] || blockSpread[blockIdx] > 12);
                        const float weight = structMask[idx] ? 0.f
                                             : grown[idx]    ? (smoothSurface ? .86f : (complexArt ? .18f : .48f))
                                             : haloMask[idx] ? (smoothSurface ? .45f : (complexArt ? .08f : .18f))
                                                             : 0.f;
                        if (weight <= 0.f)
                            continue;
                        auto* p = &cover[idx * 4];
                        float target[3]{};
                        sampleBlockRgb(x, y, target);
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
                // Clip padded cover expansion against color-step borders (tab bottoms, pill frames,
                // button borders) so rebuilt surfaces never bleed out of a colored UI container.
                if (!coverOnly && (bg.simple || surfaceColors[i].has_value())) {
                    for (LONG y = source.top - 1; y >= padded.top; --y)
                        if (isHBorderRow(item.background, y, source.left, source.right)) {
                            padded.top = std::min(source.top, y + 1);
                            break;
                        }
                    for (LONG y = source.bottom; y < padded.bottom; ++y)
                        if (isHBorderRow(item.background, y, source.left, source.right)) {
                            padded.bottom = std::max(source.bottom, y);
                            break;
                        }
                    for (LONG x = source.left - 1; x >= padded.left; --x)
                        if (isVBorderCol(item.background, x, source.top, source.bottom)) {
                            padded.left = std::min(source.left, x + 1);
                            break;
                        }
                    for (LONG x = source.right; x < padded.right; ++x)
                        if (isVBorderCol(item.background, x, source.top, source.bottom)) {
                            padded.right = std::max(source.right, x);
                            break;
                        }
                }
                EraseRow block{padded, item.background, item.background, item.background, item.background, false, padded};
                block.cover = buildCover(padded, item.background);
                if (!block.cover.empty()) {
                    // Measure actual rebuilt surface median and spread inside source so DrawPositioned
                    // and contrast checks read the true rebuilt cover instead of a stale estimate.
                    const int cw = int(padded.right - padded.left);
                    const int ch = int(padded.bottom - padded.top);
                    const int sx0 = std::clamp(int(source.left - padded.left), 0, cw);
                    const int sx1 = std::clamp(int(source.right - padded.left), sx0, cw);
                    const int sy0 = std::clamp(int(source.top - padded.top), 0, ch);
                    const int sy1 = std::clamp(int(source.bottom - padded.top), sy0, ch);
                    std::array<std::array<size_t, 256>, 3> chHist{};
                    size_t sampleCnt = 0;
                    const int st = std::max(1, int(std::sqrt(double(std::max(1, (sx1 - sx0) * (sy1 - sy0))) / 2000.)));
                    for (int y = sy0; y < sy1; y += st)
                        for (int x = sx0; x < sx1; x += st) {
                            const auto* q = &block.cover[(size_t(y) * size_t(cw) + size_t(x)) * 4];
                            ++chHist[0][q[2]];
                            ++chHist[1][q[1]];
                            ++chHist[2][q[0]];
                            ++sampleCnt;
                        }
                    if (sampleCnt > 0) {
                        int med[3]{};
                        for (int c = 0; c < 3; ++c) {
                            size_t acc = 0;
                            for (int v = 0; v < 256; ++v) {
                                acc += chHist[c][v];
                                if (acc >= (sampleCnt + 1) / 2) {
                                    med[c] = v;
                                    break;
                                }
                            }
                        }
                        long long madSum = 0;
                        for (int y = sy0; y < sy1; y += st)
                            for (int x = sx0; x < sx1; x += st) {
                                const auto* q = &block.cover[(size_t(y) * size_t(cw) + size_t(x)) * 4];
                                madSum += std::abs(int(q[2]) - med[0]) + std::abs(int(q[1]) - med[1]) +
                                          std::abs(int(q[0]) - med[2]);
                            }
                        const float coverMad = float(double(madSum) / double(sampleCnt));
                        const COLORREF rebuiltMed = RGB(med[0], med[1], med[2]);
                        block.start = rebuiltMed;
                        block.end = rebuiltMed;
                        block.top = rebuiltMed;
                        block.bottom = rebuiltMed;
                        item.uniformBackdrop = (coverMad <= 18.f) || (bg.simple && coverMad <= 26.f);
                    }
                    item.eraseLines.push_back(std::move(block));
                }
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
        prep.height = (item.detectedLines.size() <= 1 && Height(area) <= 20)
                          ? std::max(1.f, float(std::max(Height(area), Height(source))))
                          : std::max(1.f, float(Height(area)) - 2 * item.padding);
        output[i].bandHeight = detectedHeight;
        output[i].coreHeight = coreHeights[i];
        output[i].eraseHeight = float(Height(source));
        if (columns[i]) {
            auto text = Wide(segment.translated);
            auto isAsciiDigitW = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
            for (size_t at = 0; at < text.size();) {
                const wchar_t ch = text[at];
                if (ch == L'\r' || ch == L'\n' || ch == L' ' || ch == L'\t') {
                    ++at;
                    continue;
                }
                // Collapse ASCII "..." into a single vertical ellipsis ︙.
                if (ch == L'.' && at + 2 < text.size() && text[at + 1] == L'.' && text[at + 2] == L'.') {
                    prep.glyphs.push_back(std::wstring(1, L'\xFE19'));
                    at += 3;
                    continue;
                }
                // Keep standalone 2-digit numbers (tate-chu-yoko, e.g. "10") intact on one vertical row.
                if (isAsciiDigitW(ch)) {
                    size_t endDig = at;
                    while (endDig < text.size() && isAsciiDigitW(text[endDig]))
                        ++endDig;
                    if (endDig - at == 2) {
                        prep.glyphs.push_back(text.substr(at, 2));
                        at = endDig;
                        continue;
                    }
                }
                size_t length = 1;
                if (ch >= 0xD800 && ch <= 0xDBFF && at + 1 < text.size())
                    length = 2;
                if (length == 1)
                    prep.glyphs.push_back(std::wstring(1, VerticalPunctuationForm(ch)));
                else
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
        const std::wstring wOrig = Wide(segment.original);
        const bool hasProsePunct = HasSentencePunctForCenter(prep.text) || HasSentencePunctForCenter(wOrig);
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
                const bool lowercaseOnly = !traits.cjk && !traits.complexScript && traits.lower &&
                                           !traits.upper && !traits.ascender && !traits.descender &&
                                           !traits.digit;
                prep.lowercaseOnly = lowercaseOnly;
                const float core = coreHeights[i];
                float emSize = 0.f;
                if (traits.cjk) {
                    const float bandSize =
                        pixelRefined[i] ? detectedHeight : InkBandCjk(detectedHeight);
                    const bool validCjkCore = core >= detectedHeight * .55f && core <= detectedHeight * .92f;
                    prep.hasCore = validCjkCore;
                    if (validCjkCore) {
                        const float coreEm = core / .82f;
                        // When an attached top/bottom badge or notification dot inflates bandSize far
                        // beyond coreEm, trust coreEm so peer tabs stay uniform.
                        emSize = coreEm < bandSize * .86f ? (core / .80f) : (bandSize + coreEm) * .5f;
                    } else {
                        emSize = bandSize;
                    }
                } else {
                    const float bandSize = InkBand(detectedHeight) / GlyphBandRatio(traits);
                    const bool validCore = core >= 4.f && core < detectedHeight * .85f;
                    prep.hasCore = validCore;
                    if (validCore) {
                        const float blended = (bandSize + core / GlyphCoreRatio(traits)) * .5f;
                        // Guard lowercase-only runs against core deflation from curved glyph edges.
                        emSize = lowercaseOnly ? std::max(bandSize * .88f, blended) : blended;
                    } else {
                        emSize = bandSize;
                    }
                }
                preferred = std::clamp(emSize, 9.f, 144.f);
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

    auto colorDistLocal = colorDist;
    auto rectGap = [](RECT a, RECT b) {
        const LONG dx = std::max(0L, std::max(a.left, b.left) - std::min(a.right, b.right));
        const LONG dy = std::max(0L, std::max(a.top, b.top) - std::min(a.bottom, b.bottom));
        return std::max(dx, dy);
    };

    // Pass A: Harmonize preferred font sizes across peer segments on the same surface and color tier
    // so body paragraphs, list titles, subtitles and dialogue bubbles stay consistent within their own tier.
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
                const RECT rj = output[j].sourceRect;
                const float pj = preps[j].preferred;
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
                const bool bothSingleLine =
                    output[i].detectedLines.size() <= 1 && output[j].detectedLines.size() <= 1;
                const LONG xOv = std::min(ri.right, rj.right) - std::max(ri.left, rj.left);
                const LONG minW = std::min(Width(ri), Width(rj));
                const bool sameRowPeer =
                    !preps[i].column && bothSingleLine &&
                    std::abs(ri.top - rj.top) <= std::max<LONG>(8, LONG(pi * .5f)) &&
                    std::abs(Height(ri) - Height(rj)) <= 6 && hGap >= 0 && hGap <= 420;
                const bool stackedShortCol =
                    !preps[i].column && bothSingleLine && aligned && vGap <= 80 &&
                    minW > 0 && xOv * 2 >= minW;
                const int maxBgDist = stackedShortCol ? 75 : 45;
                if (colorDistLocal(output[i].background, output[j].background) > maxBgDist)
                    continue;
                // Gate peer grouping on foreground color compatibility so muted secondary subtitles
                // never merge upward into darker primary title tiers.
                if (textColors[i].has_value() && textColors[j].has_value() &&
                    colorDistLocal(*textColors[i], *textColors[j]) > 55)
                    continue;
                const float maxRatio =
                    stackedShortCol ? 1.44f
                    : sameRowPeer   ? 1.35f
                    : (preps[i].lowercaseOnly && preps[i].isShortLabel && !preps[j].lowercaseOnly) ? 1.38f
                                                                                                   : 1.22f;
                if (std::max(pi, pj) > std::min(pi, pj) * maxRatio)
                    continue;
                bool related = (i == j) || sameRowPeer || stackedShortCol;
                if (!related && preps[i].column) {
                    related = rectGap(ri, rj) <= std::max<LONG>(220, LONG(pi * 12.f)) ||
                              std::max(pi, pj) <= std::min(pi, pj) * 1.18f;
                } else if (!related) {
                    if (aligned && vGap <= std::max<LONG>(240, LONG(pi * 14.f)))
                        related = true;
                    else if (preps[i].lowercaseOnly && preps[i].isShortLabel &&
                             vGap <= std::max<LONG>(480, LONG(pi * 24.f)))
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
                    if (output[j].detectedLines.size() >= 2 || (preps[j].hasCore && !preps[j].lowercaseOnly))
                        group.push_back(pj);
                    if (sameRowPeer && !segments[i].original.empty() &&
                        segments[i].original == segments[j].original) {
                        group.push_back(pj);
                        group.push_back(pj);
                    }
                }
            }
            if (group.size() >= 2) {
                std::sort(group.begin(), group.end());
                harmonizedPreferred[i] = group[group.size() / 2];
            }
        }
    }

    // Direct stroke-density bold weight detection + same-tier peer weight harmonization so genuine
    // bold headings stay bold while split headlines, symmetric buttons and matching headers always agree.
    {
        std::vector<float> strokeRatios(output.size(), 0.f);
        for (size_t i = 0; i < translatedSegmentCount; ++i) {
            if (!preps[i].active || preps[i].column || stemWidths[i] <= 0.f)
                continue;
            const float emRef = std::max(8.f, harmonizedPreferred[i] > 0.f ? harmonizedPreferred[i]
                                                                           : preps[i].detectedHeight);
            strokeRatios[i] = stemWidths[i] / emRef;
        }
        for (size_t i = 0; i < translatedSegmentCount; ++i) {
            if (!preps[i].active || preps[i].column || stemWidths[i] <= 0.f) {
                output[i].bold = false;
                continue;
            }
            const float hpI = std::max(1.f, harmonizedPreferred[i]);
            const RECT ri = output[i].sourceRect;
            std::vector<float> gRatios, gStems, gDens;
            for (size_t j = 0; j < translatedSegmentCount; ++j) {
                if (!preps[j].active || preps[j].column || stemWidths[j] <= 0.f)
                    continue;
                if (colorDistLocal(output[i].background, output[j].background) > 45)
                    continue;
                if (textColors[i].has_value() && textColors[j].has_value() &&
                    colorDistLocal(*textColors[i], *textColors[j]) > 55)
                    continue;
                const float hpJ = std::max(1.f, harmonizedPreferred[j]);
                if (std::max(hpI, hpJ) > std::min(hpI, hpJ) * 1.15f)
                    continue;
                const RECT rj = output[j].sourceRect;
                bool peer = (i == j);
                if (!peer) {
                    const LONG vGap = rj.top >= ri.bottom
                                          ? (rj.top - ri.bottom)
                                          : (ri.top >= rj.bottom ? (ri.top - rj.bottom) : 0);
                    const LONG hGap = rj.left >= ri.right
                                          ? (rj.left - ri.right)
                                          : (ri.left >= rj.right ? (ri.left - rj.right) : 0);
                    const LONG vOv = std::min(ri.bottom, rj.bottom) - std::max(ri.top, rj.top);
                    const LONG cxI = (ri.left + ri.right) / 2, cxJ = (rj.left + rj.right) / 2;
                    const bool alignedCol =
                        (std::abs(ri.left - rj.left) <= std::max<LONG>(24, LONG(hpI * 1.5f)) ||
                         std::abs(ri.right - rj.right) <= std::max<LONG>(24, LONG(hpI * 1.5f)) ||
                         std::abs(cxI - cxJ) <= std::max<LONG>(20, LONG(hpI * 1.2f))) &&
                        vGap <= std::max<LONG>(320, LONG(hpI * 16.f));
                    const bool sameRow =
                        vOv * 10 >= std::min(Height(ri), Height(rj)) * 5 &&
                        hGap <= std::max<LONG>(360, LONG(hpI * 18.f));
                    const bool shortHeadings =
                        !preps[i].isProse && !preps[j].isProse &&
                        output[i].detectedLines.size() == 1 && output[j].detectedLines.size() == 1 &&
                        std::abs(ri.left - rj.left) <= std::max<LONG>(16, LONG(hpI * 1.0f));
                    peer = alignedCol || sameRow || shortHeadings;
                }
                if (peer) {
                    gRatios.push_back(strokeRatios[j]);
                    gStems.push_back(stemWidths[j]);
                    gDens.push_back(inkDensities[j]);
                }
            }
            if (gRatios.empty()) {
                output[i].bold = false;
                continue;
            }
            std::sort(gRatios.begin(), gRatios.end());
            std::sort(gStems.begin(), gStems.end());
            std::sort(gDens.begin(), gDens.end());
            const float medRatio = gRatios[gRatios.size() / 2];
            const float medStem = gStems[gStems.size() / 2];
            const float medDens = gDens[gDens.size() / 2];
            output[i].bold =
                medStem >= 1.75f && ((medRatio >= 0.148f && medDens >= 0.28f) || medRatio >= 0.175f);
        }
        // Synchronize final boolean bold weight across same-row symmetric buttons and side-by-side titles.
        for (size_t i = 0; i < translatedSegmentCount; ++i) {
            if (!preps[i].active || preps[i].column || !Valid(output[i].sourceRect))
                continue;
            const RECT ri = output[i].sourceRect;
            const float hpI = std::max(1.f, harmonizedPreferred[i]);
            for (size_t j = i + 1; j < translatedSegmentCount; ++j) {
                if (!preps[j].active || preps[j].column || !Valid(output[j].sourceRect))
                    continue;
                if (output[i].bold == output[j].bold)
                    continue;
                if (colorDistLocal(output[i].background, output[j].background) > 45)
                    continue;
                const float hpJ = std::max(1.f, harmonizedPreferred[j]);
                if (std::max(hpI, hpJ) > std::min(hpI, hpJ) * 1.20f)
                    continue;
                const RECT rj = output[j].sourceRect;
                const LONG vOv = std::min(ri.bottom, rj.bottom) - std::max(ri.top, rj.top);
                const LONG hGap = rj.left >= ri.right
                                      ? (rj.left - ri.right)
                                      : (ri.left >= rj.right ? (ri.left - rj.right) : 0);
                if (vOv * 10 >= std::min(Height(ri), Height(rj)) * 5 &&
                    hGap <= std::max<LONG>(480, LONG(hpI * 22.f))) {
                    const float otherRatio = output[i].bold ? strokeRatios[j] : strokeRatios[i];
                    if (otherRatio >= 0.125f) {
                        output[i].bold = true;
                        output[j].bold = true;
                    }
                }
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
            // Cap vertical column font size against adjacent strip center spacing so neighboring
            // manga columns never touch or crowd each other.
            if (prep.strips.size() >= 2) {
                std::vector<float> centers;
                centers.reserve(prep.strips.size());
                for (const auto& s : prep.strips)
                    centers.push_back(0.5f * float(s.left + s.right));
                std::sort(centers.begin(), centers.end());
                float minGap = 1e9f;
                for (size_t k = 1; k < centers.size(); ++k) {
                    const float g = centers[k] - centers[k - 1];
                    if (g >= 4.f)
                        minGap = std::min(minGap, g);
                }
                if (minGap < 1e8f)
                    size = std::min(size, std::max(8.f, minGap * .92f));
            }
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
        const bool isRtlText = HasRtlScript(prep.text);
        const float effectiveOverhang =
            item.centerAligned ? (2.f * std::min({item.maxOverhang * .5f, item.maxLeftOverhang, item.maxOverhang}))
                               : (isRtlText ? item.maxLeftOverhang : item.maxOverhang);
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
        bool wrap = prep.wrapText;
        size_t maxRows = prep.allowedRows;
        const auto holdsBox = [&](const DWRITE_TEXT_METRICS& m, float fontSize) {
            if (m.lineCount == 1 && maxRows <= 1)
                return m.widthIncludingTrailingWhitespace <= prep.width + effectiveOverhang + .5f;
            return visualBoxHeight(m, fontSize) <= prep.height + .5f &&
                   m.widthIncludingTrailingWhitespace <= prep.width + effectiveOverhang + .5f;
        };
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
                float low = std::max(9.f * scale, std::min(targetPreferred, targetPreferred * .55f));
                low = std::min(low, targetPreferred);
                float high = targetPreferred;
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
        item.overflow = (metrics.lineCount == 1 && maxRows <= 1)
                            ? (metrics.widthIncludingTrailingWhitespace > prep.width + effectiveOverhang + 1.f)
                            : (visualBoxHeight(metrics, item.fontSize) > prep.height + 1.f ||
                               metrics.widthIncludingTrailingWhitespace > prep.width + effectiveOverhang + .5f);
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
                if (colorDistLocal(output[i].background, output[j].background) > 45)
                    continue;
                if (textColors[i].has_value() && textColors[j].has_value() &&
                    colorDistLocal(*textColors[i], *textColors[j]) > 55)
                    continue;
                const float hpJ = harmonizedPreferred[j];
                if (std::max(hpI, hpJ) > std::min(hpI, hpJ) * 1.15f)
                    continue;
                if (std::max(preps[i].preferred, preps[j].preferred) >
                    std::min(preps[i].preferred, preps[j].preferred) * 1.14f)
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
                if (coupled && output[j].fontSize >= std::max(9.f * scale, hpI * .60f))
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
    constexpr double minimumContrast = 3.0;
    if (contrast(preferred) >= minimumContrast)
        return preferred;
    const double blackContrast = contrast(RGB(0, 0, 0));
    const double whiteContrast = contrast(RGB(255, 255, 255));
    int target = blackContrast >= whiteContrast ? 0 : 255;
    double requiredContrast = minimumContrast;
    // Preserve original light/dark polarity when that endpoint achieves >= 3.05 contrast (or >= 2.4
    // for white text on saturated blue chat bubbles), while flipping low-contrast pastel/beige pairs
    // (such as white on light beige/lavender where whiteContrast < 3.0) to the high-contrast endpoint.
    if (preservePolarity) {
        const bool lighterPref = luminance(preferred) >= luminance(background);
        const double sameEndContrast = lighterPref ? whiteContrast : blackContrast;
        const int rBg = int(GetRValue(background)), gBg = int(GetGValue(background)),
                  bBg = int(GetBValue(background));
        const int bgChroma = std::max({rBg, gBg, bBg}) - std::min({rBg, gBg, bBg});
        const bool saturatedBlueBubble =
            lighterPref && bgChroma >= 48 && bBg >= 200 && rBg <= 108 && (bBg - rBg) >= 110;
        const bool allowPolarity =
            sameEndContrast >= 3.05 || (saturatedBlueBubble && sameEndContrast >= 2.4);
        if (allowPolarity) {
            target = lighterPref ? 255 : 0;
            requiredContrast = std::min(minimumContrast, sameEndContrast - .02);
        }
    }
    // Keep the measured hue and move only its lightness until the pair separates, so a blue link
    // stays blue and a muted caption stays soft while reaching readable contrast.
    for (int step = 1; step <= 32; ++step) {
        const float t = float(step) / 32.f;
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
    auto wcagContrast = [](COLORREF a, COLORREF b) {
        auto relLum = [](COLORREF c) {
            auto lin = [](int v) {
                double s = v / 255.;
                return s <= .04045 ? s / 12.92 : std::pow((s + .055) / 1.055, 2.4);
            };
            return .2126 * lin(GetRValue(c)) + .7152 * lin(GetGValue(c)) + .0722 * lin(GetBValue(c));
        };
        const double la = relLum(a), lb = relLum(b);
        return (std::max(la, lb) + .05) / (std::min(la, lb) + .05);
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
    constexpr float blurSpread = 10.f;
    const auto blurredSpread = [&](const D2D1_RECT_F& box) {
        blurredBackdrop(D2D1::RectF(box.left - blurSpread, box.top - blurSpread, box.right + blurSpread,
                                    box.bottom + blurSpread));
    };
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
    const float scale = dpi / 96.f;
    for (const auto& item : items) {
        if (item.suppressed || (!item.layout && item.columnTexts.empty()))
            continue;
        auto box = rect(item.rect);
        const float pad = item.padding;
        // Compute measured surface color from the actual rebuilt cover pixels inside sourceRect.
        COLORREF measured = item.background;
        if (!item.eraseLines.empty()) {
            std::array<std::array<size_t, 256>, 3> hist{};
            size_t coverSamples = 0;
            for (const auto& row : item.eraseLines) {
                const int rw = int(row.rect.right - row.rect.left);
                const int rh = int(row.rect.bottom - row.rect.top);
                if (rw > 0 && rh > 0 && row.cover.size() >= size_t(rw) * size_t(rh) * 4) {
                    const RECT ref = Valid(item.sourceRect) ? item.sourceRect : row.rect;
                    const int x0 = std::clamp(int(ref.left - row.rect.left), 0, rw);
                    const int x1 = std::clamp(int(ref.right - row.rect.left), x0, rw);
                    const int y0 = std::clamp(int(ref.top - row.rect.top), 0, rh);
                    const int y1 = std::clamp(int(ref.bottom - row.rect.top), y0, rh);
                    const int st = std::max(1, int(std::sqrt(double(std::max(1, (x1 - x0) * (y1 - y0))) / 1024.)));
                    for (int y = y0; y < y1; y += st)
                        for (int x = x0; x < x1; x += st) {
                            const auto* q = &row.cover[(size_t(y) * size_t(rw) + size_t(x)) * 4];
                            ++hist[0][q[2]];
                            ++hist[1][q[1]];
                            ++hist[2][q[0]];
                            ++coverSamples;
                        }
                }
            }
            if (coverSamples > 0) {
                int med[3]{};
                for (int c = 0; c < 3; ++c) {
                    size_t acc = 0;
                    for (int v = 0; v < 256; ++v) {
                        acc += hist[c][v];
                        if (acc >= (coverSamples + 1) / 2) {
                            med[c] = v;
                            break;
                        }
                    }
                }
                measured = RGB(med[0], med[1], med[2]);
            } else {
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
                if (count > 0)
                    measured = RGB(int(sums[0] / count), int(sums[1] / count), int(sums[2] / count));
            }
        }
        const COLORREF textFill =
            ReadableTextColor(item.sourceColor.value_or(textColor), measured, item.sourceColor.has_value());
        brush->SetColor(color(textFill));

        const double contrast = wcagContrast(textFill, measured);
        const float fillLum =
            .299f * GetRValue(textFill) + .587f * GetGValue(textFill) + .114f * GetBValue(textFill);
        const float surfLum =
            .299f * GetRValue(measured) + .587f * GetGValue(measured) + .114f * GetBValue(measured);
        float effectiveStroke = settings.outlineWidth * scale;
        COLORREF rimColor = settings.outlineColor;
        if (settings.outlineColorMode == 1) {
            if (item.uniformBackdrop && contrast >= 2.6 && settings.matchTextColor && item.sourceColor.has_value()) {
                effectiveStroke = 0.f;
            } else if (surfLum >= 140.f && fillLum < surfLum) {
                rimColor = measured;
            } else {
                rimColor = RGB(255 - GetRValue(textFill), 255 - GetGValue(textFill),
                               255 - GetBValue(textFill));
            }
        } else if (settings.matchTextColor && settings.outlineColor == RGB(0, 0, 0) &&
                   settings.outlineColorMode == 0) {
            if (item.hasStroke && item.strokeColor.has_value()) {
                // Propagate genuine source outline (e.g. black fill + white rim in manga panels).
                rimColor = *item.strokeColor;
                effectiveStroke =
                    std::max(effectiveStroke, std::clamp(item.fontSize * .09f, 1.2f * scale, 2.5f * scale));
            } else if (item.uniformBackdrop && contrast >= 2.55) {
                // Flat/uniform UI surfaces with clear separation draw crisp un-outlined glyphs without
                // dark rim crust (chat bubbles, tooltips, pills, floating buttons, web pages).
                effectiveStroke = 0.f;
            } else {
                // Textured/photo/game artwork or marginal contrast keeps a clean protective rim.
                rimColor = fillLum >= 160.f ? RGB(20, 20, 20) : RGB(245, 245, 245);
            }
        }
        rimBrush->SetColor(color(rimColor));

        const bool isRtlLayout =
            item.layout && item.layout->GetReadingDirection() == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
        float clipLeft = box.left + pad;
        float clipRight = box.right - pad + item.maxOverhang;
        if (!item.columnTexts.empty()) {
            clipLeft = box.left - 2.f;
            clipRight = box.right + 2.f;
            for (const auto& column : item.columnTexts) {
                clipLeft = std::min(clipLeft, float(column.rect.left) - 2.f);
                clipRight = std::max(clipRight, float(column.rect.right) + 2.f);
            }
        } else if (item.centerAligned) {
            const float halfSym =
                std::min({item.maxOverhang * .5f, item.maxLeftOverhang, item.maxOverhang});
            clipLeft = box.left + pad - halfSym;
            clipRight = box.right - pad + halfSym;
        } else if (isRtlLayout) {
            clipLeft = box.left + pad - item.maxLeftOverhang;
            clipRight = box.right - pad;
        }
        target->PushAxisAlignedClip(
            D2D1::RectF(clipLeft, box.top, clipRight, box.bottom),
            D2D1_ANTIALIAS_MODE_ALIASED);
        auto drawLayout = [&](IDWriteTextLayout* layout, float x, float y) {
            if (effectiveStroke <= 0.f) {
                target->DrawTextLayout(D2D1::Point2F(x, y), layout, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            } else {
                auto rim = CreateRimRenderer(target, factory.Get(), brush.Get(), rimBrush.Get(),
                                             effectiveStroke, settings.outlineStyle, scale);
                CheckHR(layout->Draw(nullptr, rim.Get(), x, y), "layout");
            }
        };
        if (!item.columnTexts.empty()) {
            // Center each strip's (Width(strip) + 4 px) layout exactly on (strip.left + strip.right) / 2.
            for (const auto& column : item.columnTexts) {
                if (!column.layout)
                    continue;
                const auto strip = rect(column.rect);
                drawLayout(column.layout.Get(), strip.left - 2.f, strip.top - item.scroll);
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
                const float minDrawY = innerTop - inkTop;
                const float maxDrawY = std::max(minDrawY, innerBottom - inkBottom);
                const bool multiLineLeftProse = item.detectedLines.size() >= 2 && !item.centerAligned;
                if (multiLineLeftProse) {
                    // Top-align multi-line left-aligned prose to the source first line so shorter
                    // translations never drift downward and open awkward inter-paragraph gaps.
                    LONG firstTop = item.detectedLines.front().top;
                    for (const auto& line : item.detectedLines)
                        if (Valid(line))
                            firstTop = std::min(firstTop, line.top);
                    drawY = std::clamp(float(firstTop) - inkTop, minDrawY, maxDrawY);
                } else {
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
                    drawY = std::clamp(targetCenter - visualCenter, minDrawY, maxDrawY);
                }
            }
            drawLayout(item.layout.Get(), box.left + pad, drawY);
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
