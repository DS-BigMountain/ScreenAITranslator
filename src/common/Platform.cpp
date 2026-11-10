#include "Platform.h"
#include "common/TextMetrics.h"
#include <sstream>
#include <iomanip>
namespace sat {
double EstimatedEm(std::string_view text) {
    double em = 0;
    size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        char32_t code = lead;
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
        }
        if (i + length > text.size())
            break;
        for (size_t k = 1; k < length; ++k)
            code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        i += length;
        if (code >= 0x2E80)
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
