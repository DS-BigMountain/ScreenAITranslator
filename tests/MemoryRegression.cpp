#include "capture/BundledOcr.h"
#include "capture/Capture.h"
#include "common/Platform.h"
#include <psapi.h>
#include <wincodec.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
using namespace sat;
using Json=nlohmann::json;
namespace {
Json Memory(){
 PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);
 if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters)))throw std::runtime_error("Memory query failed");
 return {{"working_set",counters.WorkingSetSize},{"private_bytes",counters.PrivateUsage},{"peak_working_set",counters.PeakWorkingSetSize}};
}
Image Read(const std::filesystem::path& path){
 ComPtr<IWICImagingFactory> factory;CheckHR(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"memory-test");
 ComPtr<IWICBitmapDecoder> decoder;CheckHR(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder),"memory-test");
 ComPtr<IWICBitmapFrameDecode> frame;CheckHR(decoder->GetFrame(0,&frame),"memory-test");ComPtr<IWICFormatConverter> converter;CheckHR(factory->CreateFormatConverter(&converter),"memory-test");
 CheckHR(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"memory-test");
 UINT width{},height{};CheckHR(converter->GetSize(&width,&height),"memory-test");Image image{int(width),int(height),std::vector<unsigned char>(size_t(width)*height*4)};
 CheckHR(converter->CopyPixels(nullptr,width*4,UINT(image.bgra.size()),image.bgra.data()),"memory-test");return image;
}
}
int wmain(int argc,wchar_t** argv){try{
 ComScope com;Json report={{"initial",Memory()},{"trials",Json::array()}};
 std::atomic<size_t> peakPrivate{};
 std::jthread sampler([&](std::stop_token stop){while(!stop.stop_requested()){auto value=Memory()["private_bytes"].get<size_t>();peakPrivate.store(std::max(value,peakPrivate.load()));std::this_thread::sleep_for(std::chrono::milliseconds(5));}});
 const wchar_t* names[]{L"bbc-headlines-en.png",L"asahi-article-ja.png",L"bbc-article-en.png",L"asahi-election-ja.png",L"asahi-grid-ja.png"};
 for(int round=0;round<2;++round)for(auto name:names){
  Json trial={{"file",Utf8(name)},{"round",round}};
  {
   auto image=Read(std::filesystem::path(L"test-fixtures/user")/name);auto start=std::chrono::steady_clock::now();
   auto lines=RecognizeBundled(image,OcrJapanese,{});if(lines.empty())throw std::runtime_error("User screenshot lost OCR text");
   trial["ms"]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();trial["lines"]=Json::array();
   for(auto& line:lines)trial["lines"].push_back({{"text",line.text},{"box",{line.rect.left,line.rect.top,line.rect.right,line.rect.bottom}},{"confidence",line.confidence}});
  }
  trial["after"]=Memory();std::cout<<trial["file"]<<" round="<<round<<" ms="<<trial["ms"]<<" private_MiB="<<trial["after"]["private_bytes"].get<double>()/1048576<<"\n"<<std::flush;
  report["trials"].push_back(std::move(trial));
 }
 // Include the separate English recognizer and detector-only fallback lifecycle.
 {auto image=Read(L"test-fixtures/user/bbc-headlines-en.png");if(RecognizeBundled(image,1,{}).empty()||RecognizeBundled(image,0,{},true).empty())throw std::runtime_error("OCR modes failed");}
 {auto whole=Read(L"test-fixtures/upstream/korean-sign.jpg");auto image=Crop(whole,{0,0,whole.width/2,whole.height});whole={};for(int mode:{OcrKorean,OcrAuto})if(RecognizeBundled(image,mode,{}).empty())throw std::runtime_error("Korean lifecycle failed");}
 sampler.request_stop();sampler.join();report["final"]=Memory();report["peak_private_bytes"]=peakPrivate.load();
 if(argc>1){std::ofstream out{std::filesystem::path(argv[1])};out<<report.dump(2);if(!out)throw std::runtime_error("Cannot write memory report");}
 // Broad budgets catch retained sessions/arenas without depending on the exact
 // Windows working-set policy or transient physical-memory pressure.
 const auto initial=report["initial"]["private_bytes"].get<size_t>();
 if(report["final"]["private_bytes"].get<size_t>()>initial+64*1024*1024)throw std::runtime_error("OCR retained over 64 MiB after requests completed");
 if(peakPrivate>initial+320*1024*1024)throw std::runtime_error("OCR private-memory peak exceeded regression budget");
 std::cout<<"MemoryRegression: "<<report["final"].dump()<<" peak_private="<<peakPrivate<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<"MemoryRegression: FAIL: "<<e.what()<<"\n";return 1;}}
