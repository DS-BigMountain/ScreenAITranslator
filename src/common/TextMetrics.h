#pragma once
#include <string_view>
namespace sat {
// Advance of the text in em units: CJK glyphs fill the em box, Latin runs take about half. The
// estimate converts a measured row width into the font size that produced it.
double EstimatedEm(std::string_view text);
} // namespace sat
