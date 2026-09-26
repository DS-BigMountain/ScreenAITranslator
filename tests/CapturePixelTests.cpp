#include "capture/Capture.h"
#include "selection/Selection.h"
#include <dwmapi.h>
#include <iostream>
#include <future>
#include <thread>
#include <algorithm>
#include <wincodec.h>
#include <wrl/client.h>

namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void PumpFor(int milliseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < end);
}
constexpr COLORREF colors[]{RGB(230,30,45), RGB(35,210,65), RGB(30,65,235), RGB(240,240,240)};
constexpr POINT samples[]{{80,100},{320,100},{80,230},{320,230}};
COLORREF Pixel(const sat::Image& image, int x, int y) {
    Require(x >= 0 && x < image.width && y >= 0 && y < image.height, "pixel probe outside capture");
    const auto p = (static_cast<size_t>(y) * image.width + x) * 4;
    return RGB(image.bgra[p+2],image.bgra[p+1],image.bgra[p]);
}
bool Near(COLORREF actual, COLORREF expected, int tolerance = 5) {
    return std::abs(static_cast<int>(GetRValue(actual))-GetRValue(expected)) <= tolerance &&
        std::abs(static_cast<int>(GetGValue(actual))-GetGValue(expected)) <= tolerance &&
        std::abs(static_cast<int>(GetBValue(actual))-GetBValue(expected)) <= tolerance;
}
void PrintPixel(const char* label, COLORREF pixel) {
    std::cout << label << '=' << static_cast<int>(GetRValue(pixel)) << ',' << static_cast<int>(GetGValue(pixel)) << ',' << static_cast<int>(GetBValue(pixel)) << '\n';
}
LRESULT CALLBACK FixtureProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window,&paint);
        for (int i = 0; i < 4; ++i) {
            RECT rect{(i%2)*240,(i/2)*160,(i%2+1)*240,(i/2+1)*160};
            HBRUSH brush = CreateSolidBrush(colors[i]); FillRect(dc,&rect,brush); DeleteObject(brush);
        }
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(0,0,0));
        TextOutW(dc,260,270,L"CAPTURE PIXEL TEST 文字",21);
        EndPaint(window,&paint); return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
struct Fixture {
    HWND window{};
    Fixture(int x, int y) {
        WNDCLASSW wc{};wc.lpfnWndProc=FixtureProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SAT.CapturePixelFixture";
        RegisterClassW(&wc);
        window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"ScreenAI capture pixel fixture",WS_POPUP,x,y,480,320,nullptr,nullptr,wc.hInstance,nullptr);
        Require(window!=nullptr,"cannot create controlled fixture");
        ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);DwmFlush();PumpFor(150);
    }
    ~Fixture(){if(window)DestroyWindow(window);}
};
sat::Image Synthetic() {
    sat::Image image{480,320,std::vector<unsigned char>(480*320*4)};
    for(int y=0;y<image.height;++y)for(int x=0;x<image.width;++x){auto color=colors[(y/160)*2+x/240];const auto p=(static_cast<size_t>(y)*image.width+x)*4;image.bgra[p]=GetBValue(color);image.bgra[p+1]=GetGValue(color);image.bgra[p+2]=GetRValue(color);image.bgra[p+3]=255;}
    return image;
}
void CheckEncodedFixture(const sat::Image& desktop,int x,int y) {
    using Microsoft::WRL::ComPtr;
    const auto crop=sat::Crop(desktop,{x,y,x+480,y+320});
    auto bytes=sat::EncodeJpeg(crop,1,{});
    const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);sat::CheckHR(hr,"test");
    struct Guard{~Guard(){CoUninitialize();}} guard;
    ComPtr<IWICImagingFactory> factory;sat::CheckHR(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"test");
    ComPtr<IWICStream> stream;sat::CheckHR(factory->CreateStream(&stream),"test");sat::CheckHR(stream->InitializeFromMemory(bytes.data(),static_cast<DWORD>(bytes.size())),"test");
    ComPtr<IWICBitmapDecoder> decoder;sat::CheckHR(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder),"test");
    ComPtr<IWICBitmapFrameDecode> frame;sat::CheckHR(decoder->GetFrame(0,&frame),"test");
    UINT width{},height{};sat::CheckHR(frame->GetSize(&width,&height),"test");Require(width==480&&height==320,"encoded ROI dimensions changed");
    ComPtr<IWICFormatConverter> converter;sat::CheckHR(factory->CreateFormatConverter(&converter),"test");
    sat::CheckHR(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"test");
    sat::Image decoded{480,320,std::vector<unsigned char>(480*320*4)};
    sat::CheckHR(converter->CopyPixels(nullptr,480*4,static_cast<UINT>(decoded.bgra.size()),decoded.bgra.data()),"test");
    for(int i=0;i<4;++i)Require(Near(Pixel(decoded,samples[i].x,samples[i].y),colors[i],10),"JPEG discarded fixture colors");
    int textPixels{};
    for(int row=270;row<300;++row)for(int col=260;col<470;++col){const auto pixel=Pixel(decoded,col,row);if(GetRValue(pixel)<100&&GetGValue(pixel)<100&&GetBValue(pixel)<100)++textPixels;}
    Require(textPixels>50,"captured text did not survive crop and JPEG encode");
    std::cout << "JPEG fixture text dark pixels=" << textPixels << '\n';
}
void CheckSelection(const sat::Monitor& monitor, std::shared_ptr<sat::Image> image, int originX, int originY) {
    sat::Selection selection;selection.Show(monitor,image,[](auto){});PumpFor(100);DwmFlush();
    HDC dc=GetDC(selection.Window());
    bool matches=true;
    for(int i=0;i<4;++i){const auto p=samples[i];const auto actual=GetPixel(dc,originX+p.x,originY+p.y);PrintPixel("selection-dim",actual);const auto expected=RGB(GetRValue(colors[i])*150/255,GetGValue(colors[i])*150/255,GetBValue(colors[i])*150/255);matches &= Near(actual,expected,5);}
    ReleaseDC(selection.Window(),dc);
    Require(matches,"selection rendering lost fixture colors outside selected rectangle");
    SendMessageW(selection.Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(originX+40,originY+70));
    SendMessageW(selection.Window(),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(originX+400,originY+280));
    UpdateWindow(selection.Window());PumpFor(50);DwmFlush();
    dc=GetDC(selection.Window());matches=true;
    for(int i=0;i<4;++i){const auto p=samples[i];const auto actual=GetPixel(dc,originX+p.x,originY+p.y);PrintPixel("selection-bright",actual);matches &= Near(actual,colors[i]);}
    ReleaseDC(selection.Window(),dc);
    Require(matches,"selected rectangle failed to retain original colors");
    selection.Close();PumpFor(50);
}
}
int main(int argc,char** argv) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto monitors=sat::EnumerateMonitors();Require(!monitors.empty(),"no monitors");const auto monitor=monitors.front();
        const int x=monitor.rect.left+40,y=monitor.rect.top+100;
        sat::Monitor selectionMonitor{L"synthetic-selection",{x,y,x+480,y+320},monitor.dpi};
        CheckSelection(selectionMonitor,std::make_shared<sat::Image>(Synthetic()),0,0);
        std::cout << "Synthetic selection pixel check passed\n";
        if(argc>1 && std::string(argv[1])=="--selection-only")return 0;
        Fixture fixture(x,y);
        HDC dc=GetDC(fixture.window);
        for(int i=0;i<4;++i)Require(Near(GetPixel(dc,samples[i].x,samples[i].y),colors[i]),"fixture itself did not render expected colors");
        ReleaseDC(fixture.window,dc);
        for(int repeat=0;repeat<3;++repeat){
            auto future=std::async(std::launch::async,[&]{return sat::CaptureMonitor(monitor,{});});
            while(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)PumpFor(10);
            auto captured=std::make_shared<sat::Image>(future.get());
            std::cout << "Capture backend=" << static_cast<int>(captured->backend) << '\n';bool matches=true;
            for(int i=0;i<4;++i){const auto actual=Pixel(*captured,x-monitor.rect.left+samples[i].x,y-monitor.rect.top+samples[i].y);PrintPixel("captured",actual);matches &= Near(actual,colors[i]);}
            Require(matches,"desktop capture contains wrong/black fixture pixels");
            if(repeat==0){CheckEncodedFixture(*captured,x-monitor.rect.left,y-monitor.rect.top);CheckSelection(monitor,captured,x-monitor.rect.left,y-monitor.rect.top);}
        }
        std::cout << "CapturePixelTests PASS: fixture colors survive capture and frozen selection; no screenshots persisted\n";
        return 0;
    }catch(const std::exception& error){std::cerr << "CapturePixelTests FAIL: " << error.what() << '\n';return 1;}
}
