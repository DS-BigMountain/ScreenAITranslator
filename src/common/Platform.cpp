#include "Platform.h"
#include <sstream>
#include <iomanip>
namespace sat {
std::wstring Wide(const std::string& v) {
 if(v.empty()) return {};
 int n=MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,v.data(),static_cast<int>(v.size()),nullptr,0);
 if(!n) throw AppError("encoding", "无效的 UTF-8 文本");
 std::wstring out(n,0); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,v.data(),static_cast<int>(v.size()),out.data(),n); return out;
}
std::string Utf8(const std::wstring& v) {
 if(v.empty()) return {};
 int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,v.data(),static_cast<int>(v.size()),nullptr,0,nullptr,nullptr);
 if(!n) throw AppError("encoding", "无效的 Unicode 文本");
 std::string out(n,0); WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,v.data(),static_cast<int>(v.size()),out.data(),n,nullptr,nullptr);return out;
}
void CheckHR(HRESULT hr,const char* stage) { if(FAILED(hr)) { std::ostringstream s; s<<"Windows 调用失败 (0x"<<std::hex<<static_cast<unsigned long>(hr)<<")"; throw AppError(stage,s.str()); } }
std::string Timestamp() { SYSTEMTIME t{};GetLocalTime(&t);char b[40]{};sprintf_s(b,"%04u-%02u-%02u %02u:%02u:%02u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);return b; }
}
