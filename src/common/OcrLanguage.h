#pragma once
namespace sat {
// Persisted IDs are stable: 3 remains the legacy direct-vision mode.
inline constexpr int OcrAuto = 0, OcrEnglish = 1, OcrJapanese = 2, OcrDirectVision = 3, OcrKorean = 4;
inline constexpr int OcrLocalAuto = 5;
inline constexpr int TranslationOcrText = 0, TranslationAiVision = 1;
constexpr bool ValidTranslationMode(int mode) {
    return mode == TranslationOcrText || mode == TranslationAiVision;
}
constexpr bool ValidOcrMode(int mode) {
    return mode >= OcrAuto && mode <= OcrLocalAuto;
}
constexpr int OcrModeFromSelection(int selection) {
    return selection == 0   ? OcrLocalAuto
           : selection == 1 ? OcrEnglish
           : selection == 2 ? OcrJapanese
           : selection == 3 ? OcrKorean
                            : -1;
}
constexpr int OcrSelectionFromMode(int mode) {
    return mode == OcrKorean ? 3 : mode == OcrJapanese ? 2 : mode == OcrEnglish ? 1 : 0;
}
} // namespace sat
