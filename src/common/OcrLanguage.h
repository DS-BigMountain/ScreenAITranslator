#pragma once
namespace sat {
// Persisted IDs are stable: 3 remains the legacy direct-vision mode.
inline constexpr int OcrAuto=0,OcrEnglish=1,OcrJapanese=2,OcrDirectVision=3,OcrKorean=4;
constexpr bool ValidOcrMode(int mode){return mode>=OcrAuto&&mode<=OcrKorean;}
constexpr int OcrModeFromSelection(int selection){return selection==3?OcrKorean:selection>=0&&selection<=2?selection:-1;}
constexpr int OcrSelectionFromMode(int mode){return mode==OcrKorean?3:mode==OcrDirectVision?0:mode;}
}
