#pragma once
#include <array>
#include <string_view>
namespace sat {
// Persisted IDs are stable: 3 remains the legacy direct-vision mode.
inline constexpr int OcrAuto = 0, OcrEnglish = 1, OcrJapanese = 2, OcrDirectVision = 3, OcrKorean = 4;
inline constexpr int OcrLocalAuto = 5;
inline constexpr int OcrChinese = 6;
inline constexpr int OcrLatin = 7;
inline constexpr int OcrRussian = 8;
inline constexpr int OcrThai = 9;
inline constexpr int OcrArabic = 10;
inline constexpr int OcrGreek = 11;
inline constexpr int OcrDevanagari = 12;
inline constexpr int TranslationOcrText = 0, TranslationAiVision = 1;
constexpr bool ValidTranslationMode(int mode) {
    return mode == TranslationOcrText || mode == TranslationAiVision;
}
constexpr bool ValidOcrMode(int mode) {
    return mode >= OcrAuto && mode <= OcrDevanagari;
}
constexpr bool IsLocalOcrLanguage(int mode) {
    return mode == OcrLocalAuto || mode == OcrEnglish || mode == OcrJapanese || mode == OcrKorean ||
           (mode >= OcrChinese && mode <= OcrDevanagari);
}
inline constexpr std::array<int, 11> kOcrSelectionModes{
    OcrLocalAuto, OcrEnglish, OcrJapanese, OcrKorean, OcrChinese,   OcrLatin,
    OcrRussian,   OcrThai,    OcrArabic,   OcrGreek,  OcrDevanagari,
};
constexpr int OcrModeFromSelection(int selection) {
    return selection >= 0 && selection < int(kOcrSelectionModes.size()) ? kOcrSelectionModes[size_t(selection)]
                                                                       : -1;
}
constexpr int OcrSelectionFromMode(int mode) {
    for (size_t i = 0; i < kOcrSelectionModes.size(); ++i)
        if (kOcrSelectionModes[i] == mode)
            return int(i);
    return 0;
}
constexpr const wchar_t* OcrLanguageShortLabel(int mode) {
    switch (mode) {
    case OcrEnglish: return L"英语";
    case OcrJapanese: return L"日语";
    case OcrKorean: return L"韩语";
    case OcrChinese: return L"中文";
    case OcrLatin: return L"拉丁语系";
    case OcrRussian: return L"俄语/西里尔";
    case OcrThai: return L"泰语";
    case OcrArabic: return L"阿拉伯语";
    case OcrGreek: return L"希腊语";
    case OcrDevanagari: return L"印地语";
    default: return L"自动识别";
    }
}
constexpr const wchar_t* OcrLanguageMenuLabel(int mode) {
    switch (mode) {
    case OcrEnglish: return L"英语";
    case OcrJapanese: return L"日语";
    case OcrKorean: return L"韩语";
    case OcrChinese: return L"中文";
    case OcrLatin: return L"欧洲拉丁语系（法/德/西/意/葡/越等）";
    case OcrRussian: return L"俄语 / 西里尔语系";
    case OcrThai: return L"泰语";
    case OcrArabic: return L"阿拉伯语";
    case OcrGreek: return L"希腊语";
    case OcrDevanagari: return L"印地语 / 天城文";
    default: return L"自动识别";
    }
}
constexpr const char* TargetIdFromOcrMode(int mode) {
    switch (mode) {
    case OcrEnglish: return "en";
    case OcrJapanese: return "ja";
    case OcrKorean: return "ko";
    case OcrChinese: return "zh-Hans";
    case OcrRussian: return "ru";
    case OcrThai: return "th";
    case OcrArabic: return "ar";
    case OcrGreek: return "el";
    case OcrDevanagari: return "hi";
    case OcrLatin: return "fr";
    default: return "";
    }
}
constexpr int OcrModeFromLanguageCode(std::string_view code) {
    if (code == "en")
        return OcrEnglish;
    if (code == "ja")
        return OcrJapanese;
    if (code == "ko")
        return OcrKorean;
    if (code == "zh-Hans" || code == "zh-Hant" || code == "zh" || code == "zh-CN" || code == "zh-TW" ||
        code == "yue" || code == "wyw")
        return OcrChinese;
    if (code == "ru" || code == "uk" || code == "be" || code == "bg" || code == "sr" || code == "mk" ||
        code == "kk" || code == "mn" || code == "uz")
        return OcrRussian;
    if (code == "th")
        return OcrThai;
    if (code == "ar" || code == "fa" || code == "ur" || code == "sd")
        return OcrArabic;
    if (code == "el")
        return OcrGreek;
    if (code == "hi" || code == "mr" || code == "ne")
        return OcrDevanagari;
    if (code == "fr" || code == "de" || code == "es" || code == "pt" || code == "it" || code == "vi" ||
        code == "id" || code == "tr" || code == "nl" || code == "pl" || code == "cs" || code == "da" ||
        code == "fi" || code == "sv" || code == "no" || code == "is" || code == "ro" || code == "hr" ||
        code == "sk" || code == "sl" || code == "lv" || code == "lt" || code == "et" || code == "hu" ||
        code == "ms" || code == "tl" || code == "sq" || code == "az" || code == "eu" || code == "bs" ||
        code == "ca" || code == "gl" || code == "ga" || code == "la" || code == "lb" || code == "mt" ||
        code == "cy" || code == "sw" || code == "af" || code == "ha" || code == "haw" || code == "ceb" ||
        code == "eo" || code == "yo" || code == "ku")
        return OcrLatin;
    return OcrLocalAuto;
}
} // namespace sat
