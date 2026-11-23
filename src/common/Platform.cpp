#include "Platform.h"
#include "common/TextMetrics.h"
#include <sstream>
#include <iomanip>
namespace sat {
char32_t NextCodePoint(std::string_view text, size_t& index) {
    if (index >= text.size())
        return 0;
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80) {
        ++index;
        return lead;
    }
    char32_t code = 0;
    size_t length = 1;
    if ((lead & 0xE0) == 0xC0) {
        code = lead & 0x1F;
        length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        code = lead & 0x0F;
        length = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        code = lead & 0x07;
        length = 4;
    } else {
        ++index;
        return 0xFFFD;
    }
    if (index + length > text.size()) {
        index = text.size();
        return 0xFFFD;
    }
    for (size_t k = 1; k < length; ++k) {
        const auto cont = static_cast<unsigned char>(text[index + k]);
        if ((cont & 0xC0) != 0x80) {
            ++index;
            return 0xFFFD;
        }
        code = (code << 6) | (cont & 0x3F);
    }
    index += length;
    return code;
}
bool IsCombiningMark(char32_t code) {
    return (code >= 0x0300 && code <= 0x036F) || (code >= 0x0610 && code <= 0x061A) ||
           (code >= 0x064B && code <= 0x065F) || code == 0x0670 || (code >= 0x06D6 && code <= 0x06DC) ||
           (code >= 0x06DF && code <= 0x06E4) || (code >= 0x06E7 && code <= 0x06E8) ||
           (code >= 0x06EA && code <= 0x06ED) || (code >= 0x0900 && code <= 0x0902) || code == 0x093A ||
           code == 0x093C || (code >= 0x0941 && code <= 0x0948) || code == 0x094D ||
           (code >= 0x0951 && code <= 0x0957) || (code >= 0x0962 && code <= 0x0963) || code == 0x0E31 ||
           (code >= 0x0E34 && code <= 0x0E3A) || (code >= 0x0E47 && code <= 0x0E4E);
}
bool IsRtlCodePoint(char32_t code) {
    if ((code >= 0x0660 && code <= 0x0669) || (code >= 0x06F0 && code <= 0x06F9))
        return false;
    return (code >= 0x0590 && code <= 0x05FF) || (code >= 0x0600 && code <= 0x06FF) ||
           (code >= 0x0750 && code <= 0x077F) || (code >= 0x08A0 && code <= 0x08FF) ||
           (code >= 0xFB50 && code <= 0xFDFF) || (code >= 0xFE70 && code <= 0xFEFF);
}
bool HasRtlScript(std::string_view text) {
    for (size_t i = 0; i < text.size();)
        if (IsRtlCodePoint(NextCodePoint(text, i)))
            return true;
    return false;
}
bool HasRtlScript(std::wstring_view text) {
    for (wchar_t ch : text)
        if (IsRtlCodePoint(static_cast<char32_t>(ch)))
            return true;
    return false;
}
bool HasComplexVerticalScript(std::wstring_view text) {
    for (wchar_t ch : text) {
        const auto code = static_cast<char32_t>(ch);
        if ((code >= 0x0E00 && code <= 0x0E7F) || (code >= 0x0900 && code <= 0x097F) ||
            (code >= 0x0600 && code <= 0x06FF) || (code >= 0x0750 && code <= 0x077F) ||
            (code >= 0x08A0 && code <= 0x08FF))
            return true;
    }
    return false;
}
double EstimatedEm(std::string_view text) {
    double em = 0;
    for (size_t i = 0; i < text.size();) {
        const char32_t code = NextCodePoint(text, i);
        if (IsCombiningMark(code))
            continue;
        if ((code >= 0x2E80 && code <= 0x9FFF) || (code >= 0xAC00 && code <= 0xD7AF) ||
            (code >= 0xF900 && code <= 0xFAFF) || (code >= 0xFF00 && code <= 0xFFEF))
            em += 1.0;
        else if (code == ' ')
            em += .3;
        else if (code < 0x80)
            em += .55;
        else
            em += .6;
    }
    return em;
}
std::wstring Wide(const std::string& v) {
    if (v.empty())
        return {};
    int n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, v.data(), static_cast<int>(v.size()), nullptr, 0);
    if (!n)
        throw AppError("encoding", "无效的 UTF-8 文本");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, v.data(), static_cast<int>(v.size()), out.data(), n);
    return out;
}
std::string Utf8(const std::wstring& v) {
    if (v.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, v.data(), static_cast<int>(v.size()), nullptr,
                                0, nullptr, nullptr);
    if (!n)
        throw AppError("encoding", "无效的 Unicode 文本");
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, v.data(), static_cast<int>(v.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}
void CheckHR(HRESULT hr, const char* stage) {
    if (FAILED(hr)) {
        std::ostringstream s;
        s << "Windows 调用失败 (0x" << std::hex << static_cast<unsigned long>(hr) << ")";
        throw AppError(stage, s.str());
    }
}
std::string Timestamp() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char b[40]{};
    sprintf_s(b, "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
} // namespace sat
