#include "capture/TextDetection.h"
#include "common/Platform.h"
#include <wincodec.h>
#include <iostream>
#include <thread>
#include <algorithm>
using namespace sat;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
Image Read(const wchar_t* path){
 ComPtr<IWICImagingFactory> f;CheckHR(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)),"test");
 ComPtr<IWICBitmapDecoder> decoder;CheckHR(f->CreateDecoderFromFilename(path,nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder),"test");
 ComPtr<IWICBitmapFrameDecode> frame;CheckHR(decoder->GetFrame(0,&frame),"test");ComPtr<IWICFormatConverter> converter;CheckHR(f->CreateFormatConverter(&converter),"test");
 CheckHR(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"test");UINT w{},h{};converter->GetSize(&w,&h);
 Image image{int(w),int(h),std::vector<unsigned char>(size_t(w)*h*4)};CheckHR(converter->CopyPixels(nullptr,w*4,UINT(image.bgra.size()),image.bgra.data()),"test");return image;
}
std::string Text(const TextDetection& result){std::string text;for(const auto& r:result.regions)text+=r.text+'\n';return text;}
}
int wmain(int argc,wchar_t** argv){try{
 ComScope com;auto english=Read(L"test-fixtures/installer-en.png");
 if(argc==2&&std::wstring(argv[1])==L"--missing-recognizer"){
  Require(DetectText(english).regions.empty(),"Missing recognizer unexpectedly succeeded");auto geometry=DetectTextGeometry(english);Require(geometry.status=="local-geometry-ai"&&!geometry.regions.empty(),"AI fallback lost available detector geometry");for(const auto& r:geometry.regions)Require(r.text.empty()&&r.imageBox.width>0&&r.lineHeight>0,"AI geometry carried invented OCR text");std::cout<<"Detector-only fallback: PASS\n";return 0;
 }
 if(argc==2&&std::wstring(argv[1])==L"--missing-assets"){
  auto result=DetectText(english);Require(result.regions.empty()&&result.status=="ocr-unavailable","Missing assets did not produce safe fallback");
  Require(DetectText(english,{},3).status=="direct-ai","Direct AI attempted to load missing OCR");
  std::cout<<"Missing OCR assets: safe fallback PASS\n";return 0;
 }
 for(int mode:{0,1}){
  auto start=std::chrono::steady_clock::now();auto result=DetectText(english,{},mode);auto text=Text(result);
  Require(result.status=="bundled-ocr"&&text.find("Additional Tasks")!=std::string::npos&&text.find("desktop shortcut")!=std::string::npos,"Bundled English recognition failed");
  std::cout<<"English mode "<<mode<<": "<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()<<" ms\n";
 }
 auto geometry=DetectTextGeometry(english);Require(!geometry.regions.empty()&&geometry.status=="local-geometry-ai","Geometry-only AI path failed");for(const auto& r:geometry.regions)Require(r.text.empty()&&r.imageBox.width>0,"Geometry-only path ran recognition");
 auto japanese=Read(L"test-fixtures/news-ja.png");auto result=DetectText(japanese,{},2);
 Require(result.status=="bundled-ocr"&&Text(result).find("1200")!=std::string::npos&&Text(result).find("古代")!=std::string::npos,"Bundled Japanese recognition failed");
 Image blank{500,200,std::vector<unsigned char>(500*200*4,255)};
 Require(DetectText(blank).status=="no-text","Blank image generated OCR regions");
 Require(DetectText(japanese,{},3).status=="direct-ai","Explicit AI path ignored");
 Require(DetectText(japanese,{},99).status=="invalid-mode","Invalid OCR mode accepted");
 std::stop_source source;std::jthread cancel([&]{std::this_thread::sleep_for(std::chrono::milliseconds(10));source.request_stop();});bool cancelled=false;
 try{DetectText(japanese,source.get_token());}catch(const Cancelled&){cancelled=true;}Require(cancelled,"In-flight OCR cancellation ignored");
 Require(DetectText(english,{},1).status=="bundled-ocr","Cancelled run poisoned subsequent inference");
 std::cout<<"BundledOcrTests: PASS (English, Japanese, blank, direct AI, cancellation, recovery)\n";return 0;
 }catch(const std::exception& e){std::cerr<<"BundledOcrTests: FAIL: "<<e.what()<<"\n";return 1;}}
