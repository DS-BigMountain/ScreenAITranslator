#include "app/SourceMatching.h"
#include "common/Diagnostics.h"
#include "common/TextMetrics.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>

namespace sat {
namespace {
// The model rectangle approximates a passage; the detector measures the individual screen rows of
// that passage. Pairing therefore happens at row level, and these limits describe when a row still
// belongs to the sampled passage after the model rectangle drifted.
constexpr float MinimumHorizontalShare = .3f;
constexpr float MinimumOverlappingShare = .2f;
// A model rectangle routinely sits most of one line above or below its text, so a row qualifies as a
// candidate through its width and its centre distance even when the rectangles do not intersect.
constexpr float MaximumCentreDrift = 2.6f;
constexpr float MinimumWidthCloseness = .3f;
constexpr float MinimumScore = .3f;
// A translation may occupy the neighbouring rows of its paragraph, never more than a short block.
constexpr int MaximumPassageRows = 6;

RECT ModelPixels(const Box& box, int width, int height) {
    const int x = std::clamp(box.x, 0, 999), y = std::clamp(box.y, 0, 999);
    const int right =
        int(std::clamp(static_cast<long long>(box.x) + box.width, static_cast<long long>(x + 1), 1000LL));
    const int bottom =
        int(std::clamp(static_cast<long long>(box.y) + box.height, static_cast<long long>(y + 1), 1000LL));
    RECT r{MulDiv(x, width, 1000), MulDiv(y, height, 1000), MulDiv(right, width, 1000),
           MulDiv(bottom, height, 1000)};
    r.left = std::min<LONG>(r.left, width - 1);
    r.top = std::min<LONG>(r.top, height - 1);
    r.right = std::min<LONG>(width, std::max(r.left + 1, r.right));
    r.bottom = std::min<LONG>(height, std::max(r.top + 1, r.bottom));
    return r;
}

long long Area(RECT r) {
    return static_cast<long long>(std::max<LONG>(0, r.right - r.left)) *
           static_cast<long long>(std::max<LONG>(0, r.bottom - r.top));
}

// Horizontal share compares the two x spans directly: a row that sits below the model rectangle
// still describes the same passage when its columns match.
float HorizontalShare(RECT model, RECT line) {
    const LONG left = std::max<LONG>(model.left, line.left);
    const LONG right = std::min<LONG>(model.right, line.right);
    if (right <= left)
        return 0.f;
    const LONG wider = std::max(Width(model), Width(line));
    return wider > 0 ? float(right - left) / float(wider) : 0.f;
}

float VerticalShare(RECT model, RECT line) {
    const LONG top = std::max<LONG>(model.top, line.top);
    const LONG bottom = std::min<LONG>(model.bottom, line.bottom);
    if (bottom <= top)
        return 0.f;
    const LONG smaller = std::min(Height(model), Height(line));
    return smaller > 0 ? float(bottom - top) / float(smaller) : 0.f;
}

float WidthCloseness(RECT a, RECT b) {
    const LONG wide = std::max(Width(a), Width(b));
    const LONG narrow = std::min(Width(a), Width(b));
    return wide > 0 ? float(narrow) / float(wide) : 0.f;
}

// Vertical distance between the centres, expressed in source rows.
float CentreDrift(RECT model, RECT line) {
    const float height = std::max(6.f, float(std::min(Height(model), Height(line))));
    const float first = float(model.top + model.bottom) * .5f;
    const float second = float(line.top + line.bottom) * .5f;
    return std::abs(first - second) / height;
}

struct SourceLine {
    RECT rect{};
    float height{};
    float confidence{};
    int regionId{};
    bool claimed{};
    // A vertical passage arrives as one rectangle over its balloon plus the strips beneath it. The
    // strips are what the layout needs, so they travel with the pairing.
    std::vector<RECT> strips;
};
} // namespace

SourceMatchReport AttachSourceGeometry(TranslationResult& result, int width, int height,
                                       const std::vector<TextRegion>& regions) {
    SourceMatchReport report;
    report.segments = result.segments.size();
    if (width <= 0 || height <= 0 || regions.empty())
        return report;

    // Detection describes screen rows; the model describes passages. Flattening the recognized rows
    // gives the pairing the geometry that a model rectangle can only approximate.
    std::vector<SourceLine> lines;
    for (const auto& region : regions) {
        // A vertical passage wraps across strips, so its balloon rectangle is what a model box can
        // describe. Pairing it whole keeps every strip for the layout, which sets one glyph per row.
        const bool column = !region.lines.empty() &&
                            std::all_of(region.lines.begin(), region.lines.end(), [](RECT line) {
                                return Width(line) > 0 && Height(line) * 5 > Width(line) * 6;
                            });
        if (!region.lines.empty() && !column) {
            for (const auto& line : region.lines)
                if (Valid(line))
                    lines.push_back({line, float(Height(line)), region.confidence, region.id, false, {}});
            continue;
        }
        if (Valid(region.rect))
            lines.push_back({region.rect, float(Height(region.rect)), region.confidence, region.id, false,
                             column ? region.lines : std::vector<RECT>{}});
    }
    if (lines.empty())
        return report;

    // Passages arrive in reading order, so each one claims its rows before the next passage looks.
    // A wrapped paragraph therefore keeps its own rows instead of losing them to a neighbour.
    std::vector<std::vector<size_t>> owned(result.segments.size());
    for (size_t s = 0; s < result.segments.size(); ++s) {
        const auto& segment = result.segments[s];
        if (segment.positionUnavailable || segment.sourcePixels)
            continue;
        const RECT model = ModelPixels(segment.box, width, height);
        size_t anchor = lines.size();
        float anchorScore = MinimumScore;
        for (size_t l = 0; l < lines.size(); ++l) {
            if (lines[l].claimed)
                continue;
            const RECT row = lines[l].rect;
            const float horizontal = HorizontalShare(model, row);
            if (horizontal < MinimumHorizontalShare) {
                ++report.rejectedSpace;
                continue;
            }
            const float vertical = VerticalShare(model, row);
            const float closeness = WidthCloseness(model, row);
            const float drift = CentreDrift(model, row);
            // Row geometry and the model rectangle can miss each other; a row of matching width
            // within reach still describes the passage.
            if (vertical < MinimumOverlappingShare &&
                (drift > MaximumCentreDrift || closeness < MinimumWidthCloseness)) {
                ++report.rejectedBounds;
                continue;
            }
            const float score = 1.f * vertical + 2.4f * horizontal + 2.f * closeness - .5f * drift;
            if (score <= MinimumScore) {
                ++report.rejectedText;
                continue;
            }
            if (score > anchorScore) {
                anchorScore = score;
                anchor = l;
            }
        }
        if (anchor == lines.size()) {
            // Small labels inside artwork sit far from the rectangle the model drew for them; the
            // nearest unclaimed row of matching width still describes that label.
            float bestDistance = std::numeric_limits<float>::max();
            for (size_t l = 0; l < lines.size(); ++l) {
                if (lines[l].claimed || HorizontalShare(model, lines[l].rect) < .35f ||
                    WidthCloseness(model, lines[l].rect) < .35f)
                    continue;
                const float distance = CentreDrift(model, lines[l].rect);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    anchor = l;
                }
            }
        }
        if (anchor == lines.size()) {
            continue;
        }
        lines[anchor].claimed = true;
        owned[s].push_back(anchor);
        // A wrapped paragraph continues on the next row. Detection describes the wrap exactly, while
        // the model rectangle covers only its first row, so the extra row comes from the measured
        // geometry. A row joins the passage when the translation outgrows the anchor row, the next
        // row is narrower (the shape of a wrapped line), and the source text itself already filled
        // more than one row.
        const double em = EstimatedEm(segment.translated);
        const double sourceEm = EstimatedEm(segment.original);
        const double rowWidth = std::max(8.f, float(Width(lines[anchor].rect)));
        const double bandSize = double(lines[anchor].height) / .86f;
        const bool outgrows = em * bandSize > rowWidth * 1.15;
        // One continuation row per passage: a wrapped line continues below its anchor, while a
        // longer paragraph is rare enough on screen that the anchor row already describes it.
        while (outgrows && owned[s].size() < 2) {
            const RECT last = lines[owned[s].back()].rect;
            size_t best = lines.size();
            float bestGap = std::numeric_limits<float>::max();
            for (size_t l = 0; l < lines.size(); ++l) {
                if (lines[l].claimed || lines[l].rect.top < last.top)
                    continue;
                if (HorizontalShare(last, lines[l].rect) < .2f)
                    continue;
                if (float(Width(lines[l].rect)) > float(Width(last)) * .95f)
                    continue;
                const float gap = float(lines[l].rect.top - last.bottom);
                // A continuation row follows the anchor with a real gap. Touching or overlapping bands
                // describe the same baseline, and claiming one of those would shift a passage upward.
                if (gap < 0 || gap > std::max(8.f, lines[l].height * .95f))
                    continue;
                if (gap < bestGap) {
                    bestGap = gap;
                    best = l;
                }
            }
            if (best == lines.size())
                break;
            if (sourceEm <= rowWidth / bandSize * .9)
                break;
            lines[best].claimed = true;
            owned[s].push_back(best);
        }
    }

    // A semantic line often reaches the detector as several boxes: a label and its value sit side by
    // side while the model describes them as one passage. Merging same-band neighbours keeps the
    // coverage rectangle as wide as the source line, which a narrow label box would otherwise
    // starve and force the translation into an under-sized face.
    for (size_t s = 0; s < owned.size(); ++s) {
        if (owned[s].empty())
            continue;
        const RECT model = ModelPixels(result.segments[s].box, width, height);
        // The anchor row fixes the horizontal band. A merged paragraph union spans several baseline
        // rows, so measuring overlap against that union would pull vertically adjacent rows in.
        const RECT band = lines[owned[s][0]].rect;
        const LONG bandHeight = Height(band);
        bool grew = true;
        while (grew && owned[s].size() < size_t(MaximumPassageRows)) {
            grew = false;
            RECT united = lines[owned[s].front()].rect;
            for (size_t l : owned[s])
                UnionRect(&united, &united, &lines[l].rect);
            for (size_t l = 0; l < lines.size(); ++l) {
                if (lines[l].claimed)
                    continue;
                const RECT row = lines[l].rect;
                const LONG sharedRows = std::min(band.bottom, row.bottom) - std::max(band.top, row.top);
                if (sharedRows < bandHeight * .7f)
                    continue;
                const LONG gap = std::max(united.left, row.left) - std::min(united.right, row.right);
                if (gap > std::max<LONG>(4, Width(row) / 2))
                    continue;
                RECT merged{};
                UnionRect(&merged, &united, &row);
                if (float(Width(merged)) > float(Width(model)) * 1.6f)
                    continue;
                lines[l].claimed = true;
                owned[s].push_back(l);
                grew = true;
            }
        }
    }

    for (size_t s = 0; s < owned.size(); ++s) {
        if (owned[s].empty())
            continue;
        auto& segment = result.segments[s];
        const auto& anchorLine = lines[owned[s].front()];
        if (!anchorLine.strips.empty()) {
            // Column passage: the balloon rectangle supplies the source pixels, and the strips supply the
            // glyph size, which is their median width.
            segment.sourcePixels = anchorLine.rect;
            segment.sourceLines = anchorLine.strips;
            std::vector<int> widths;
            widths.reserve(anchorLine.strips.size());
            for (auto strip : anchorLine.strips)
                widths.push_back(Width(strip));
            std::sort(widths.begin(), widths.end());
            segment.sourceLineHeight = float(widths[widths.size() / 2]);
            ++report.adopted;
            ++report.matchedLines;
            report.sourceIds.push_back(anchorLine.regionId);
            DiagnosticEvent("geometry_adopted", {{"segment", int(s)},
                                                 {"source_id", anchorLine.regionId},
                                                 {"rows", 1},
                                                 {"column", 1}});
            continue;
        }
        std::vector<RECT> rows;
        rows.reserve(owned[s].size());
        for (size_t l : owned[s])
            rows.push_back(lines[l].rect);
        std::sort(rows.begin(), rows.end(), [](RECT a, RECT b) { return a.top < b.top; });
        RECT united = rows.front();
        std::vector<float> heights;
        heights.reserve(rows.size());
        for (const auto& row : rows) {
            UnionRect(&united, &united, &row);
            heights.push_back(float(Height(row)));
        }
        std::sort(heights.begin(), heights.end());
        segment.sourcePixels = united;
        segment.sourceLines = std::move(rows);
        segment.sourceLineHeight = heights[heights.size() / 2];
        ++report.adopted;
        report.matchedLines += owned[s].size();
        report.sourceIds.push_back(lines[owned[s].front()].regionId);
        DiagnosticEvent("geometry_adopted", {{"segment", int(s)},
                                             {"source_id", lines[owned[s].front()].regionId},
                                             {"rows", int(owned[s].size())}});
    }

    // Recognized rows no passage explained still carry source text. The overlay rebuilds them so the
    // screen reads as a translation rather than a mixed page.
    for (size_t l = 0; l < lines.size(); ++l) {
        if (lines[l].claimed || lines[l].confidence < .85f)
            continue;
        bool covered = false;
        for (const auto& segment : result.segments) {
            if (!segment.sourcePixels)
                continue;
            RECT overlap{};
            if (!IntersectRect(&overlap, &segment.sourcePixels.value(), &lines[l].rect))
                continue;
            if (float(Area(overlap)) / float(std::max<long long>(1, Area(lines[l].rect))) > .6f) {
                covered = true;
                break;
            }
        }
        if (!covered)
            report.uncoveredLines.push_back(lines[l].rect);
    }
    return report;
}
} // namespace sat
