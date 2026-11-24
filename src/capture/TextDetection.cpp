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
    struct RowScan {
        int pixels{};
        int softPixels{};
        int maxRun{};
        int bridgedSpan{};
        int softDistSum{};
        int l{};
        int rr{};
        bool isRule{};
    };
    const int boxH = Height(rect);
    std::vector<RowScan> rows(static_cast<size_t>(boxH));
    for (int y = rect.top; y < rect.bottom; ++y) {
        auto& rs = rows[size_t(y - rect.top)];
        rs.l = rect.right;
        rs.rr = rect.left;
        int currentRun = 0;
        int bridgedRun = 0, bridgedGap = 0;
        const auto* rowPtr = &image.bgra[size_t(y) * image.width * 4];
        for (int x = rect.left; x < rect.right; ++x) {
            const auto& bg = chunkBg[size_t((x - rect.left) / chunkW)];
            const auto* p = &rowPtr[size_t(x) * 4];
            const int d = std::abs(int(p[0]) - bg.b) + std::abs(int(p[1]) - bg.g) + std::abs(int(p[2]) - bg.r);
            if (d > 36) {
                ++rs.softPixels;
                rs.softDistSum += d;
                bridgedRun += 1 + bridgedGap;
                bridgedGap = 0;
                rs.bridgedSpan = std::max(rs.bridgedSpan, bridgedRun);
            } else if (bridgedRun > 0 && bridgedGap < 6) {
                ++bridgedGap;
            } else {
                bridgedRun = 0;
                bridgedGap = 0;
            }
            if (d > 78) {
                ++rs.pixels;
                ++currentRun;
                rs.maxRun = std::max(rs.maxRun, currentRun);
                rs.l = std::min(rs.l, x);
                rs.rr = std::max(rs.rr, x + 1);
            } else {
                currentRun = 0;
            }
        }
        // Exclude solid horizontal rules, underlines, and dashed chart gridlines from glyph bounds.
        const bool solidRule = boxW >= 32 && rs.maxRun > std::max(24, int(boxW * .55));
        bool dashedRule = false;
        if (!solidRule && boxW >= 48 && rs.softPixels >= int(boxW * .35) &&
            rs.bridgedSpan >= int(boxW * .72) && rs.maxRun <= 14) {
            const int meanSoftDist = rs.softPixels > 0 ? rs.softDistSum / rs.softPixels : 255;
            int extPixels = 0;
            const auto& rightBg = chunkBg.back();
            const int rx1 = std::min(image.width, int(rect.right) + 24);
            for (int x = rect.right; x < rx1; ++x) {
                const auto* p = &rowPtr[size_t(x) * 4];
                if (std::abs(int(p[0]) - rightBg.b) + std::abs(int(p[1]) - rightBg.g) +
                        std::abs(int(p[2]) - rightBg.r) >
                    36)
                    ++extPixels;
            }
            const auto& leftBg = chunkBg.front();
            const int lx0 = std::max(0, int(rect.left) - 24);
            for (int x = lx0; x < rect.left; ++x) {
                const auto* p = &rowPtr[size_t(x) * 4];
                if (std::abs(int(p[0]) - leftBg.b) + std::abs(int(p[1]) - leftBg.g) +
                        std::abs(int(p[2]) - leftBg.r) >
                    36)
                    ++extPixels;
            }
            if (meanSoftDist < 125 || extPixels >= 8)
                dashedRule = true;
        }
        rs.isRule = solidRule || dashedRule;
    }
    std::vector<unsigned char> rowHasInk(static_cast<size_t>(boxH), 0);
    for (int ry = 0; ry < boxH; ++ry) {
        const auto& rs = rows[size_t(ry)];
        if (!rs.isRule && rs.pixels >= std::max(2, boxW / 500) && rs.pixels < boxW * .88)
            rowHasInk[size_t(ry)] = 1;
    }
    // Trim detached 1-2 px top/bottom horizontal gridline bands separated by >= 2 blank rows from
    // the main glyph body.
    if (boxH >= 8 && boxW >= 48) {
        int mainStart = -1, mainEnd = -1, mainLen = 0;
        for (int ry = 0; ry < boxH;) {
            if (!rowHasInk[size_t(ry)]) {
                ++ry;
                continue;
            }
            int end = ry + 1;
            while (end < boxH && (rowHasInk[size_t(end)] ||
                                  (end + 1 < boxH && rowHasInk[size_t(end + 1)])))
                ++end;
            if (end - ry > mainLen) {
                mainLen = end - ry;
                mainStart = ry;
                mainEnd = end;
            }
            ry = end;
        }
        if (mainLen >= 5) {
            for (int ry = 0; ry < boxH; ++ry) {
                if (!rowHasInk[size_t(ry)])
                    continue;
                const bool detachedTop = ry + 2 <= mainStart && ry <= 3;
                const bool detachedBottom = ry >= mainEnd + 2 && ry >= boxH - 4;
                if ((detachedTop || detachedBottom) && rows[size_t(ry)].bridgedSpan >= int(boxW * .55))
                    rowHasInk[size_t(ry)] = 0;
            }
        }
    }
    RECT ink{rect.right, rect.bottom, rect.left, rect.top};
    for (int ry = 0; ry < boxH; ++ry) {
        if (!rowHasInk[size_t(ry)])
            continue;
        const auto& rs = rows[size_t(ry)];
        const int y = rect.top + ry;
        ink.left = std::min<LONG>(ink.left, rs.l);
        ink.right = std::max<LONG>(ink.right, rs.rr);
        ink.top = std::min<LONG>(ink.top, y);
        ink.bottom = y + 1;
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
    const float minShare = (Height(rect) <= 30 && Height(ink) >= 5) ? .20f : .30f;
    if (heightShare > .92f || heightShare < minShare)
        return rect;
    return ink;
}
int CodePointCount(std::string_view text) {
    int count = 0;
    for (size_t i = 0; i < text.size();) {
        const char32_t cp = NextCodePoint(text, i);
        if (cp == U' ' || cp == U'\t' || cp == U'\r' || cp == U'\n' || cp == 0x3000 || IsCombiningMark(cp))
            continue;
        ++count;
    }
    return count;
}
bool HasCjkText(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const char32_t cp = NextCodePoint(text, i);
        if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
            (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF))
            return true;
    }
    return false;
}
bool HasJapaneseText(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const char32_t cp = NextCodePoint(text, i);
        if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
            (cp >= 0x4E00 && cp <= 0x9FFF))
            return true;
    }
    return false;
}
bool IsPureKana(std::string_view text) {
    int kana = 0;
    for (size_t i = 0; i < text.size();) {
        const char32_t cp = NextCodePoint(text, i);
        if (cp == U' ' || cp == U'\t' || cp == U'\r' || cp == U'\n' || cp == 0x3000)
            continue;
        if ((cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
            (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0xFF65 && cp <= 0xFF9F) || cp == 0x30FC) {
            ++kana;
            continue;
        }
        return false;
    }
    return kana > 0;
}
bool IsPunctOrSymbolOnly(std::string_view text) {
    int visible = 0;
    for (size_t i = 0; i < text.size();) {
        const char32_t cp = NextCodePoint(text, i);
        if (cp == U' ' || cp == U'\t' || cp == U'\r' || cp == U'\n' || cp == 0x3000 || IsCombiningMark(cp))
            continue;
        ++visible;
        const bool isLetterOrDigit =
            (cp >= U'0' && cp <= U'9') || (cp >= U'A' && cp <= U'Z') || (cp >= U'a' && cp <= U'z') ||
            (cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x0370 && cp <= 0x052F) ||
            (cp >= 0x0590 && cp <= 0x06FF) || (cp >= 0x0900 && cp <= 0x0E7F) ||
            (cp >= 0x1E00 && cp <= 0x1EFF) || (cp >= 0x3040 && cp <= 0x30FF) ||
            (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
            (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xFF10 && cp <= 0xFF19) ||
            (cp >= 0xFF21 && cp <= 0xFF3A) || (cp >= 0xFF41 && cp <= 0xFF5A) ||
            (cp >= 0xFF65 && cp <= 0xFF9F);
        if (isLetterOrDigit)
            return false;
    }
    return visible > 0;
}
bool IsAsciiOrPunctOnly(std::string_view text) {
    int visible = 0;
    for (size_t i = 0; i < text.size();) {
        const char32_t cp = NextCodePoint(text, i);
        if (cp == U' ' || cp == U'\t' || cp == U'\r' || cp == U'\n' || cp == 0x3000)
            continue;
        ++visible;
        if (cp >= 0x80 && !(cp >= 0x2000 && cp <= 0x206F) && !(cp >= 0x2190 && cp <= 0x27BF) &&
            !(cp >= 0x3000 && cp <= 0x303F))
            return false;
    }
    return visible > 0;
}
bool EndsCjkBreakPunct(std::string_view text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    constexpr std::string_view kBreakPunct[] = {"」", "』", "】", "）", "？", "！", "。", "、", "》", "〉"};
    for (auto p : kBreakPunct)
        if (text.size() >= p.size() && text.substr(text.size() - p.size()) == p)
            return true;
    return false;
}
bool StartsRarityPrefix(const std::string& text) {
    const auto wide = Wide(text);
    const auto first = wide.find_first_not_of(L" \t");
    if (first == std::wstring::npos)
        return false;
    if (wide[first] == L'★' || wide[first] == L'☆')
        return true;
    if ((wide[first] == L'・' || wide[first] == L'·' || wide[first] == L'•') && first + 1 < wide.size() &&
        (wide[first + 1] == L'★' || wide[first + 1] == L'☆'))
        return true;
    return false;
}
// A decorated line opens a passage of its own. Bullets, note markers and banner glyphs separate
// sections in UI copy and article pages, and merging across them flattens the page structure into a
// single block that no translation can lay out faithfully. Only unmistakable non-ASCII bullets and
// section markers qualify: ASCII markers also appear inside ordinary sentences.
bool StartsPassage(const std::string& text) {
    const auto wide = Wide(text);
    const auto first = wide.find_first_not_of(L" \t");
    if (first == std::wstring::npos)
        return false;
    return std::wstring_view(L"◆◇●○◎■□▲△▼▽▶▷★☆※・·•‣◦▪▫").find(wide[first]) != std::wstring_view::npos;
}
bool StartsListMarker(const std::string& text) {
    const auto wide = Wide(text);
    const auto first = wide.find_first_not_of(L" \t");
    if (first == std::wstring::npos)
        return false;
    const wchar_t ch = wide[first];
    if (std::wstring_view(L"・·•‣◦▪▫◆◇●○◎■□▲△▼▽▶▷★☆※").find(ch) != std::wstring_view::npos)
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
    const bool tallStrip = Height(rect) > Width(rect) * 3 / 2;
    const double innerShare = double(bins[mode]) / double(sampled);
    bool ringConfirmed = false;
    const LONG rLeft = std::max<LONG>(0, rect.left - 3);
    const LONG rTop = std::max<LONG>(0, rect.top - 3);
    const LONG rRight = std::min<LONG>(image.width, rect.right + 3);
    const LONG rBottom = std::min<LONG>(image.height, rect.bottom + 3);
    if (innerShare < (tallStrip ? .65 : .45)) {
        std::array<size_t, 4096> ringOnly{};
        size_t ringSampled = 0;
        for (LONG y = rTop; y < rBottom; ++y)
            for (LONG x = rLeft; x < rRight; x += stepX) {
                if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
                    continue;
                ++ringOnly[binOf(&image.bgra[(size_t(y) * image.width + x) * 4])];
                ++ringSampled;
            }
        if (ringSampled >= 12) {
            const int ringMode = int(std::max_element(ringOnly.begin(), ringOnly.end()) - ringOnly.begin());
            if (double(ringOnly[ringMode]) >= double(ringSampled) * .30) {
                // Prioritize the outer ring alone so dense outlined glyphs do not invert background.
                mode = ringMode;
                ringConfirmed = true;
            } else {
                for (size_t b = 0; b < 4096; ++b)
                    ringOnly[b] = bins[b] + ringOnly[b] * 2;
                mode = int(std::max_element(ringOnly.begin(), ringOnly.end()) - ringOnly.begin());
            }
        }
    }
    const int br = ((mode >> 8) & 15) * 16 + 8, bg = ((mode >> 4) & 15) * 16 + 8,
              bb = (mode & 15) * 16 + 8;
    long long bgSum[3]{};
    size_t bgCount = 0;
    struct FgSample {
        int dist;
        unsigned char r, g, b;
    };
    std::vector<FgSample> fg;
    fg.reserve(sampled / 2);
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
                fg.push_back({dist, p[2], p[1], p[0]});
            }
        }
    if (bgCount < sampled * .15 && ringConfirmed) {
        for (LONG y = rTop; y < rBottom; ++y)
            for (LONG x = rLeft; x < rRight; x += stepX) {
                if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
                    continue;
                const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                const int dist = std::abs(int(p[2]) - br) + std::abs(int(p[1]) - bg) + std::abs(int(p[0]) - bb);
                if (dist < 42) {
                    bgSum[0] += p[2];
                    bgSum[1] += p[1];
                    bgSum[2] += p[0];
                    ++bgCount;
                }
            }
    }
    const bool validBg = bgCount >= sampled * .15 || (ringConfirmed && bgCount >= 8);
    if (!validBg || fg.size() < 4 || fg.size() > sampled * .90)
        return result;
    result.bgR = int(bgSum[0] / bgCount);
    result.bgG = int(bgSum[1] / bgCount);
    result.bgB = int(bgSum[2] / bgCount);
    // Separate darker-than-bg and brighter-than-bg foreground candidates and pick the dominant
    // polarity so bimodal outlined glyphs (white fill + black outline on mid-gray) never cancel out.
    const int bgLum3 = result.bgR + result.bgG + result.bgB;
    std::vector<FgSample> darkFg, brightFg;
    for (const auto& s : fg) {
        if (int(s.r) + int(s.g) + int(s.b) < bgLum3)
            darkFg.push_back(s);
        else
            brightFg.push_back(s);
    }
    auto& domFg = (darkFg.size() >= 4 && darkFg.size() >= brightFg.size())
                      ? darkFg
                      : (brightFg.size() >= 4 ? brightFg : fg);
    // Measure ink from the strongest stroke cores (top 35% farthest from bg) so anti-aliasing and
    // dark-UI glow do not dilute true hue or lightness.
    const size_t topK = std::max<size_t>(4, (domFg.size() * 35 + 99) / 100);
    const size_t cut = domFg.size() - topK;
    std::nth_element(domFg.begin(), domFg.begin() + cut, domFg.end(),
                     [](const FgSample& a, const FgSample& b) { return a.dist < b.dist; });
    long long inkSum[3]{};
    for (size_t i = cut; i < domFg.size(); ++i) {
        inkSum[0] += domFg[i].r;
        inkSum[1] += domFg[i].g;
        inkSum[2] += domFg[i].b;
    }
    result.hasInk = true;
    result.inkR = int(inkSum[0] / topK);
    result.inkG = int(inkSum[1] / topK);
    result.inkB = int(inkSum[2] / topK);
    return result;
}
bool AppearanceMatches(const LineAppearance& a, const LineAppearance& b,
                       bool allowAchromaticBgGradient = false) {
    if (!a.hasInk || !b.hasInk)
        return true;
    const int dbR = a.bgR - b.bgR, dbG = a.bgG - b.bgG, dbB = a.bgB - b.bgB;
    const int bgDiff = std::abs(dbR) + std::abs(dbG) + std::abs(dbB);
    const int bgChromaA = std::max({a.bgR, a.bgG, a.bgB}) - std::min({a.bgR, a.bgG, a.bgB});
    const int bgChromaB = std::max({b.bgR, b.bgG, b.bgB}) - std::min({b.bgR, b.bgG, b.bgB});
    const int bgChromaDiff = std::max({std::abs(dbR - dbG), std::abs(dbG - dbB), std::abs(dbR - dbB)});
    const int maxBgDiff = (allowAchromaticBgGradient && std::max(bgChromaA, bgChromaB) <= 16 &&
                           bgChromaDiff <= 12)
                              ? 56
                              : 36;
    if (bgDiff > maxBgDiff)
        return false;
    const int dR = a.inkR - b.inkR, dG = a.inkG - b.inkG, dB = a.inkB - b.inkB;
    const int chromaDiff = std::max({std::abs(dR - dG), std::abs(dG - dB), std::abs(dR - dB)});
    if (chromaDiff > 32)
        return false;
    const int inkDiff = std::abs(dR) + std::abs(dG) + std::abs(dB);
    const int chromaA = std::max({a.inkR, a.inkG, a.inkB}) - std::min({a.inkR, a.inkG, a.inkB});
    const int chromaB = std::max({b.inkR, b.inkG, b.inkB}) - std::min({b.inkR, b.inkG, b.inkB});
    const int maxInkDiff = (chromaA <= 24 && chromaB <= 24 && bgDiff <= 18) ? 90 : 68;
    return inkDiff <= maxInkDiff;
}
bool HasSeparator(const Image& image, RECT a, RECT b, const LineAppearance& appA = {},
                  const LineAppearance& appB = {}) {
    const int left = std::max(a.left, b.left), right = std::min(a.right, b.right);
    const int span = right - left;
    if (span < 20 || b.top < a.bottom - 3)
        return false;
    if (b.top > a.bottom) {
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
    }
    // Detect dashed or subtle horizontal gridlines strictly inside the inter-line gap. Every dash
    // pixel on row y must itself contrast against the local background as well as rows above and
    // below, so text baselines at a.bottom - 1 never false-trigger when compared to blank gap rows.
    if (span >= 40 && b.top > a.bottom) {
        const int maxDashGap = std::max(6, span / 25);
        const LONG dStartY = std::max<LONG>(2, a.bottom + 1);
        const LONG dEndY = std::min<LONG>(image.height - 2, b.top);
        for (LONG y = dStartY; y < dEndY; ++y) {
            int dashEdges = 0, dashSegments = 0;
            int bridgedRun = 0, maxBridgedRun = 0, gap = 0;
            bool inDash = false;
            int minR = 255, maxR = 0, minG = 255, maxG = 0, minB = 255, maxB = 0;
            for (int x = left; x < right; ++x) {
                const auto p = (size_t(y) * image.width + x) * 4;
                if (appA.hasInk) {
                    const int dBg = std::abs(int(image.bgra[p + 2]) - appA.bgR) +
                                    std::abs(int(image.bgra[p + 1]) - appA.bgG) +
                                    std::abs(int(image.bgra[p + 0]) - appA.bgB);
                    if (dBg <= 18) {
                        inDash = false;
                        if (bridgedRun > 0 && gap < maxDashGap)
                            ++gap;
                        else
                            bridgedRun = gap = 0;
                        continue;
                    }
                }
                int up1 = 0, up2 = 0, dn1 = 0, dn2 = 0;
                for (int c = 0; c < 3; ++c) {
                    up1 += std::abs(int(image.bgra[p + c]) - int(image.bgra[p - image.width * 4 + c]));
                    if (y - 2 >= a.bottom)
                        up2 += std::abs(int(image.bgra[p + c]) - int(image.bgra[p - image.width * 8 + c]));
                    dn1 += std::abs(int(image.bgra[p + c]) - int(image.bgra[p + image.width * 4 + c]));
                    if (y + 2 < b.top)
                        dn2 += std::abs(int(image.bgra[p + c]) - int(image.bgra[p + image.width * 8 + c]));
                }
                if (std::max(up1, up2) > 20 && std::max(dn1, dn2) > 20) {
                    ++dashEdges;
                    if (!inDash) {
                        ++dashSegments;
                        inDash = true;
                    }
                    bridgedRun += 1 + gap;
                    gap = 0;
                    maxBridgedRun = std::max(maxBridgedRun, bridgedRun);
                    minR = std::min(minR, int(image.bgra[p + 2]));
                    maxR = std::max(maxR, int(image.bgra[p + 2]));
                    minG = std::min(minG, int(image.bgra[p + 1]));
                    maxG = std::max(maxG, int(image.bgra[p + 1]));
                    minB = std::min(minB, int(image.bgra[p + 0]));
                    maxB = std::max(maxB, int(image.bgra[p + 0]));
                } else {
                    inDash = false;
                    if (bridgedRun > 0 && gap < maxDashGap) {
                        ++gap;
                    } else {
                        bridgedRun = 0;
                        gap = 0;
                    }
                }
            }
            const int spread = (maxR - minR) + (maxG - minG) + (maxB - minB);
            if (dashEdges >= int(span * .35) && maxBridgedRun >= int(span * .72) && dashSegments >= 4 &&
                spread <= 110)
                return true;
        }
    }
    // Detect a contrasting container-gutter band (e.g. white gutter between tinted timeline pills,
    // or dark gutter between colored highlight bars).
    if (appA.hasInk && appB.hasInk && b.top >= a.bottom - 4) {
        const LONG gStartY =
            std::max<LONG>(0, std::max<LONG>(a.top + Height(a) * 2 / 3, std::min(a.bottom, b.top) - 2));
        const LONG gEndY = std::min<LONG>(
            image.height, std::min<LONG>(b.bottom - Height(b) * 2 / 3, std::max(a.bottom, b.top) + 2));
        for (LONG y = gStartY; y < gEndY; ++y) {
            int gutterCols = 0;
            long long gR = 0, gG = 0, gB = 0;
            for (int x = left; x < right; ++x) {
                const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                const int dA =
                    std::abs(int(p[2]) - appA.bgR) + std::abs(int(p[1]) - appA.bgG) + std::abs(int(p[0]) - appA.bgB);
                const int dB =
                    std::abs(int(p[2]) - appB.bgR) + std::abs(int(p[1]) - appB.bgG) + std::abs(int(p[0]) - appB.bgB);
                if (dA >= 28 && dB >= 28) {
                    ++gutterCols;
                    gR += p[2];
                    gG += p[1];
                    gB += p[0];
                }
            }
            if (gutterCols >= int(span * .70)) {
                const int mR = int(gR / gutterCols), mG = int(gG / gutterCols), mB = int(gB / gutterCols);
                int uniformCols = 0;
                for (int x = left; x < right; ++x) {
                    const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                    if (std::abs(int(p[2]) - mR) + std::abs(int(p[1]) - mG) + std::abs(int(p[0]) - mB) <= 36)
                        ++uniformCols;
                }
                if (uniformCols >= int(span * .65))
                    return true;
            }
        }
    }
    return false;
}
bool HasLeadingBullet(const Image& image, RECT rect, const LineAppearance& app) {
    if (!Valid(rect) || !app.hasInk)
        return false;
    const int h = Height(rect), w = Width(rect);
    if (h < 10 || w < h * 2)
        return false;
    auto isInk = [&](int x, int y) {
        const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
        return std::abs(int(p[2]) - app.bgR) + std::abs(int(p[1]) - app.bgG) + std::abs(int(p[0]) - app.bgB) >= 56;
    };
    const int searchEnd = std::min<int>(rect.right, rect.left + int(h * 2.2f));
    const int maxBulletDim = std::max(3, int(std::lround(h * .52f)));
    const int minGapAfter = std::max(5, int(std::lround(h * .42f)));
    int firstInkX = -1;
    for (int x = rect.left; x < searchEnd; ++x) {
        for (int y = rect.top; y < rect.bottom; ++y) {
            if (isInk(x, y)) {
                firstInkX = x;
                break;
            }
        }
        if (firstInkX >= 0)
            break;
    }
    if (firstInkX < 0 || firstInkX - rect.left > h)
        return false;
    int clusterEnd = firstInkX;
    int minY = rect.bottom, maxY = rect.top;
    for (int x = firstInkX; x < std::min<int>(rect.right, firstInkX + maxBulletDim + 2); ++x) {
        bool colHit = false;
        for (int y = rect.top; y < rect.bottom; ++y) {
            if (isInk(x, y)) {
                colHit = true;
                minY = std::min(minY, y);
                maxY = std::max(maxY, y + 1);
            }
        }
        if (!colHit)
            break;
        clusterEnd = x + 1;
    }
    const int bw = clusterEnd - firstInkX;
    const int bh = maxY - minY;
    if (bw < 2 || bw > maxBulletDim || bh < 2 || bh > maxBulletDim)
        return false;
    // Bullet dots sit vertically centered inside the line height, clear of top/bottom cap edges.
    if (minY < rect.top + int(h * .14f) || maxY > rect.bottom - int(h * .14f))
        return false;
    // Require a clean whitespace gap after the bullet followed by actual text ink.
    int nextInkX = -1;
    const int gapScanEnd = std::min<int>(rect.right, clusterEnd + std::max(minGapAfter + 12, int(h * 1.6f)));
    for (int x = clusterEnd; x < gapScanEnd; ++x) {
        for (int y = rect.top; y < rect.bottom; ++y) {
            if (isInk(x, y)) {
                nextInkX = x;
                break;
            }
        }
        if (nextInkX >= 0)
            break;
    }
    return nextInkX >= clusterEnd + minGapAfter;
}
bool HasRepeatingLeftMarker(const Image& image, RECT a, RECT b, const LineAppearance& app) {
    if (!app.hasInk || b.top <= a.bottom)
        return false;
    const int h = std::max(Height(a), Height(b));
    if (h < 8 || std::abs(a.left - b.left) > std::max(6, int(h * .4f)))
        return false;
    const int leftEdge = std::min(a.left, b.left);
    if (leftEdge < 8)
        return false;
    auto countStrip = [&](int x0, int x1, int y0, int y1, int thresh, int& totalOut) {
        x0 = std::max(0, x0);
        x1 = std::min(image.width, x1);
        y0 = std::max(0, y0);
        y1 = std::min(image.height, y1);
        totalOut = std::max(0, (x1 - x0) * (y1 - y0));
        int hits = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
                if (std::abs(int(p[2]) - app.bgR) + std::abs(int(p[1]) - app.bgG) + std::abs(int(p[0]) - app.bgB) >=
                    thresh)
                    ++hits;
            }
        return hits;
    };
    // 1. Immediate leader-line tips touching left edge of short callout labels (with blank inter-line gap).
    if (Width(a) <= int(h * 11.5f) && Width(b) <= int(h * 11.5f)) {
        const int tx0 = leftEdge - 4, tx1 = leftEdge - 1;
        int totA = 0, totB = 0, totG = 0;
        const int padA = std::max(1, Height(a) / 6), padB = std::max(1, Height(b) / 6);
        const int hitA = countStrip(tx0, tx1, a.top + padA, a.bottom - padA, 48, totA);
        const int hitB = countStrip(tx0, tx1, b.top + padB, b.bottom - padB, 48, totB);
        const int hitG = countStrip(tx0, tx1, a.bottom, b.top, 48, totG);
        if (hitA >= 2 && hitB >= 2 && totG >= 3 && hitG == 0)
            return true;
    }
    // 2. Legend icons / color swatches to the left of both rows separated by a clean vertical gap
    //    and a clean vertical background buffer between the swatch and the text.
    if (b.top - a.bottom >= 3) {
        const int iconW = std::min(46, std::max(20, int(h * 3)));
        const int ix0 = leftEdge - iconW, ix1 = leftEdge - 6;
        if (ix1 - ix0 >= 10) {
            int totA = 0, totB = 0, totG = 0, totBuf = 0;
            const int hitA = countStrip(ix0, ix1, a.top, a.bottom, 25, totA);
            const int hitB = countStrip(ix0, ix1, b.top, b.bottom, 25, totB);
            const int hitG = countStrip(ix0, ix1, a.bottom, b.top, 25, totG);
            const int hitBuf = countStrip(leftEdge - 5, leftEdge - 2, a.top, b.bottom, 32, totBuf);
            if (totA > 0 && totB > 0 && totG > 0 && totBuf > 0 && hitBuf <= totBuf / 8) {
                const double fracA = double(hitA) / totA;
                const double fracB = double(hitB) / totB;
                const double fracG = double(hitG) / totG;
                if (fracA >= .18 && fracB >= .18 && fracG <= .06)
                    return true;
            }
        }
    }
    return false;
}
bool HasVerticalSeparator(const Image& image, RECT leftRect, RECT rightRect, LONG scanStartX = -1,
                          LONG scanEndX = -1, const std::vector<RECT>* ignoreRects = nullptr) {
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
    auto isIgnored = [&](LONG x, LONG y) {
        if (!ignoreRects)
            return false;
        for (const RECT& r : *ignoreRects)
            if (x >= r.left - 1 && x < r.right + 1 && y >= r.top - 1 && y < r.bottom + 1)
                return true;
        return false;
    };
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
        int effSpan = 0;
        int minR = 255, maxR = 0, minG = 255, maxG = 0, minB = 255, maxB = 0;
        for (LONG y = top; y < bottom; ++y) {
            if (isIgnored(x, y))
                continue;
            ++effSpan;
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
        if (effSpan < 8)
            continue;
        int extTop = 0, extBottom = 0;
        for (LONG y = std::max<LONG>(0, top - 4); y < top; ++y)
            extTop += (!isIgnored(x, y) && edgeAt(x, y)) ? 1 : 0;
        for (LONG y = bottom; y < std::min<LONG>(image.height, bottom + 4); ++y)
            extBottom += (!isIgnored(x, y) && edgeAt(x, y)) ? 1 : 0;
        const bool continuesVertically = extTop >= 2 || extBottom >= 2;
        const bool uniformColor = (maxR - minR) + (maxG - minG) + (maxB - minB) <= 36;
        const bool uniformStroke = uniformColor && edges > effSpan * .55;
        const bool verticalEdge = edges > effSpan * .72 && (continuesVertically || effSpan >= 36);
        if (verticalEdge || uniformStroke)
            return true;
    }
    return false;
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
bool HasJaggedBubbleBorder(const Image& image, RECT leftRect, RECT rightRect, const LineAppearance& app,
                           const std::vector<RECT>* ignoreRects = nullptr) {
    const LONG x0 = leftRect.right + 1;
    const LONG x1 = rightRect.left - 1;
    if (x1 <= x0 || !app.hasInk)
        return false;
    const int bgLum = (app.bgR + app.bgG + app.bgB) / 3;
    if (bgLum < 170)
        return false;
    const LONG top = std::max<LONG>(0, std::max(leftRect.top, rightRect.top) - 6);
    const LONG bottom = std::min<LONG>(image.height, std::min(leftRect.bottom, rightRect.bottom) + 6);
    const int span = int(bottom - top);
    if (span < 12)
        return false;
    auto isIgnored = [&](LONG x, LONG y) {
        if (!ignoreRects)
            return false;
        for (const RECT& r : *ignoreRects)
            if (x >= r.left - 1 && x < r.right + 1 && y >= r.top - 1 && y < r.bottom + 1)
                return true;
        return false;
    };
    int darkRows = 0;
    for (LONG y = top; y < bottom; ++y) {
        const auto* row = &image.bgra[size_t(y) * image.width * 4];
        bool rowHit = false;
        for (LONG x = x0; x < x1; ++x) {
            if (isIgnored(x, y))
                continue;
            const auto* p = &row[size_t(x) * 4];
            const int d =
                std::abs(int(p[2]) - app.bgR) + std::abs(int(p[1]) - app.bgG) + std::abs(int(p[0]) - app.bgB);
            if (d >= 90) {
                rowHit = true;
                break;
            }
        }
        if (rowHit)
            ++darkRows;
    }
    return darkRows * 100 >= span * 22;
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
bool IsGraphicFalsePositive(const TextLine& l) {
    const int cpCount = CodePointCount(l.text);
    const int w = Width(l.rect), h = Height(l.rect);
    const int maxDim = std::max(w, h);
    const bool asciiOrPunct = IsAsciiOrPunctOnly(l.text);
    const bool punctOnly = IsPunctOrSymbolOnly(l.text);
    if (cpCount == 0)
        return true;
    // Low-confidence single-character graphic/cursor/photo false positives.
    if (cpCount <= 1 && l.confidence < .52f)
        return true;
    // Tiny chart dots, circles, or axis tick marks misread as single ASCII/punct glyphs.
    if (maxDim <= 16 && cpCount <= 1 && asciiOrPunct && l.confidence < .80f)
        return true;
    // Clipped scroll slivers / sub-8px strips with low confidence.
    if (h <= 8 && l.confidence < .65f)
        return true;
    // Very low-confidence short photo/icon noise (preserves real manga/UI lines >= 0.30).
    if (l.confidence < .30f && cpCount <= 3)
        return true;
    // Isolated 1-2 glyph symbol/punctuation icons (arrows, chevrons, bullets, dashes).
    if (punctOnly && cpCount <= 2 && w <= int(h * 2.5f) && l.confidence < .72f)
        return true;
    // Oversized rectangular photo patches misread as a single ASCII glyph, or studio-light blobs
    // misread as 1-3 short ASCII letters.
    if (h >= 48 && cpCount <= 1 && asciiOrPunct && w > int(h * 1.6f) && l.confidence < .80f)
        return true;
    if (h >= 36 && w >= 36 && h >= (w * 3) / 4 && cpCount <= 3 && asciiOrPunct && l.confidence < .52f)
        return true;
    return false;
}
void OrderMangaRegions(std::vector<TextRegion>& regions, const Image& image) {
    if (regions.size() < 2)
        return;
    const bool hasPixels = image.bgra.size() == size_t(image.width) * image.height * 4;
    auto isWhitePx = [&](int x, int y) {
        const auto* p = &image.bgra[(size_t(y) * image.width + x) * 4];
        return p[0] >= 228 && p[1] >= 228 && p[2] >= 228;
    };
    auto hasHorizontalGutter = [&](LONG y0, LONG y1, LONG x0, LONG x1) {
        if (!hasPixels || y1 - y0 < 3 || x1 - x0 < 20)
            return false;
        const int step = std::max(1, int(x1 - x0) / 120);
        int whiteStreak = 0;
        for (LONG y = std::max<LONG>(0, y0); y < std::min<LONG>(image.height, y1); ++y) {
            int total = 0, white = 0;
            for (LONG x = std::max<LONG>(0, x0); x < std::min<LONG>(image.width, x1); x += step) {
                ++total;
                if (isWhitePx(int(x), int(y)))
                    ++white;
            }
            if (total > 0 && white * 100 >= total * 85) {
                if (++whiteStreak >= 3)
                    return true;
            } else {
                whiteStreak = 0;
            }
        }
        return false;
    };
    auto hasVerticalGutter = [&](LONG x0, LONG x1, LONG y0, LONG y1) {
        if (!hasPixels || x1 - x0 < 3 || y1 - y0 < 20)
            return false;
        const int step = std::max(1, int(y1 - y0) / 120);
        int whiteStreak = 0;
        for (LONG x = std::max<LONG>(0, x0); x < std::min<LONG>(image.width, x1); ++x) {
            int total = 0, white = 0;
            for (LONG y = std::max<LONG>(0, y0); y < std::min<LONG>(image.height, y1); y += step) {
                ++total;
                if (isWhitePx(int(x), int(y)))
                    ++white;
            }
            if (total > 0 && white * 100 >= total * 85) {
                if (++whiteStreak >= 3)
                    return true;
            } else {
                whiteStreak = 0;
            }
        }
        return false;
    };
    auto orderRec = [&](auto& self, std::vector<size_t> idxs) -> std::vector<size_t> {
        if (idxs.size() <= 1)
            return idxs;
        RECT box = regions[idxs[0]].rect;
        for (size_t k = 1; k < idxs.size(); ++k)
            UnionRect(&box, &box, &regions[idxs[k]].rect);
        // 1. Prefer splitting across a continuous full-width horizontal panel gutter (top to bottom).
        {
            std::vector<size_t> byTop = idxs;
            std::stable_sort(byTop.begin(), byTop.end(), [&](size_t a, size_t b) {
                return regions[a].rect.top < regions[b].rect.top;
            });
            LONG prefixBottom = regions[byTop[0]].rect.bottom;
            size_t bestSplit = 0;
            LONG bestGap = -1;
            for (size_t k = 1; k < byTop.size(); ++k) {
                const LONG gap = regions[byTop[k]].rect.top - prefixBottom;
                if (gap >= 8 && hasHorizontalGutter(prefixBottom, regions[byTop[k]].rect.top, box.left, box.right) &&
                    gap > bestGap) {
                    bestGap = gap;
                    bestSplit = k;
                }
                prefixBottom = std::max(prefixBottom, regions[byTop[k]].rect.bottom);
            }
            if (bestSplit > 0) {
                auto topHalf = self(self, std::vector<size_t>(byTop.begin(), byTop.begin() + bestSplit));
                auto botHalf = self(self, std::vector<size_t>(byTop.begin() + bestSplit, byTop.end()));
                topHalf.insert(topHalf.end(), botHalf.begin(), botHalf.end());
                return topHalf;
            }
        }
        // 2. Otherwise split across a continuous full-height vertical panel gutter (right to left).
        {
            std::vector<size_t> byLeft = idxs;
            std::stable_sort(byLeft.begin(), byLeft.end(), [&](size_t a, size_t b) {
                return regions[a].rect.left < regions[b].rect.left;
            });
            LONG prefixRight = regions[byLeft[0]].rect.right;
            size_t bestSplit = 0;
            LONG bestGap = -1;
            for (size_t k = 1; k < byLeft.size(); ++k) {
                const LONG gap = regions[byLeft[k]].rect.left - prefixRight;
                if (gap >= 6 && hasVerticalGutter(prefixRight, regions[byLeft[k]].rect.left, box.top, box.bottom) &&
                    gap > bestGap) {
                    bestGap = gap;
                    bestSplit = k;
                }
                prefixRight = std::max(prefixRight, regions[byLeft[k]].rect.right);
            }
            if (bestSplit > 0) {
                auto rightHalf = self(self, std::vector<size_t>(byLeft.begin() + bestSplit, byLeft.end()));
                auto leftHalf = self(self, std::vector<size_t>(byLeft.begin(), byLeft.begin() + bestSplit));
                rightHalf.insert(rightHalf.end(), leftHalf.begin(), leftHalf.end());
                return rightHalf;
            }
        }
        // 3. Within a single panel, group vertically stacked bubbles that share a column track,
        //    order tracks right-to-left, and order bubbles within each track top-to-bottom.
        std::stable_sort(idxs.begin(), idxs.end(), [&](size_t a, size_t b) {
            const auto& ra = regions[a].rect;
            const auto& rb = regions[b].rect;
            const LONG ca = ra.left + ra.right, cb = rb.left + rb.right;
            return ca == cb ? ra.top < rb.top : ca > cb;
        });
        struct Track {
            std::vector<size_t> members;
            RECT bounds{};
        };
        std::vector<Track> tracks;
        for (size_t id : idxs) {
            const RECT& r = regions[id].rect;
            size_t match = tracks.size();
            for (size_t t = 0; t < tracks.size(); ++t) {
                const RECT& tb = tracks[t].bounds;
                const LONG hOverlap = std::min(r.right, tb.right) - std::max(r.left, tb.left);
                const LONG minW = std::max<LONG>(1, std::min(Width(r), Width(tb)));
                bool vDisjoint = true;
                for (size_t mid : tracks[t].members) {
                    const RECT& mr = regions[mid].rect;
                    const LONG vOv = std::min(r.bottom, mr.bottom) - std::max(r.top, mr.top);
                    if (vOv > std::min(Height(r), Height(mr)) * .25f) {
                        vDisjoint = false;
                        break;
                    }
                }
                if (vDisjoint && hOverlap * 100 >= minW * 35) {
                    match = t;
                    break;
                }
            }
            if (match == tracks.size()) {
                tracks.push_back({{id}, r});
            } else {
                tracks[match].members.push_back(id);
                UnionRect(&tracks[match].bounds, &tracks[match].bounds, &r);
            }
        }
        std::stable_sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) {
            const LONG ca = a.bounds.left + a.bounds.right;
            const LONG cb = b.bounds.left + b.bounds.right;
            return ca == cb ? a.bounds.top < b.bounds.top : ca > cb;
        });
        std::vector<size_t> ordered;
        ordered.reserve(idxs.size());
        for (auto& t : tracks) {
            std::stable_sort(t.members.begin(), t.members.end(), [&](size_t a, size_t b) {
                return regions[a].rect.top < regions[b].rect.top;
            });
            ordered.insert(ordered.end(), t.members.begin(), t.members.end());
        }
        return ordered;
    };
    std::vector<size_t> all(regions.size());
    for (size_t i = 0; i < regions.size(); ++i)
        all[i] = i;
    const auto perm = orderRec(orderRec, std::move(all));
    std::vector<TextRegion> reordered;
    reordered.reserve(regions.size());
    for (size_t idx : perm)
        reordered.push_back(std::move(regions[idx]));
    regions = std::move(reordered);
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
    auto pruneRedundant = [](std::vector<TextLine>& items, float keepConf, float dropConf) {
        for (size_t i = 0; i < items.size();) {
            bool redundant = false;
            for (size_t j = 0; j < items.size(); ++j) {
                if (i == j || items[j].confidence < keepConf || items[i].confidence >= dropConf ||
                    items[i].confidence > items[j].confidence)
                    continue;
                RECT overlap{};
                const auto a = items[i].rect, b = items[j].rect;
                const bool unread = IsUnreadText(items[i].text);
                if (IntersectRect(&overlap, &a, &b) &&
                    Width(overlap) * Height(overlap) >= (unread ? .45f : .65f) * Width(a) * Height(a) &&
                    Width(a) < Width(b) * .4f) {
                    redundant = true;
                    break;
                }
            }
            if (redundant)
                items.erase(items.begin() + i);
            else
                ++i;
        }
    };
    pruneRedundant(lines, .65f, .65f);
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
    auto groupText = [](const LineGroup& g) {
        std::string s;
        for (size_t k = 0; k < g.parts.size(); ++k) {
            if (k > 0)
                s += ' ';
            s += g.parts[k].text;
        }
        return s;
    };
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
        const bool tightHeight = maxH <= LONG(std::lround(minH * 1.35f));
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
        const bool aFirst = a.left <= b.left;
        const auto& leftG = aFirst ? ga : gb;
        const auto& rightG = aFirst ? gb : ga;
        const RECT leftRect = leftG.rect;
        const RECT rightRect = rightG.rect;
        const auto leftInk = leftG.inkSpan;
        const auto rightInk = rightG.inkSpan;
        const LONG gap = rightRect.left - leftRect.right;
        const LONG inkGap = rightInk.first - leftInk.second;
        const LONG refH = tightHeight ? std::max(minH, (minH + maxH) / 2)
                                      : LONG(std::lround((minH + maxH) * .5f));
        const bool sameLineInlineSpan =
            vOverlap >= minH * .70f && gap <= LONG(std::lround(refH * 1.05f)) &&
            (Width(leftRect) >= refH * 8 || Width(rightRect) >= refH * 8);
        if (!AppearanceMatches(ga.app, gb.app)) {
            if (!sameLineInlineSpan || !ga.app.hasInk || !gb.app.hasInk)
                return false;
            const int bgDiff = std::abs(ga.app.bgR - gb.app.bgR) + std::abs(ga.app.bgG - gb.app.bgG) +
                               std::abs(ga.app.bgB - gb.app.bgB);
            const int dR = ga.app.inkR - gb.app.inkR, dG = ga.app.inkG - gb.app.inkG,
                      dB = ga.app.inkB - gb.app.inkB;
            const int inkDiff = std::abs(dR) + std::abs(dG) + std::abs(dB);
            const int chromaDiff = std::max({std::abs(dR - dG), std::abs(dG - dB), std::abs(dR - dB)});
            if (bgDiff > 36 || inkDiff > 240 || chromaDiff > 45)
                return false;
        }
        const std::string leftTxt = groupText(leftG);
        const std::string rightTxt = groupText(rightG);
        // Do not fuse a tiny low-confidence graphic/icon onto a clean adjacent text line.
        if (!allUnread) {
            auto isLowConfIcon = [](const LineGroup& g, const std::string& txt) {
                return g.confidence < .52f && CodePointCount(txt) <= 1 &&
                       Width(g.rect) <= int(Height(g.rect) * 1.6f);
            };
            if ((isLowConfIcon(leftG, leftTxt) && rightG.confidence >= .75f) ||
                (isLowConfIcon(rightG, rightTxt) && leftG.confidence >= .75f))
                return false;
        }
        const bool leftCjk = HasCjkText(leftTxt);
        const bool rightCjk = HasCjkText(rightTxt);
        const bool anyCjk = leftCjk || rightCjk;
        const bool cjkWideJoin =
            anyCjk && (EndsCjkBreakPunct(leftTxt) || StartsRarityPrefix(leftTxt) ||
                       (leftCjk && rightCjk && CodePointCount(leftTxt) == 1));
        int sameRowTabs = 0;
        if (refH <= 22 && !cjkWideJoin) {
            const LONG rowCenterY = (centreA + centreB) / 2;
            const LONG nearLeft = leftRect.left - refH * 24;
            const LONG nearRight = rightRect.right + refH * 24;
            for (const auto& og : groups) {
                const LONG oh = Height(og.rect);
                if (oh < refH * .55f || oh > refH * 1.65f || Width(og.rect) > refH * 12)
                    continue;
                if (og.rect.right < nearLeft || og.rect.left > nearRight)
                    continue;
                const LONG ocy = (og.rect.top + og.rect.bottom) / 2;
                if (std::abs(ocy - rowCenterY) <= std::max<LONG>(4, (refH * 3) / 5))
                    ++sameRowTabs;
            }
        }
        const bool isMultiTabRow = !cjkWideJoin && sameRowTabs >= 4;
        LONG maxInkGap{};
        LONG maxBoxGap{};
        if (cjkWideJoin) {
            maxInkGap = std::max<LONG>(10, LONG(std::lround(refH * 1.55f)));
            maxBoxGap = std::max<LONG>(6, LONG(std::lround(refH * .92f)));
        } else if (isMultiTabRow) {
            // Horizontal navigation bars and footer link strips carry 4+ short tabs on the same row.
            maxInkGap = std::max<LONG>(6, LONG(std::lround(refH * .80f)));
            maxBoxGap = std::max<LONG>(3, LONG(std::lround(refH * .48f)));
        } else if (anyCjk) {
            maxInkGap = std::max<LONG>(8, LONG(std::lround(refH * (tightHeight ? 1.22f : 1.00f))));
            maxBoxGap = std::max<LONG>(4, LONG(std::lround(refH * (tightHeight ? .78f : .72f))));
        } else {
            maxInkGap = std::max<LONG>(8, LONG(std::lround(refH * (tightHeight ? 1.15f : .95f))));
            maxBoxGap = std::max<LONG>(4, LONG(std::lround(refH * (tightHeight ? .68f : .72f))));
        }
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
    // Re-run redundant nested-box cleanup after horizontal merging (catches inner fragments inside
    // newly assembled multi-word lines) and drop non-text graphic/icon false positives.
    pruneRedundant(lines, .55f, .60f);
    if (!allUnread) {
        std::erase_if(lines, [](const TextLine& l) {
            return !IsUnreadText(l.text) && IsGraphicFalsePositive(l);
        });
    }
    // Reading order follows the page shape. A page of vertical columns reads right to left, each
    // column top to bottom; a horizontal page reads top to bottom, left to right.
    const bool columnPage =
        std::count_if(lines.begin(), lines.end(),
                      [](const auto& line) { return Height(line.rect) > Width(line.rect); }) *
            2 >
        int(lines.size());
    // Suppress vertical furigana (ruby) strips sitting immediately to the right of a wider main
    // Japanese vertical column before vertical balloon merging, and retain their bounds so intra-bubble
    // furigana ink is never mistaken for a bubble divider.
    std::vector<RECT> rubyRects;
    if (columnPage && !allUnread) {
        std::vector<unsigned char> isRuby(lines.size(), 0);
        for (size_t i = 0; i < lines.size(); ++i) {
            const RECT r = lines[i].rect;
            if (!IsPureKana(lines[i].text) || Height(r) < Width(r) * .9f)
                continue;
            for (size_t j = 0; j < lines.size(); ++j) {
                if (i == j || isRuby[j])
                    continue;
                const RECT m = lines[j].rect;
                if (Width(m) < 16 || Height(m) < Width(m) * .85f)
                    continue;
                if (Width(r) > Width(m) * .64f || Height(r) > Height(m) * .95f + 4)
                    continue;
                const LONG vOv = std::min(r.bottom, m.bottom) - std::max(r.top, m.top);
                if (vOv < Height(r) * .70f)
                    continue;
                const LONG midM = (m.left + m.right) / 2;
                if (r.left >= midM && r.right >= m.right - 2 && r.left - m.right <= LONG(Width(m) * .45f)) {
                    isRuby[i] = 1;
                    break;
                }
            }
        }
        std::vector<TextLine> kept;
        kept.reserve(lines.size());
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!isRuby[i])
                kept.push_back(std::move(lines[i]));
            else
                rubyRects.push_back(lines[i].rect);
        }
        lines = std::move(kept);
    }
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
    std::vector<unsigned char> hasBullet(lines.size(), 0);
    for (size_t i = 0; i < lines.size(); ++i) {
        appearances[i] = MeasureLineAppearance(image, lines[i].rect);
        if (!columnPage && !IsUnreadText(lines[i].text))
            hasBullet[i] = HasLeadingBullet(image, lines[i].rect, appearances[i]) ? 1 : 0;
    }
    auto hasDistinctLeftSiblings = [&](RECT a, RECT b, float refH) {
        const LONG maxLeftGap = std::max<LONG>(48, LONG(std::lround(refH * 3.5f)));
        const LONG maxSibWidth = std::max<LONG>(72, LONG(std::lround(refH * 5.0f)));
        const LONG minLeft = std::min(a.left, b.left);
        std::vector<RECT> sibsA, sibsB;
        for (const auto& o : lines) {
            if (EqualRect(&o.rect, &a) || EqualRect(&o.rect, &b))
                continue;
            if (!allUnread && o.confidence < .50f)
                continue;
            if (Width(o.rect) > maxSibWidth)
                continue;
            if (o.rect.right > minLeft + 6 || minLeft - o.rect.right > maxLeftGap)
                continue;
            const LONG ovA = std::min(a.bottom, o.rect.bottom) - std::max(a.top, o.rect.top);
            const LONG ovB = std::min(b.bottom, o.rect.bottom) - std::max(b.top, o.rect.top);
            const LONG minHA = std::max<LONG>(1, std::min(Height(a), Height(o.rect)));
            const LONG minHB = std::max<LONG>(1, std::min(Height(b), Height(o.rect)));
            const float centerO = (o.rect.top + o.rect.bottom) * .5f;
            const float centerA = (a.top + a.bottom) * .5f;
            const float centerB = (b.top + b.bottom) * .5f;
            if ((ovA >= minHA * .25f && ovB <= minHB * .15f) ||
                (std::abs(centerO - centerA) <= refH * .70f && std::abs(centerO - centerB) >= refH * .85f))
                sibsA.push_back(o.rect);
            else if ((ovB >= minHB * .25f && ovA <= minHA * .15f) ||
                     (std::abs(centerO - centerB) <= refH * .70f && std::abs(centerO - centerA) >= refH * .85f))
                sibsB.push_back(o.rect);
        }
        const LONG alignTol = std::max<LONG>(16, LONG(std::lround(refH * 1.5f)));
        for (const RECT& sa : sibsA)
            for (const RECT& sb : sibsB)
                if (std::abs(sa.left - sb.left) <= alignTol || std::abs(sa.right - sb.right) <= alignTol)
                    return true;
        return false;
    };
    std::vector<TextRegion> regions;
    std::vector<LineAppearance> regionAppearances;
    std::vector<unsigned char> regionHadLeadingBullet;
    for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const auto& line = lines[lineIndex];
        const auto& lineApp = appearances[lineIndex];
        const bool lineUnread = IsUnreadText(line.text);
        size_t bestIndex = regions.size();
        int bestGap = INT_MAX;
        // A decorated or visually bulleted line starts a new passage; it never continues the block above it.
        const bool opensPassage = StartsPassage(line.text) || hasBullet[lineIndex] != 0;
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
            const float rawMaxH = std::max(h, float(Height(line.rect)));
            const float topPitch = float(line.rect.top - last.top);
            // Expand reference height to rawMaxH only when top-to-top pitch matches single-spaced
            // continuation (recovering descender-free lines without merging separate timestamps/tags).
            const float refH = (topPitch <= rawMaxH * 1.62f) ? rawMaxH : h;
            const float centerPitch =
                float((line.rect.top + line.rect.bottom) - (last.top + last.bottom)) * .5f;
            const int overlap = std::min(last.right, line.rect.right) - std::max(last.left, line.rect.left);
            const bool sentenceEnded = EndsSentence(region.text);
            const bool rtlRegion = HasRtlScript(region.text) || HasRtlScript(line.text);
            const bool hangingBullet =
                !columnPage && region.lines.size() == 1 && !rtlRegion &&
                (StartsListMarker(region.text) || StartsPassage(region.text) ||
                 regionHadLeadingBullet[regionIndex] != 0) &&
                line.rect.left >= last.left && line.rect.left - last.left <= refH * 1.85f &&
                Width(last) >= Width(line.rect) * .72f;
            const bool tightLeftContinuation =
                !columnPage && !sentenceEnded &&
                std::abs(line.rect.left - last.left) <= std::max<LONG>(6, LONG(std::lround(refH * .35f))) &&
                Width(last) >= Width(line.rect) * .72f;
            const bool firstLineIndent =
                !columnPage && region.lines.size() == 1 && !sentenceEnded &&
                (rtlRegion ? (line.rect.right > last.right && line.rect.right - last.right <= refH * 1.45f)
                           : (last.left > line.rect.left && last.left - line.rect.left <= refH * 1.45f)) &&
                Width(last) >= refH * 8.f &&
                (Width(line.rect) >= refH * 8.f ||
                 std::abs(rtlRegion ? (last.left - line.rect.left) : (last.right - line.rect.right)) <=
                     refH * 1.8f);
            const bool wideProseContinuation =
                !columnPage && !sentenceEnded && (tightLeftContinuation || firstLineIndent) &&
                Width(last) >= refH * 18.f && centerPitch <= refH * 1.90f;
            const float maxGap = columnPage
                                     ? refH * .9f
                                     : (wideProseContinuation
                                            ? refH * .90f
                                            : ((tightLeftContinuation || hangingBullet) ? refH * .68f
                                                                                        : refH * .55f));
            const float minRatio = columnPage ? .65f : (sentenceEnded ? .80f : .55f);
            const float maxRatio = columnPage ? 1.45f : ((tightLeftContinuation || hangingBullet ||
                                                          wideProseContinuation)
                                                             ? 1.45f
                                                             : 1.24f);
            if (gap < -refH * .25f || gap > maxGap || gap >= bestGap || Height(line.rect) < h * minRatio ||
                Height(line.rect) > h * maxRatio)
                continue;
            const LONG centerLast = (last.left + last.right) / 2;
            const LONG centerLine = (line.rect.left + line.rect.right) / 2;
            const bool centerAligned =
                std::abs(centerLast - centerLine) <= std::max<LONG>(6, LONG(std::lround(refH * .75f)));
            const bool alignedHorizontally =
                std::abs(line.rect.left - last.left) <= refH * 2 ||
                std::abs(line.rect.right - last.right) <= refH || centerAligned;
            if (!alignedHorizontally ||
                overlap < (centerAligned ? .50f : .65f) * std::min(Width(last), Width(line.rect)))
                continue;
            if (!columnPage) {
                if (!AppearanceMatches(regionAppearances[regionIndex], lineApp))
                    continue;
                const bool iconShiftedHeadline =
                    region.lines.size() == 1 && !sentenceEnded && !rtlRegion && gap <= refH * .40f &&
                    line.rect.left > last.left && line.rect.left - last.left <= refH * 1.65f &&
                    Width(last) >= refH * 8.f && Width(line.rect) >= refH * 6.f &&
                    overlap >= Width(line.rect) * .85f;
                const float maxIndentShift =
                    hangingBullet ? refH * 1.85f : (iconShiftedHeadline ? refH * 1.65f : refH * 1.1f);
                // An indented next line or a short preceding heading marks a paragraph boundary, unless
                // the lines are center-aligned inside the same speech bubble, hanging-indent bullets, or
                // a 1-em first-line-indented prose paragraph.
                if (!centerAligned &&
                    (rtlRegion ? (last.right - line.rect.right > maxIndentShift ||
                                  (region.lines.size() >= 2 && line.rect.right - last.right > refH * 1.1f))
                               : (line.rect.left - last.left > maxIndentShift ||
                                  (region.lines.size() >= 2 && last.left - line.rect.left > refH * 1.1f))))
                    continue;
                if (Width(line.rect) > refH * 4 && Width(last) < Width(line.rect) * .72f &&
                    (!centerAligned || sentenceEnded || gap > refH * .45f))
                    continue;
                // A truly short trailing line in a multi-line region marks paragraph end. Do not split
                // normal ragged-right wrapped prose lines before a sentence ends.
                if (region.lines.size() >= 2 && !centerAligned) {
                    const LONG trailMargin =
                        rtlRegion ? (last.left - region.rect.left) : (region.rect.right - last.right);
                    const bool shortBreak =
                        sentenceEnded
                            ? (trailMargin > LONG(refH * 1.2f))
                            : (Width(last) < Width(region.rect) * .62f && trailMargin > LONG(refH * 3.5f));
                    if (shortBreak)
                        continue;
                }
                // A sentence-ending line that stops noticeably short of the paragraph width ends the block,
                // and a short standalone heading below a completed full-width sentence starts a new block.
                if (sentenceEnded &&
                    (StartsListMarker(line.text) ||
                     Width(last) < std::max(Width(region.rect), Width(line.rect)) * .88f ||
                     (Width(line.rect) < Width(last) * .45f && !EndsSentence(line.text))))
                    continue;
                // A short centered title above a noticeably smaller centered subtitle remains separate.
                if (centerAligned && Width(last) < refH * 9.f && Width(line.rect) < refH * 9.f &&
                    std::max(Height(last), Height(line.rect)) >
                        LONG(std::lround(std::min(Height(last), Height(line.rect)) * 1.18f)))
                    continue;
                // Short stacked UI labels/menu items with noticeable vertical spacing remain separate.
                if (Width(last) < refH * 5.5f && Width(line.rect) < refH * 5.5f && gap > refH * .32f)
                    continue;
                if (region.lines.size() >= 2) {
                    std::vector<int> intraGaps;
                    intraGaps.reserve(region.lines.size() - 1);
                    for (size_t k = 1; k < region.lines.size(); ++k)
                        intraGaps.push_back(std::max(0, int(region.lines[k].top - region.lines[k - 1].bottom)));
                    std::sort(intraGaps.begin(), intraGaps.end());
                    const int medianIntra = intraGaps[intraGaps.size() / 2];
                    if (gap > std::max(refH * .45f, float(medianIntra) * 1.45f + 2.f))
                        continue;
                }
                if (hasDistinctLeftSiblings(last, line.rect, refH) ||
                    HasRepeatingLeftMarker(image, last, line.rect, regionAppearances[regionIndex]))
                    continue;
            }
            if (region.text.size() + line.text.size() + 1 > 8192 ||
                HasSeparator(image, last, line.rect, regionAppearances[regionIndex], lineApp))
                continue;
            RECT joined{};
            UnionRect(&joined, &region.rect, &line.rect);
            bool crosses = false;
            for (const auto& other : lines) {
                if (&other == &line || std::any_of(region.lines.begin(), region.lines.end(),
                                                   [&](RECT r) { return EqualRect(&r, &other.rect); }))
                    continue;
                // Tiny low-confidence inline icons (e.g. trailing paywall padlocks or arrows) sitting
                // beside a shorter line must not shatter multi-line headlines.
                const int oCp = CodePointCount(other.text);
                if (!allUnread && oCp <= 1 && other.confidence < .82f &&
                    Width(other.rect) <= int(Height(other.rect) * 1.65f))
                    continue;
                RECT intersection{};
                if (IntersectRect(&intersection, &joined, &other.rect) &&
                    Height(intersection) > std::min(Height(other.rect), Height(line.rect)) * .25f) {
                    RECT hitReg{}, hitLine{};
                    const bool hitsExisting =
                        IntersectRect(&hitReg, &region.rect, &other.rect) ||
                        IntersectRect(&hitLine, &line.rect, &other.rect) ||
                        (other.rect.bottom > last.bottom && other.rect.top < line.rect.top);
                    if (!hitsExisting && oCp <= 2 &&
                        Width(other.rect) <= int(Height(other.rect) * 2.0f))
                        continue;
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
            regionHadLeadingBullet.push_back(hasBullet[lineIndex]);
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
        const auto isOpeningColumn = [&](const TextRegion& region, const TextRegion& next) {
            if (strips(region))
                return true;
            if (region.lines.size() != 1 || !strips(next))
                return false;
            const RECT line = region.lines.front();
            return Width(line) > 0 && Height(line) >= Width(line) * .85f && HasJapaneseText(region.text);
        };
        const auto isColumnTail = [&](const TextRegion& region, float stripW) {
            if (strips(region))
                return true;
            if (region.lines.size() != 1)
                return false;
            const RECT line = region.lines.front();
            const float minWidthFactor = IsPunctOrSymbolOnly(region.text) ? .35f : .55f;
            return Width(line) >= stripW * minWidthFactor && Width(line) <= stripW * 1.45f &&
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
                const float minWidthFactor =
                    (join.lines.size() == 1 && IsPunctOrSymbolOnly(join.text)) ? .35f : .55f;
                // Neighbouring strips of one balloon share most of their glyph run and sit tightly
                // on the same bubble surface. Separate balloons keep a bubble outline, a different
                // surface, a wider gutter, or a different vertical span.
                if (!isOpeningColumn(keep, join) || !isColumnTail(join, strip) ||
                    Width(join.lines.front()) < strip * minWidthFactor ||
                    Width(join.lines.front()) > strip * 1.45f ||
                    spacing < -LONG(strip * .5f) || spacing > LONG(strip * .60f) ||
                    overlap * 100 <= std::min(Height(keep.rect), Height(join.rect)) * 45 ||
                    !AppearanceMatches(regionAppearances[i], regionAppearances[j], true) ||
                    HasVerticalSeparator(image, join.rect, keep.rect, -1, -1, &rubyRects) ||
                    (spacing >= 4 &&
                     HasJaggedBubbleBorder(image, join.rect, keep.rect, regionAppearances[i], &rubyRects))) {
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
        OrderMangaRegions(regions, image);
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
        int scOnlyHan{};
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
        float confidence{};
    };
    auto isSimplifiedOnlyHan = [](uint32_t cp) {
        // Common Simplified-Chinese-only characters absent from standard Japanese Joyo/JIS usage.
        switch (cp) {
        case 0x4EEC: // 们
        case 0x8FD9: // 这
        case 0x4E3A: // 为
        case 0x8BF4: // 说
        case 0x8FD8: // 还
        case 0x4ECE: // 从
        case 0x5BF9: // 对
        case 0x65F6: // 时
        case 0x95F4: // 间
        case 0x7EC8: // 终
        case 0x54CD: // 响
        case 0x79BB: // 离
        case 0x8BED: // 语
        case 0x79CD: // 种
        case 0x5F00: // 开
        case 0x89C1: // 见
        case 0x8BA9: // 让
        case 0x7ED9: // 给
        case 0x8FDB: // 进
        case 0x7ECF: // 经
        case 0x8FC7: // 过
        case 0x5B9E: // 实
        case 0x9645: // 际
        case 0x603B: // 总
        case 0x7ED3: // 结
        case 0x6784: // 构
        case 0x8BBE: // 设
        case 0x8BA1: // 计
        case 0x89C4: // 规
        case 0x6837: // 样
        case 0x6001: // 态
        case 0x5E94: // 应
        case 0x6267: // 执
        case 0x65E0: // 无
        case 0x534E: // 华
        case 0x7231: // 爱
        case 0x68A6: // 梦
        case 0x98CE: // 风
        case 0x98DE: // 飞
        case 0x9A6C: // 马
        case 0x8F66: // 车
        case 0x4E66: // 书
        case 0x957F: // 长
        case 0x4E1C: // 东
        case 0x4E50: // 乐
        case 0x5934: // 头
        case 0x7535: // 电
        case 0x8BDD: // 话
        case 0x542C: // 听
        case 0x53D1: // 发
        case 0x73B0: // 现
        case 0x5173: // 关
        case 0x95EE: // 问
        case 0x9898: // 题
        case 0x673A: // 机
        case 0x52A8: // 动
        case 0x52A1: // 务
        case 0x4EA7: // 产
        case 0x4E1A: // 业
        case 0x8D28: // 质
        case 0x4E2A: // 个
        case 0x4E48: // 么
        case 0x5E76: // 并
        case 0x4E8E: // 于
        case 0x5219: // 则
        case 0x4E24: // 两
        case 0x4EBF: // 亿
        case 0x8BCD: // 词
        case 0x6C47: // 汇
        case 0x8FDC: // 远
            return true;
        default:
            return false;
        }
    };
    auto collectCounts = [&](bool applyFloor) {
        std::vector<RegionCounts> out;
        out.reserve(regions.size());
        for (const auto& r : regions) {
            if (r.text.empty())
                continue;
            const int cpCount = CodePointCount(r.text);
            if (cpCount == 0)
                continue;
            if (applyFloor && r.confidence > 0.f &&
                (r.confidence < .62f || (cpCount <= 2 && r.confidence < .80f)))
                continue;
            RegionCounts c;
            c.confidence = r.confidence;
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
                } else if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) ||
                           (cp >= 0x3130 && cp <= 0x318F)) {
                    flushWord();
                    ++c.hangul;
                } else if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)) {
                    flushWord();
                    ++c.han;
                    if (isSimplifiedOnlyHan(cp))
                        ++c.scOnlyHan;
                } else if ((cp >= 0x0400 && cp <= 0x04FF) || (cp >= 0x0500 && cp <= 0x052F)) {
                    flushWord();
                    ++c.cyrillic;
                    if (cp == 0x0404 || cp == 0x0454 || cp == 0x0406 || cp == 0x0456 || cp == 0x0407 ||
                        cp == 0x0457 || cp == 0x0490 || cp == 0x0491)
                        ++c.ukrainian;
                } else if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
                           (cp >= 0x08A0 && cp <= 0x08FF)) {
                    flushWord();
                    ++c.arabic;
                } else if (cp >= 0x0590 && cp <= 0x05FF) {
                    flushWord();
                    ++c.hebrew;
                } else if (cp >= 0x0E00 && cp <= 0x0E7F) {
                    flushWord();
                    ++c.thai;
                } else if (cp >= 0x0370 && cp <= 0x03FF) {
                    flushWord();
                    ++c.greek;
                } else if (cp >= 0x0900 && cp <= 0x097F) {
                    flushWord();
                    ++c.devanagari;
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
            out.push_back(c);
        }
        return out;
    };
    auto perRegion = collectCounts(true);
    if (perRegion.empty())
        perRegion = collectCounts(false);
    int totalKana = 0, totalHangul = 0, totalHan = 0, totalPureHan = 0, maxPureHan = 0, totalScOnlyHan = 0;
    int totalCyrillic = 0, totalArabic = 0, totalHebrew = 0, totalThai = 0, totalGreek = 0, totalDevanagari = 0;
    int totalLatin = 0;
    int maxKana = 0, maxHangul = 0, maxCyrillic = 0, maxArabic = 0, maxHebrew = 0, maxThai = 0, maxGreek = 0,
        maxDevanagari = 0;
    for (const auto& c : perRegion) {
        totalKana += c.kana;
        totalHangul += c.hangul;
        totalHan += c.han;
        if (c.han >= 6 && c.kana == 0) {
            totalPureHan += c.han;
            totalScOnlyHan += c.scOnlyHan;
        }
        if (c.kana == 0)
            maxPureHan = std::max(maxPureHan, c.han);
        totalCyrillic += c.cyrillic;
        totalArabic += c.arabic;
        totalHebrew += c.hebrew;
        totalThai += c.thai;
        totalGreek += c.greek;
        totalDevanagari += c.devanagari;
        totalLatin += c.latinLetters;
        maxKana = std::max(maxKana, c.kana);
        maxHangul = std::max(maxHangul, c.hangul);
        maxCyrillic = std::max(maxCyrillic, c.cyrillic);
        maxArabic = std::max(maxArabic, c.arabic);
        maxHebrew = std::max(maxHebrew, c.hebrew);
        maxThai = std::max(maxThai, c.thai);
        maxGreek = std::max(maxGreek, c.greek);
        maxDevanagari = std::max(maxDevanagari, c.devanagari);
    }
    const int totalChars = totalKana + totalHangul + totalHan + totalCyrillic + totalArabic + totalHebrew +
                           totalThai + totalGreek + totalDevanagari + totalLatin;
    auto hasStrongScript = [&](int scriptTotal, int scriptMaxRegion) {
        if (scriptTotal <= 0)
            return false;
        const int otherChars = totalChars - scriptTotal;
        // When another script clearly dominates the page, require >= 2 characters (and either a
        // multi-char region or >= 2 occurrences) so a single hallucinated glyph never pollutes langs.
        if (otherChars >= 6)
            return scriptTotal >= 2 && (scriptMaxRegion >= 2 || scriptTotal >= 3);
        return true;
    };
    const bool anyKana = hasStrongScript(totalKana, maxKana);
    const bool anyNonLatin =
        anyKana || hasStrongScript(totalHangul, maxHangul) || totalHan >= (totalChars >= 8 ? 2 : 1) ||
        hasStrongScript(totalCyrillic, maxCyrillic) || hasStrongScript(totalArabic, maxArabic) ||
        hasStrongScript(totalHebrew, maxHebrew) || hasStrongScript(totalThai, maxThai) ||
        hasStrongScript(totalGreek, maxGreek) || hasStrongScript(totalDevanagari, maxDevanagari);
    const bool allowZhAlongsideJa =
        (maxPureHan >= 12 && totalPureHan >= 20 && totalPureHan * 4 >= totalHan) || totalScOnlyHan >= 2;
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
        if (c.kana > 0 && anyKana)
            pushUnique("ja");
        if (c.hangul > 0 && hasStrongScript(totalHangul, maxHangul))
            pushUnique("ko");
        if (c.han > 0 && c.kana == 0) {
            if (ocrHint == OcrJapanese || sourceHint == "ja") {
                pushUnique("ja");
            } else if (!anyKana) {
                if (totalChars - totalHan < 6 || totalHan >= 2)
                    pushUnique(sourceHint == "zh-Hant" ? "zh-Hant" : "zh-Hans");
            } else if (allowZhAlongsideJa && (c.han >= 12 || c.scOnlyHan >= 2)) {
                pushUnique(sourceHint == "zh-Hant" ? "zh-Hant" : "zh-Hans");
            }
        }
        if (c.arabic > 0 && hasStrongScript(totalArabic, maxArabic))
            pushUnique(sourceHint == "fa" || sourceHint == "ur" ? sourceHint : "ar");
        if (c.hebrew > 0 && hasStrongScript(totalHebrew, maxHebrew))
            pushUnique("he");
        if (c.thai > 0 && hasStrongScript(totalThai, maxThai))
            pushUnique("th");
        if (c.greek > 0 && hasStrongScript(totalGreek, maxGreek))
            pushUnique("el");
        if (c.devanagari > 0 && hasStrongScript(totalDevanagari, maxDevanagari))
            pushUnique("hi");
        if (c.cyrillic > 0 && hasStrongScript(totalCyrillic, maxCyrillic)) {
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
        // Pure local OCR never requests AI fallback; drop empty/unrecognized detector boxes so
        // checkboxes and graphic blanks cannot survive as active empty regions.
        if (!geometryOnly && retainLowConfidenceText)
            std::erase_if(lines, [](const TextLine& line) { return IsUnreadText(line.text); });
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
