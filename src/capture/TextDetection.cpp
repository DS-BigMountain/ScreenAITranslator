#include "TextDetection.h"
#include "Capture.h"
#include "common/Platform.h"
#include "common/Diagnostics.h"
#include "common/TextMetrics.h"
#include "BundledOcr.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

namespace sat {
namespace {
RECT TightGlyphBounds(RECT rect, const Image& image) {
    // DB expands a shrunken mask for recognition. Recover glyph height on flat or smoothly varying
    // backgrounds before grouping; expanded recognition crops must not be mistaken for line spacing.
    if (!Valid(rect) || image.bgra.size() != size_t(image.width) * image.height * 4)
        return rect;
    auto key = [](const unsigned char* p) {
        return (int(p[0] >> 4) << 8) | (int(p[1] >> 4) << 4) | (p[2] >> 4);
    };
    const int boxW = Width(rect);
    const int chunkW = boxW > 128 ? 96 : boxW;
    const int chunks = std::max(1, (boxW + chunkW - 1) / chunkW);
    struct ChunkBg {
        int b{}, g{}, r{};
    };
    std::vector<ChunkBg> chunkBg(static_cast<size_t>(chunks));
    std::array<size_t, 4096> colors{};
    for (int c = 0; c < chunks; ++c) {
        const int x0 = rect.left + c * chunkW;
        const int x1 = std::min<int>(rect.right, x0 + chunkW);
        colors.fill(0);
        size_t count = 0;
        for (int y = rect.top; y < rect.bottom; ++y)
            for (int x = x0; x < x1; ++x) {
                ++colors[key(&image.bgra[(size_t(y) * image.width + x) * 4])];
                ++count;
            }
        if (!count)
            return rect;
        auto mode = std::max_element(colors.begin(), colors.end());
        int index = int(mode - colors.begin());
        int b = ((index >> 8) & 15) * 16 + 8;
        int g = ((index >> 4) & 15) * 16 + 8;
        int r = (index & 15) * 16 + 8;
        if (*mode < count * .45) {
            std::array<size_t, 4096> ring{};
            size_t ringCount = 0;
            const int rx0 = std::max(0, x0 - 2), rx1 = std::min(image.width, x1 + 2);
            const int ry0 = std::max(0, int(rect.top) - 2), ry1 = std::min(image.height, int(rect.bottom) + 2);
            for (int y = ry0; y < ry1; ++y)
                for (int x = rx0; x < rx1; ++x) {
                    if (x >= x0 && x < x1 && y >= rect.top && y < rect.bottom) {
                        x = x1 - 1;
                        continue;
                    }
                    ++ring[key(&image.bgra[(size_t(y) * image.width + x) * 4])];
                    ++ringCount;
                }
            if (ringCount >= 8) {
                auto ringMode = std::max_element(ring.begin(), ring.end());
                if (*ringMode >= ringCount * .45) {
                    index = int(ringMode - ring.begin());
                    b = ((index >> 8) & 15) * 16 + 8;
                    g = ((index >> 4) & 15) * 16 + 8;
                    r = (index & 15) * 16 + 8;
                }
            }
        }
        chunkBg[size_t(c)] = {b, g, r};
    }
    RECT ink{rect.right, rect.bottom, rect.left, rect.top};
    for (int y = rect.top; y < rect.bottom; ++y) {
        int pixels = 0, l = rect.right, rr = rect.left;
        int currentRun = 0, maxRun = 0;
        for (int x = rect.left; x < rect.right; ++x) {
            const auto& bg = chunkBg[size_t((x - rect.left) / chunkW)];
            auto p = &image.bgra[(size_t(y) * image.width + x) * 4];
            if (std::abs(int(p[0]) - bg.b) + std::abs(int(p[1]) - bg.g) + std::abs(int(p[2]) - bg.r) > 78) {
                ++pixels;
                ++currentRun;
                maxRun = std::max(maxRun, currentRun);
                l = std::min(l, x);
                rr = std::max(rr, x + 1);
            } else {
                currentRun = 0;
            }
        }
        // Exclude solid horizontal rules and underlines from glyph bounds.
        const bool isRule = boxW >= 32 && maxRun > std::max(24, int(boxW * .55));
        if (!isRule && pixels >= std::max(2, boxW / 500) && pixels < boxW * .88) {
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
bool StartsListMarker(const std::string& text) {
    const auto wide = Wide(text);
    const auto first = wide.find_first_not_of(L" \t");
    if (first == std::wstring::npos)
        return false;
    const wchar_t ch = wide[first];
    if (std::wstring_view(L"・•‣◦▪▫◆◇●○◎■□▲△※").find(ch) != std::wstring_view::npos)
        return true;
    if (ch >= 0x2460 && ch <= 0x2473)
        return true;
    if ((ch >= L'0' && ch <= L'9') || (ch >= L'０' && ch <= L'９'))
        return true;
    if ((ch == L'(' || ch == L'（') && first + 1 < wide.size() &&
        ((wide[first + 1] >= L'0' && wide[first + 1] <= L'9') ||
         (wide[first + 1] >= L'０' && wide[first + 1] <= L'９')))
        return true;
    return false;
}
bool HasSeparator(const Image& image, RECT a, RECT b) {
    const int left = std::max(a.left, b.left), right = std::min(a.right, b.right);
    const int span = right - left;
    if (span < 20 || b.top <= a.bottom)
        return false;
    const LONG startY = std::max<LONG>(1, a.bottom);
    const LONG endY = std::min<LONG>(image.height, b.top);
    for (LONG y = startY; y < endY; ++y) {
        int edges = 0;
        int currentRun = 0, maxRun = 0, gap = 0;
        for (int x = left; x < right; ++x) {
            const auto p = (size_t(y) * image.width + x) * 4;
            int delta = 0;
            for (int c = 0; c < 3; ++c)
                delta += std::abs(int(image.bgra[p + c]) - int(image.bgra[p - image.width * 4 + c]));
            if (delta > 28) {
                ++edges;
                currentRun += 1 + gap;
                gap = 0;
                maxRun = std::max(maxRun, currentRun);
            } else if (currentRun > 0 && gap < 1) {
                ++gap;
            } else {
                currentRun = 0;
                gap = 0;
            }
        }
        if ((edges > .60 * span && maxRun >= std::max(16, int(span * .40))) || edges > .75 * span)
            return true;
    }
    return false;
}
bool HasVerticalSeparator(const Image& image, RECT leftRect, RECT rightRect, LONG scanStartX = -1,
                          LONG scanEndX = -1) {
    const LONG top = std::max<LONG>(0, std::max(leftRect.top, rightRect.top));
    const LONG bottom = std::min<LONG>(image.height, std::min(leftRect.bottom, rightRect.bottom));
    const int span = int(bottom - top);
    if (span < 8)
        return false;
    const bool fromInkSpan = scanStartX >= 0 && scanEndX >= 0;
    const LONG rawStart = fromInkSpan ? scanStartX : leftRect.right;
    const LONG rawEnd = fromInkSpan ? scanEndX : rightRect.left;
    // Stay clear of glyph edges and 1 px anti-aliased halos when scanning between tight ink spans
    // so neighbor delta checks (x - 1, x + 1) only inspect true inter-word background or UI dividers.
    const LONG inset = fromInkSpan ? 2 : 1;
    if (rawEnd - rawStart < inset * 2 + 1)
        return false;
    const LONG startX = std::max<LONG>(1, rawStart + inset);
    const LONG endX = std::min<LONG>(image.width - 2, rawEnd - inset - 1);
    if (endX < startX)
        return false;
    auto edgeAt = [&](LONG x, LONG y) {
        const auto p = (size_t(y) * image.width + x) * 4;
        int deltaL = 0, deltaR = 0;
        for (int c = 0; c < 3; ++c) {
            deltaL += std::abs(int(image.bgra[p + c]) - int(image.bgra[p - 4 + c]));
            deltaR += std::abs(int(image.bgra[p + c]) - int(image.bgra[p + 4 + c]));
        }
        return std::max(deltaL, deltaR) > 16;
    };
    for (LONG x = startX; x <= endX; ++x) {
        int edges = 0;
        int minR = 255, maxR = 0, minG = 255, maxG = 0, minB = 255, maxB = 0;
        for (LONG y = top; y < bottom; ++y) {
            const auto p = (size_t(y) * image.width + x) * 4;
            if (edgeAt(x, y))
                ++edges;
            minR = std::min(minR, int(image.bgra[p + 2]));
            maxR = std::max(maxR, int(image.bgra[p + 2]));
            minG = std::min(minG, int(image.bgra[p + 1]));
            maxG = std::max(maxG, int(image.bgra[p + 1]));
            minB = std::min(minB, int(image.bgra[p + 0]));
            maxB = std::max(maxB, int(image.bgra[p + 0]));
        }
        int extTop = 0, extBottom = 0;
        for (LONG y = std::max<LONG>(0, top - 4); y < top; ++y)
            extTop += edgeAt(x, y) ? 1 : 0;
        for (LONG y = bottom; y < std::min<LONG>(image.height, bottom + 4); ++y)
            extBottom += edgeAt(x, y) ? 1 : 0;
        const bool continuesVertically = extTop >= 2 || extBottom >= 2;
        const bool uniformColor = (maxR - minR) + (maxG - minG) + (maxB - minB) <= 36;
        const bool uniformStroke = uniformColor && edges > span * .55;
        const bool verticalEdge = edges > span * .72 && (continuesVertically || span >= 36);
        if (verticalEdge || uniformStroke)
            return true;
    }
    return false;
}
struct LineAppearance {
    bool hasInk{};
    int bgR{}, bgG{}, bgB{};
    int inkR{}, inkG{}, inkB{};
};
LineAppearance MeasureLineAppearance(const Image& image, RECT rect) {
    LineAppearance result;
    if (!Valid(rect))
        return result;
    const int stepX = std::max(1, Width(rect) / 160);
    std::array<size_t, 4096> bins{};
    size_t sampled = 0;
    auto binOf = [](const unsigned char* p) {
        return (int(p[2] >> 4) << 8) | (int(p[1] >> 4) << 4) | (p[0] >> 4);
    };
    for (int y = rect.top; y < rect.bottom; ++y)
        for (int x = rect.left; x < rect.right; x += stepX) {
            ++bins[binOf(&image.bgra[(size_t(y) * image.width + x) * 4])];
            ++sampled;
        }
    if (!sampled)
        return result;
    int mode = int(std::max_element(bins.begin(), bins.end()) - bins.begin());
    if (bins[mode] < sampled * .45) {
        std::array<size_t, 4096> ringBins = bins;
        const LONG rLeft = std::max<LONG>(0, rect.left - 2);
        const LONG rTop = std::max<LONG>(0, rect.top - 2);
        const LONG rRight = std::min<LONG>(image.width, rect.right + 2);
        const LONG rBottom = std::min<LONG>(image.height, rect.bottom + 2);
        for (LONG y = rTop; y < rBottom; ++y)
            for (LONG x = rLeft; x < rRight; x += stepX) {
                if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
                    continue;
                ringBins[binOf(&image.bgra[(size_t(y) * image.width + x) * 4])] += 2;
            }
        mode = int(std::max_element(ringBins.begin(), ringBins.end()) - ringBins.begin());
    }
    const int br = ((mode >> 8) & 15) * 16 + 8, bg = ((mode >> 4) & 15) * 16 + 8,
              bb = (mode & 15) * 16 + 8;
    long long bgSum[3]{}, inkSum[3]{};
    size_t bgCount = 0, inkCount = 0;
    for (int y = rect.top; y < rect.bottom; ++y)
        for (int x = rect.left; x < rect.right; x += stepX) {
            const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
            const int dist = std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb);
            if (dist < 42) {
                bgSum[0] += p[2];
                bgSum[1] += p[1];
                bgSum[2] += p[0];
                ++bgCount;
            } else if (dist >= 54) {
                inkSum[0] += p[2];
                inkSum[1] += p[1];
                inkSum[2] += p[0];
                ++inkCount;
            }
        }
    if (bgCount < sampled * .30 || inkCount < 4 || inkCount > sampled * .85)
        return result;
    result.hasInk = true;
    result.bgR = int(bgSum[0] / bgCount);
    result.bgG = int(bgSum[1] / bgCount);
    result.bgB = int(bgSum[2] / bgCount);
    result.inkR = int(inkSum[0] / inkCount);
    result.inkG = int(inkSum[1] / inkCount);
    result.inkB = int(inkSum[2] / inkCount);
    return result;
}
bool AppearanceMatches(const LineAppearance& a, const LineAppearance& b) {
    if (!a.hasInk || !b.hasInk)
        return true;
    const int bgDiff = std::abs(a.bgR - b.bgR) + std::abs(a.bgG - b.bgG) + std::abs(a.bgB - b.bgB);
    if (bgDiff > 32)
        return false;
    const int inkDiff = std::abs(a.inkR - b.inkR) + std::abs(a.inkG - b.inkG) + std::abs(a.inkB - b.inkB);
    return inkDiff <= 68;
}
std::pair<LONG, LONG> HorizontalInkSpan(const Image& image, RECT rect, const LineAppearance& app) {
    if (!Valid(rect) || !app.hasInk)
        return {rect.left, rect.right};
    LONG minX = rect.right, maxX = rect.left;
    for (LONG y = rect.top; y < rect.bottom; ++y) {
        const auto* row = &image.bgra[size_t(y) * image.width * 4];
        for (LONG x = rect.left; x < rect.right; ++x) {
            const auto* p = &row[size_t(x) * 4];
            const int dist =
                std::abs(int(p[2]) - app.bgR) + std::abs(int(p[1]) - app.bgG) + std::abs(int(p[0]) - app.bgB);
            if (dist >= 48) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x + 1);
            }
        }
    }
    if (minX >= maxX)
        return {rect.left, rect.right};
    return {minX, maxX};
}
bool IsUnreadText(std::string_view text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}
bool EndsSentence(std::string_view text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    if (text.empty())
        return false;
    const char c = text.back();
    if (c == '.' || c == '!' || c == '?' || c == ':')
        return true;
    constexpr std::string_view kUtf8Closers[] = {"。", "！", "？", "：", "”", "」", "』",
                                                  "؟",  "؛",  "۔",  "।",  "॥"};
    for (auto closer : kUtf8Closers)
        if (text.size() >= closer.size() && text.substr(text.size() - closer.size()) == closer)
            return true;
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
            const bool unread = IsUnreadText(lines[i].text);
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
    const bool allUnread =
        std::all_of(lines.begin(), lines.end(), [](const TextLine& l) { return IsUnreadText(l.text); });
    // A list marker and its text can be detected as two slightly overlapping boxes, and a short
    // caption or prose line can arrive as several word fragments whose tight ink bounds vary between
    // x-height ("man", "ever") and ascender+descender height ("bad news,", "mapped").
    // Run a fixed-point merge preferring the closest horizontal neighbor on each step, and assemble
    // constituent fragments strictly in horizontal reading order so top-jitter never scrambles words.
    struct LineGroup {
        std::vector<TextLine> parts;
        RECT rect{};
        float confidence{1.f};
        bool unread{};
        LineAppearance app{};
        std::pair<LONG, LONG> inkSpan{};
    };
    std::vector<LineGroup> groups;
    groups.reserve(lines.size());
    for (auto& line : lines) {
        LineGroup g;
        g.rect = line.rect;
        g.confidence = line.confidence;
        g.unread = IsUnreadText(line.text);
        g.app = MeasureLineAppearance(image, line.rect);
        g.inkSpan = HorizontalInkSpan(image, line.rect, g.app);
        g.parts.push_back(std::move(line));
        groups.push_back(std::move(g));
    }
    auto canMergeHorizontal = [&](const LineGroup& ga, const LineGroup& gb, LONG& outGap) -> bool {
        if (!allUnread && (ga.unread || gb.unread))
            return false;
        const RECT a = ga.rect, b = gb.rect;
        // A vertical column is its own reading unit: joining columns side by side would splice
        // separate lines of dialogue together, and same-column rows are joined by the passage
        // pass below.
        if (Height(a) > Width(a) * 3 / 2 || Height(b) > Width(b) * 3 / 2)
            return false;
        const LONG minH = std::min(Height(a), Height(b));
        const LONG maxH = std::max(Height(a), Height(b));
        if (minH <= 0 || maxH > LONG(std::lround(minH * 1.85f)))
            return false;
        const LONG vOverlap = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
        const LONG centreA = (a.top + a.bottom) / 2, centreB = (b.top + b.bottom) / 2;
        const bool tightHeight = maxH <= LONG(std::lround(minH * 1.30f));
        if (tightHeight) {
            if (std::abs(centreA - centreB) > std::max<LONG>(3, minH / 3) || vOverlap < minH * .55f)
                return false;
        } else {
            const bool sharedBaseline = std::abs(a.bottom - b.bottom) <= std::max<LONG>(4, LONG(std::lround(maxH * .30f)));
            const bool sharedCapTop = std::abs(a.top - b.top) <= std::max<LONG>(4, LONG(std::lround(maxH * .30f)));
            const bool sharedCentre = std::abs(centreA - centreB) <= std::max<LONG>(4, LONG(std::lround(maxH * .25f)));
            if (vOverlap < minH * .72f || (!sharedBaseline && !sharedCapTop && !sharedCentre))
                return false;
        }
        if (!AppearanceMatches(ga.app, gb.app))
            return false;
        const bool aFirst = a.left <= b.left;
        const RECT leftRect = aFirst ? a : b;
        const RECT rightRect = aFirst ? b : a;
        const auto leftInk = aFirst ? ga.inkSpan : gb.inkSpan;
        const auto rightInk = aFirst ? gb.inkSpan : ga.inkSpan;
        const LONG gap = rightRect.left - leftRect.right;
        const LONG inkGap = rightInk.first - leftInk.second;
        const LONG refH = tightHeight ? minH : LONG(std::lround((minH + maxH) * .5f));
        const LONG maxInkGap = std::max<LONG>(8, LONG(std::lround(refH * (tightHeight ? 1.15f : .95f))));
        const LONG maxBoxGap = std::max<LONG>(4, LONG(std::lround(refH * (tightHeight ? .68f : .72f))));
        RECT overlap{};
        const bool touching = IntersectRect(&overlap, &a, &b) && Height(overlap) >= .65f * minH &&
                              Width(overlap) < .5f * std::min(Width(a), Width(b)) && inkGap <= maxInkGap;
        const bool adjacent = gap <= maxBoxGap && gap >= -std::max<LONG>(4, refH / 4) && inkGap <= maxInkGap;
        if (!touching && !adjacent)
            return false;
        if (HasVerticalSeparator(image, leftRect, rightRect, leftInk.second, rightInk.first))
            return false;
        outGap = std::max<LONG>(0, gap);
        return true;
    };
    for (;;) {
        size_t bestI = groups.size(), bestJ = groups.size();
        LONG bestGap = LONG_MAX;
        for (size_t i = 0; i < groups.size(); ++i) {
            for (size_t j = i + 1; j < groups.size(); ++j) {
                LONG gap = 0;
                if (canMergeHorizontal(groups[i], groups[j], gap) && gap < bestGap) {
                    bestGap = gap;
                    bestI = i;
                    bestJ = j;
                }
            }
        }
        if (bestI >= groups.size())
            break;
        auto& dst = groups[bestI];
        auto& src = groups[bestJ];
        dst.parts.insert(dst.parts.end(), std::make_move_iterator(src.parts.begin()),
                         std::make_move_iterator(src.parts.end()));
        RECT merged{};
        UnionRect(&merged, &dst.rect, &src.rect);
        dst.rect = merged;
        dst.confidence = std::min(dst.confidence, src.confidence);
        dst.unread = dst.unread && src.unread;
        dst.inkSpan = {std::min(dst.inkSpan.first, src.inkSpan.first),
                       std::max(dst.inkSpan.second, src.inkSpan.second)};
        if (!dst.app.hasInk && src.app.hasInk)
            dst.app = src.app;
        groups.erase(groups.begin() + bestJ);
    }
    lines.clear();
    lines.reserve(groups.size());
    for (auto& g : groups) {
        bool rtlLine = false;
        for (const auto& part : g.parts)
            if (HasRtlScript(part.text)) {
                rtlLine = true;
                break;
            }
        std::stable_sort(g.parts.begin(), g.parts.end(), [rtlLine](const TextLine& a, const TextLine& b) {
            return rtlLine ? a.rect.left > b.rect.left : a.rect.left < b.rect.left;
        });
        TextLine combined;
        combined.rect = g.rect;
        combined.confidence = g.confidence;
        for (size_t k = 0; k < g.parts.size(); ++k) {
            if (k > 0)
                combined.text += ' ';
            combined.text += g.parts[k].text;
        }
        lines.push_back(std::move(combined));
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
            const int hOverlap = std::min(a.rect.right, b.rect.right) - std::max(a.rect.left, b.rect.left);
            const int vOverlap = std::min(a.rect.bottom, b.rect.bottom) - std::max(a.rect.top, b.rect.top);
            const bool sameColumn =
                hOverlap * 10 >= std::min(Width(a.rect), Width(b.rect)) * 4 &&
                vOverlap * 10 < std::min(Height(a.rect), Height(b.rect)) * 3;
            if (!sameColumn)
                return (a.rect.left + a.rect.right) > (b.rect.left + b.rect.right);
            return a.rect.top < b.rect.top;
        });
    else
        std::stable_sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) {
            return a.rect.top == b.rect.top ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
        });
    std::vector<LineAppearance> appearances(lines.size());
    for (size_t i = 0; i < lines.size(); ++i)
        appearances[i] = MeasureLineAppearance(image, lines[i].rect);
    std::vector<TextRegion> regions;
    std::vector<LineAppearance> regionAppearances;
    for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const auto& line = lines[lineIndex];
        const auto& lineApp = appearances[lineIndex];
        const bool lineUnread = IsUnreadText(line.text);
        size_t bestIndex = regions.size();
        int bestGap = INT_MAX;
        // A decorated line starts a new passage; it never continues the block above it.
        const bool opensPassage = StartsPassage(line.text);
        for (size_t regionIndex = 0; regionIndex < regions.size(); ++regionIndex) {
            auto& region = regions[regionIndex];
            if (opensPassage)
                continue;
            const bool regionUnread = IsUnreadText(region.text);
            if (lineUnread != regionUnread || (!allUnread && lineUnread))
                continue;
            const auto last = region.lines.back();
            const int gap = line.rect.top - last.bottom;
            const float h = region.lineHeight;
            const int overlap = std::min(last.right, line.rect.right) - std::max(last.left, line.rect.left);
            const bool sentenceEnded = EndsSentence(region.text);
            const bool tightLeftContinuation =
                !columnPage && !sentenceEnded &&
                std::abs(line.rect.left - last.left) <= std::max<LONG>(6, LONG(std::lround(h * .35f))) &&
                Width(last) >= Width(line.rect) * .72f;
            const float maxGap = columnPage ? h * .9f : (tightLeftContinuation ? h * .68f : h * .55f);
            const float minRatio = columnPage ? .65f : (sentenceEnded ? .80f : .55f);
            const float maxRatio = columnPage ? 1.45f : (tightLeftContinuation ? 1.45f : 1.24f);
            if (gap < -h * .25f || gap > maxGap || gap >= bestGap || Height(line.rect) < h * minRatio ||
                Height(line.rect) > h * maxRatio)
                continue;
            const LONG centerLast = (last.left + last.right) / 2;
            const LONG centerLine = (line.rect.left + line.rect.right) / 2;
            const bool centerAligned =
                std::abs(centerLast - centerLine) <= std::max<LONG>(6, LONG(std::lround(h * .75f)));
            const bool alignedHorizontally =
                std::abs(line.rect.left - last.left) <= h * 2 ||
                std::abs(line.rect.right - last.right) <= h || centerAligned;
            if (!alignedHorizontally ||
                overlap < (centerAligned ? .50f : .65f) * std::min(Width(last), Width(line.rect)))
                continue;
            if (!columnPage) {
                if (!AppearanceMatches(regionAppearances[regionIndex], lineApp))
                    continue;
                const bool rtlRegion = HasRtlScript(region.text) || HasRtlScript(line.text);
                // An indented next line or a short preceding heading marks a paragraph boundary, unless
                // the lines are center-aligned inside the same speech bubble or centered caption.
                if (!centerAligned &&
                    (rtlRegion ? (last.right - line.rect.right > h * 1.1f ||
                                  (region.lines.size() >= 2 && line.rect.right - last.right > h * 1.1f))
                               : (line.rect.left - last.left > h * 1.1f ||
                                  (region.lines.size() >= 2 && last.left - line.rect.left > h * 1.1f))))
                    continue;
                if (Width(line.rect) > h * 4 && Width(last) < Width(line.rect) * .72f &&
                    (!centerAligned || sentenceEnded || gap > h * .45f))
                    continue;
                // A substantially shorter trailing line in a multi-line region marks paragraph end.
                if (region.lines.size() >= 2 && !centerAligned &&
                    (rtlRegion ? (last.left > region.rect.left + LONG(h * 1.2f))
                               : (last.right < region.rect.right - LONG(h * 1.2f))))
                    continue;
                // A sentence-ending line that stops noticeably short of the paragraph width ends the block.
                if (sentenceEnded &&
                    (StartsListMarker(line.text) ||
                     Width(last) < std::max(Width(region.rect), Width(line.rect)) * .88f))
                    continue;
                // Short stacked UI labels/menu items with noticeable vertical spacing remain separate.
                if (Width(last) < h * 5.5f && Width(line.rect) < h * 5.5f && gap > h * .32f)
                    continue;
                if (region.lines.size() >= 2) {
                    std::vector<int> intraGaps;
                    intraGaps.reserve(region.lines.size() - 1);
                    for (size_t k = 1; k < region.lines.size(); ++k)
                        intraGaps.push_back(std::max(0, int(region.lines[k].top - region.lines[k - 1].bottom)));
                    std::sort(intraGaps.begin(), intraGaps.end());
                    const int medianIntra = intraGaps[intraGaps.size() / 2];
                    if (gap > std::max(h * .45f, float(medianIntra) * 1.45f + 2.f))
                        continue;
                }
            }
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
            bestIndex = regionIndex;
            bestGap = gap;
        }
        if (bestIndex == regions.size()) {
            regions.push_back(
                {0, line.rect, line.text, {line.rect}, float(Height(line.rect)), {}, line.confidence});
            regionAppearances.push_back(lineApp);
            continue;
        }
        auto* best = &regions[bestIndex];
        if (lineApp.hasInk)
            regionAppearances[bestIndex] = lineApp;
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
        const auto isTallStrip = [](RECT line) {
            return Width(line) > 0 && Height(line) * 5 > Width(line) * 6;
        };
        const auto strips = [&](const TextRegion& region) {
            if (region.lines.empty())
                return false;
            size_t tall = 0;
            for (RECT line : region.lines)
                tall += isTallStrip(line) ? 1 : 0;
            return tall >= 1 && tall * 2 >= region.lines.size();
        };
        const auto isColumnTail = [&](const TextRegion& region, float stripW) {
            if (strips(region))
                return true;
            if (region.lines.size() != 1)
                return false;
            const RECT line = region.lines.front();
            return Width(line) >= stripW * .55f && Width(line) <= stripW * 1.45f &&
                   Height(line) * 2 >= Width(line) && Height(line) <= stripW * 2.5f;
        };
        for (size_t i = 0; i < regions.size(); ++i)
            for (size_t j = i + 1; j < regions.size();) {
                auto& keep = regions[i];
                const auto& join = regions[j];
                const float strip = float(std::max(1, Width(keep.lines.back())));
                const LONG overlap =
                    std::min(keep.rect.bottom, join.rect.bottom) - std::max(keep.rect.top, join.rect.top);
                const LONG spacing = keep.rect.left - join.rect.right;
                // Neighbouring strips of one balloon share most of their glyph run and sit within a
                // couple of strip widths on the same bubble surface. Separate balloons keep a bubble
                // outline, a different surface, a wider gutter, or a different vertical span.
                if (!strips(keep) || !isColumnTail(join, strip) ||
                    Width(join.lines.front()) < strip * .55f || Width(join.lines.front()) > strip * 1.45f ||
                    spacing < -LONG(strip * .5f) || spacing > LONG(strip * 2.1f) ||
                    overlap * 100 <= std::min(Height(keep.rect), Height(join.rect)) * 45 ||
                    !AppearanceMatches(regionAppearances[i], regionAppearances[j]) ||
                    HasVerticalSeparator(image, join.rect, keep.rect)) {
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
                regionAppearances.erase(regionAppearances.begin() + j);
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
std::vector<std::string> DetectScriptLanguages(const std::vector<TextRegion>& regions, int ocrHint,
                                               std::string_view sourceHint) {
    struct RegionCounts {
        int kana{};
        int hangul{};
        int han{};
        int cyrillic{};
        int ukrainian{};
        int arabic{};
        int hebrew{};
        int thai{};
        int greek{};
        int devanagari{};
        int vietnamese{};
        int latinLetters{};
        int latinWords{};
    };
    std::vector<RegionCounts> perRegion;
    perRegion.reserve(regions.size());
    bool anyKana = false;
    bool anyNonLatin = false;
    for (const auto& r : regions) {
        if (r.text.empty())
            continue;
        RegionCounts c;
        int curWordLen = 0;
        auto flushWord = [&] {
            if (curWordLen >= 2)
                ++c.latinWords;
            curWordLen = 0;
        };
        for (size_t i = 0; i < r.text.size();) {
            const uint32_t cp = NextCodePoint(r.text, i);
            const bool isAsciiAlpha = (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
            const bool isLatinExt = (cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x1E00 && cp <= 0x1EFF);
            if ((cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
                (cp >= 0xFF65 && cp <= 0xFF9F)) {
                flushWord();
                ++c.kana;
                anyKana = true;
                anyNonLatin = true;
            } else if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) ||
                       (cp >= 0x3130 && cp <= 0x318F)) {
                flushWord();
                ++c.hangul;
                anyNonLatin = true;
            } else if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)) {
                flushWord();
                ++c.han;
                anyNonLatin = true;
            } else if ((cp >= 0x0400 && cp <= 0x04FF) || (cp >= 0x0500 && cp <= 0x052F)) {
                flushWord();
                ++c.cyrillic;
                if (cp == 0x0404 || cp == 0x0454 || cp == 0x0406 || cp == 0x0456 || cp == 0x0407 ||
                    cp == 0x0457 || cp == 0x0490 || cp == 0x0491)
                    ++c.ukrainian;
                anyNonLatin = true;
            } else if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
                       (cp >= 0x08A0 && cp <= 0x08FF)) {
                flushWord();
                ++c.arabic;
                anyNonLatin = true;
            } else if (cp >= 0x0590 && cp <= 0x05FF) {
                flushWord();
                ++c.hebrew;
                anyNonLatin = true;
            } else if (cp >= 0x0E00 && cp <= 0x0E7F) {
                flushWord();
                ++c.thai;
                anyNonLatin = true;
            } else if (cp >= 0x0370 && cp <= 0x03FF) {
                flushWord();
                ++c.greek;
                anyNonLatin = true;
            } else if (cp >= 0x0900 && cp <= 0x097F) {
                flushWord();
                ++c.devanagari;
                anyNonLatin = true;
            } else if (isAsciiAlpha || isLatinExt) {
                ++c.latinLetters;
                ++curWordLen;
                if ((cp >= 0x1EA0 && cp <= 0x1EF9) || cp == 0x0102 || cp == 0x0103 || cp == 0x0110 ||
                    cp == 0x0111 || cp == 0x0128 || cp == 0x0129 || cp == 0x0168 || cp == 0x0169 ||
                    cp == 0x01A0 || cp == 0x01A1 || cp == 0x01AF || cp == 0x01B0)
                    ++c.vietnamese;
            } else {
                flushWord();
            }
        }
        flushWord();
        perRegion.push_back(c);
    }
    std::vector<std::string> detected;
    auto pushUnique = [&](std::string_view code) {
        if (code.empty())
            return;
        if (std::find(detected.begin(), detected.end(), code) == detected.end())
            detected.emplace_back(code);
    };
    const bool latinHint = !sourceHint.empty() && sourceHint != "auto" && sourceHint != "zh-Hans" &&
                           sourceHint != "zh-Hant" && sourceHint != "ja" && sourceHint != "ko" &&
                           sourceHint != "ru" && sourceHint != "uk" && sourceHint != "bg" &&
                           sourceHint != "sr" && sourceHint != "be" && sourceHint != "ar" &&
                           sourceHint != "fa" && sourceHint != "ur" && sourceHint != "he" &&
                           sourceHint != "th" && sourceHint != "el" && sourceHint != "hi";
    for (const auto& c : perRegion) {
        if (c.kana > 0)
            pushUnique("ja");
        if (c.hangul > 0)
            pushUnique("ko");
        if (c.han > 0 && c.kana == 0) {
            if (ocrHint == OcrJapanese || sourceHint == "ja")
                pushUnique("ja");
            else if (!anyKana || c.han >= 8)
                pushUnique(sourceHint == "zh-Hant" ? "zh-Hant" : "zh-Hans");
        }
        if (c.arabic > 0)
            pushUnique(sourceHint == "fa" || sourceHint == "ur" ? sourceHint : "ar");
        if (c.hebrew > 0)
            pushUnique("he");
        if (c.thai > 0)
            pushUnique("th");
        if (c.greek > 0)
            pushUnique("el");
        if (c.devanagari > 0)
            pushUnique("hi");
        if (c.cyrillic > 0) {
            if (c.ukrainian > 0 || sourceHint == "uk")
                pushUnique("uk");
            else if (sourceHint == "bg" || sourceHint == "sr" || sourceHint == "be")
                pushUnique(sourceHint);
            else
                pushUnique("ru");
        }
        if (c.vietnamese > 0) {
            pushUnique("vi");
        } else if (c.latinLetters > 0) {
            const bool strongLatin = !anyNonLatin || (c.kana == 0 && c.hangul == 0 && c.han == 0 &&
                                                      c.cyrillic == 0 && c.arabic == 0 && c.thai == 0 &&
                                                      c.greek == 0 && c.devanagari == 0 &&
                                                      (c.latinWords >= 2 || c.latinLetters >= 6));
            if (strongLatin)
                pushUnique(latinHint ? sourceHint : "en");
        }
    }
    return detected;
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
        auto detectedLangs = onlyGeometry ? std::vector<std::string>{} : DetectScriptLanguages(regions, mode);
        return {std::move(regions), geometryOnly || onlyGeometry ? "local-geometry-ai" : "bundled-ocr",
                size_t(unrecognizedLines), std::move(detectedLangs)};
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
    if (!IsLocalOcrLanguage(language))
        throw AppError("ocr", "OCR 文字翻译需要选择有效的本地识别语言");
    // The page shape decides the engine, and the detected boxes already describe it. A full general pass
    // recognizes every line three times over, so starting there costs seconds of work that a manga page
    // discards. Detection first keeps the spent time inside the engine that actually reads the page, and
    // the boxes travel on so the routed pass repeats no detector run.
    auto shapeLines = RecognizeBundled(image, language, stop, true);
    std::vector<RECT> detectedBoxes;
    detectedBoxes.reserve(shapeLines.size());
    for (const auto& line : shapeLines)
        detectedBoxes.push_back(line.rect);
    // Routing follows the measured page shape: true vertical columns (Height > 1.2 * Width) decide
    // the engine without a second detection budget. Explicit Japanese accepts a 30% vertical share;
    // local-auto requires stronger vertical evidence so square UI badges or short horizontal tokens
    // do not misroute into the Japanese-only manga recognizer.
    size_t vertical = 0;
    for (auto box : detectedBoxes)
        if (Height(box) * 5 > Width(box) * 6)
            ++vertical;
    const double share = detectedBoxes.empty() ? 0.0 : double(vertical) / double(detectedBoxes.size());
    const bool mangaCapable = language == OcrLocalAuto || language == OcrJapanese;
    const bool mangaCandidate =
        language == OcrJapanese
            ? (share >= .30)
            : (vertical >= 2 && share >= .40) ||
              (detectedBoxes.size() == 1 && vertical == 1 &&
               Height(detectedBoxes.front()) > Width(detectedBoxes.front()) * 2);
    if (detectedBoxes.empty() || !mangaCandidate || !mangaCapable || !MangaOcrAvailable()) {
        DiagnosticEvent("ocr_route", {{"engine", "general"},
                                      {"reason", detectedBoxes.empty() ? "no_regions"
                                                 : !mangaCandidate     ? "horizontal"
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
    double mangaConfidenceSum = 0.0;
    size_t mangaNonEmpty = 0;
    for (const auto& region : located.regions) {
        if (!region.text.empty()) {
            mangaConfidenceSum += region.confidence;
            ++mangaNonEmpty;
        }
    }
    const double mangaMeanConfidence = mangaNonEmpty ? mangaConfidenceSum / double(mangaNonEmpty) : 0.0;
    if (located.regions.empty() || (language == OcrLocalAuto && mangaMeanConfidence < .75)) {
        const char* fallbackReason = located.regions.empty() ? "manga_empty" : "manga_low_confidence";
        manga.End("ok", {{"regions", located.regions.size()}, {"mean_confidence", mangaMeanConfidence}});
        DiagnosticEvent("ocr_route", {{"engine", "general"},
                                      {"reason", fallbackReason},
                                      {"vertical_share", share},
                                      {"mean_confidence", mangaMeanConfidence}});
        return DetectLocal(image, stop, language, false, true, true, false, &detectedBoxes);
    }
    manga.End("ok", {{"regions", located.regions.size()}, {"mean_confidence", mangaMeanConfidence}});
    DiagnosticEvent("ocr_route", {{"engine", "manga"},
                                  {"reason", "vertical_columns"},
                                  {"vertical_share", share},
                                  {"regions", located.regions.size()}});
    return located;
}

} // namespace sat
