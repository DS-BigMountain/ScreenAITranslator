#pragma once
#include <string_view>
namespace sat {
// Decodes the next UTF-8 code point at `index` (advancing `index`), returning 0xFFFD on invalid bytes.
char32_t NextCodePoint(std::string_view text, size_t& index);
// True for zero-width combining diacritics and stacked marks (Latin, Arabic, Thai, Devanagari).
bool IsCombiningMark(char32_t code);
// True for strong RTL script code points (Arabic, Hebrew).
bool IsRtlCodePoint(char32_t code);
bool HasRtlScript(std::string_view text);
bool HasRtlScript(std::wstring_view text);
// True when text contains tall stacked scripts (Thai, Devanagari, Arabic) needing extra line height.
bool HasComplexVerticalScript(std::wstring_view text);
// Advance of the text in em units: CJK glyphs fill the em box, alphabetic runs take about half,
// and combining marks contribute zero horizontal advance.
double EstimatedEm(std::string_view text);
} // namespace sat
