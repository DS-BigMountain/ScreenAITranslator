#pragma once
#include "common/Types.h"
namespace sat {
class Selection {
 HWND hwnd_{};
 Monitor monitor_;
 std::shared_ptr<Image> image_;
 POINT start_{}, current_{};
 bool dragging_{};
 std::function<void(std::optional<RECT>)> done_;
 static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
 void Paint();
 void Finish(std::optional<RECT> rect);
public:
 ~Selection();
 void Show(const Monitor&,std::shared_ptr<Image>,std::function<void(std::optional<RECT>)>);
 void Close();
 HWND Window()const{return hwnd_;}
};
}
