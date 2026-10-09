// Manual benchmark: no CTest registration; network access requires --saved-settings.
// The production transport seam records attempt counts and numeric token usage only.
#include "../src/providers/Provider.cpp"
#include "settings/Store.h"
#include "overlay/SpatialLayout.h"
#include <wincodec.h>
#include <fstream>
#include <iostream>
#include <iomanip>
using namespace sat;
namespace {
using BenchClock=std::chrono::steady_clock;
double Milliseconds(BenchClock::time_point start){return std::chrono::duration<double,std::milli>(BenchClock::now()-start).count();}
Image ReadImage(const std::filesystem::path& path){
 ComPtr<IWICImagingFactory> f;CheckHR(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)),"benchmark");
 ComPtr<IWICBitmapDecoder> d;CheckHR(f->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&d),"benchmark");
 ComPtr<IWICBitmapFrameDecode> frame;CheckHR(d->GetFrame(0,&frame),"benchmark");ComPtr<IWICFormatConverter> c;CheckHR(f->CreateFormatConverter(&c),"benchmark");
 CheckHR(c->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"benchmark");UINT w{},h{};c->GetSize(&w,&h);
 if(!w||!h||uint64_t(w)*h>100000000)throw AppError("benchmark","Invalid image dimensions");
 Image image{int(w),int(h),std::vector<unsigned char>(size_t(w)*h*4)};CheckHR(c->CopyPixels(nullptr,w*4,UINT(image.bgra.size()),image.bgra.data()),"benchmark");return image;
}
void WriteJson(const std::filesystem::path& path,const Json& value){std::ofstream out(path);out<<value.dump(2);if(!out)throw AppError("benchmark","Cannot save benchmark output");}
void Render(const Image& image,const std::vector<PositionedText>& items,const std::filesystem::path& path){
 ComPtr<IWICImagingFactory> wic;CheckHR(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)),"benchmark");
 ComPtr<IWICBitmap> bitmap;CheckHR(wic->CreateBitmap(image.width,image.height,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&bitmap),"benchmark");
 ComPtr<ID2D1Factory> factory;CheckHR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()),"benchmark");
 ComPtr<ID2D1RenderTarget> target;CheckHR(factory->CreateWicBitmapRenderTarget(bitmap.Get(),D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96),&target),"benchmark");
 ComPtr<ID2D1Bitmap> background;CheckHR(target->CreateBitmap(D2D1::SizeU(image.width,image.height),image.bgra.data(),image.width*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE)),&background),"benchmark");
 target->BeginDraw();target->DrawBitmap(background.Get());DrawPositioned(target.Get(),items,RGB(20,20,20));CheckHR(target->EndDraw(),"benchmark");
 ComPtr<IWICStream> stream;CheckHR(wic->CreateStream(&stream),"benchmark");CheckHR(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"benchmark");ComPtr<IWICBitmapEncoder> encoder;CheckHR(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"benchmark");CheckHR(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"benchmark");ComPtr<IWICBitmapFrameEncode> frame;CheckHR(encoder->CreateNewFrame(&frame,nullptr),"benchmark");CheckHR(frame->Initialize(nullptr),"benchmark");CheckHR(frame->WriteSource(bitmap.Get(),nullptr),"benchmark");CheckHR(frame->Commit(),"benchmark");CheckHR(encoder->Commit(),"benchmark");
}
Json Rectangle(RECT r){return Json::array({r.left,r.top,r.right,r.bottom});}
}
int wmain(int argc,wchar_t** argv){
 if(argc<7||std::wstring(argv[1])!=L"--saved-settings"){
  std::cerr<<"Usage: TranslationBenchmark --saved-settings OUTPUT_DIR ROUNDS TIMEOUT_SECONDS_OR_0 IMAGE1 IMAGE2 [IMAGE3 ...]\n";return 2;
 }
 std::string key;struct Wipe{std::string& value;~Wipe(){if(!value.empty())SecureZeroMemory(value.data(),value.size());}} wipe{key};
 try{
  ComScope com;SetConsoleOutputCP(CP_UTF8);Store store;auto settings=store.LoadSettings();const auto savedTimeout=settings.timeoutSeconds;
  const int rounds=std::stoi(argv[3]),timeout=std::stoi(argv[4]);if(rounds<1||rounds>10||timeout<0||timeout>300)throw AppError("benchmark","Invalid run limits");
  if(timeout)settings.timeoutSeconds=timeout;
  settings.contextEnabled=false;settings.spatialOverlay=true;key=UnprotectSecret(settings.encryptedKey);if(key.empty())throw AppError("benchmark","No saved API key; no requests made");
  if(settings.provider!="deepseek"&&settings.provider!="openai"&&settings.provider!="openai-compatible"&&settings.provider!="openai_compatible")throw AppError("benchmark","Unsupported provider");
  std::filesystem::path output=argv[2];std::filesystem::create_directories(output);
  if(std::filesystem::exists(output/L"trials.jsonl"))throw AppError("benchmark","Output already has trials; choose a new directory");
  std::vector<Image> images;Json inputs=Json::array();for(int i=5;i<argc;++i){images.push_back(ReadImage(argv[i]));inputs.push_back({{"image",i-4},{"file",Utf8(std::filesystem::path(argv[i]).filename().wstring())},{"width",images.back().width},{"height",images.back().height}});}
  SYSTEM_INFO cpu{};GetNativeSystemInfo(&cpu);
  WriteJson(output/L"metadata.json",{{"provider",settings.provider},{"model",settings.model},{"quality",settings.quality},{"thinking_high",settings.thinkingHigh},{"timeout_seconds",settings.timeoutSeconds},{"saved_timeout_seconds",savedTimeout},{"context_enabled",false},{"rounds",rounds},{"logical_processors",cpu.dwNumberOfProcessors},{"inputs",inputs},{"timing_definition","Image already in memory; JPEG/base64 + OCR when applicable + API including retries + BuildOverlay. PNG export and disk I/O excluded. Original image provided to both paths. Alternating order by image and round; first OCR run is process-cold."}});
  std::ofstream records(output/L"trials.jsonl");bool firstOcr=true;std::vector<bool> preview(images.size()*2,false);int failed=0;
  for(int round=1;round<=rounds;++round)for(size_t i=0;i<images.size();++i)for(int slot=0;slot<2;++slot){
   const bool ocr=((round+int(i)+slot)%2==0);const auto& image=images[i];
   const std::string pathName=ocr?"ocr_image_ai":"direct_ai";
   std::string stem="image"+std::to_string(i+1)+"-round"+std::to_string(round)+"-"+pathName;
   Json row={{"image",i+1},{"round",round},{"path",pathName},{"order_in_pair",slot+1},{"ocr_process_cold",ocr&&firstOcr},{"attempts",Json::array()}};
   std::cout<<"START "<<stem<<" timeout="<<settings.timeoutSeconds<<"s\n"<<std::flush;
   auto start=BenchClock::now();double preprocess=0,ocrMs=0,apiMs=0,layoutMs=0;auto stage=start;std::string phase="preprocess";TextDetection detected;
   try{
    auto encoded=EncodeJpeg(image,settings.quality,{});auto data=Base64(encoded);preprocess=Milliseconds(stage);row["jpeg_bytes"]=encoded.size();
    phase="ocr";stage=BenchClock::now();if(ocr){detected=DetectText(image);ocrMs=Milliseconds(stage);firstOcr=false;row["ocr_status"]=detected.status;row["ocr_regions"]=detected.regions.size();
     if(detected.regions.empty())throw AppError("benchmark","Local OCR unavailable; refusing to label a direct-AI fallback as OCR");
    }
    CompatibleProvider provider(settings.provider=="deepseek",[&](const Settings& options,const std::string& secret,const std::string& body,Clock::time_point deadline,std::stop_token stop){
     const auto before=BenchClock::now();Json attempt={{"request_bytes",body.size()}};
     try{auto response=SendHttp(options,secret,body,deadline,stop);attempt["ms"]=Milliseconds(before);attempt["http_status"]=response.status;
      auto envelope=Json::parse(response.body,nullptr,false);if(envelope.is_object()&&envelope.contains("usage")&&envelope["usage"].is_object()){
       const auto& usage=envelope["usage"];for(const auto* field:{"prompt_tokens","completion_tokens","total_tokens","prompt_cache_hit_tokens","prompt_cache_miss_tokens"})if(usage.contains(field)&&usage[field].is_number())attempt[field]=usage[field];
       for(const auto* field:{"prompt_tokens_details","completion_tokens_details"})if(usage.contains(field)&&usage[field].is_object())for(auto it=usage[field].begin();it!=usage[field].end();++it)if(it.value().is_number())attempt[std::string(field)+"."+it.key()]=it.value();
      }
      row["attempts"].push_back(attempt);return response;
     }catch(...){attempt["ms"]=Milliseconds(before);attempt["transport_error"]=true;row["attempts"].push_back(attempt);throw;}
    });
    phase="api";stage=BenchClock::now();auto translated=provider.TranslateLocated(data,{},settings,key,{},detected.regions);apiMs=Milliseconds(stage);
    phase="layout";stage=BenchClock::now();RECT rect{0,0,image.width,image.height};Monitor monitor{L"benchmark",rect,96};auto blocks=BuildOverlay(translated,image,rect,monitor,settings);layoutMs=Milliseconds(stage);
    row["total_ms"]=Milliseconds(start);row["success"]=true;row["segments"]=translated.segments.size();row["original_chars"]=0;row["translated_chars"]=0;row["suppressed"]=0;row["overflow"]=0;
    Json outputSegments=Json::array();int sourceCount=0,targetCount=0,suppressed=0,overflow=0;
    for(size_t s=0;s<translated.segments.size();++s){const auto& segment=translated.segments[s];sourceCount+=int(Wide(segment.original).size());targetCount+=int(Wide(segment.translated).size());Json value={{"original",segment.original},{"translated",segment.translated},{"source_id",segment.sourceId},{"model_box",{segment.box.x,segment.box.y,segment.box.width,segment.box.height}}};
     if(!blocks.empty()){const auto& p=blocks[0].positioned[s];value["source_rect"]=Rectangle(p.sourceRect);value["layout_rect"]=Rectangle(p.rect);value["suppressed"]=p.suppressed;value["overflow"]=p.overflow;suppressed+=p.suppressed;overflow+=p.overflow;}
     outputSegments.push_back(value);
    }
    row["original_chars"]=sourceCount;row["translated_chars"]=targetCount;row["suppressed"]=suppressed;row["overflow"]=overflow;
    WriteJson(output/(stem+".json"),outputSegments);
    const size_t index=i*2+(ocr?1:0);if(!preview[index]&&!blocks.empty()){Render(image,blocks[0].positioned,output/("image"+std::to_string(i+1)+"-"+pathName+".png"));preview[index]=true;}
   }catch(const AppError& e){
    if(phase=="api")apiMs=Milliseconds(stage);else if(phase=="ocr")ocrMs=Milliseconds(stage);else if(phase=="layout")layoutMs=Milliseconds(stage);
    row["success"]=false;row["total_ms"]=Milliseconds(start);row["error_stage"]=e.stage;row["error"]=Sanitize(e.what(),key);++failed;
    if(e.stage=="Authentication"||e.stage=="Vision"||e.status==429){records<<row.dump()<<'\n'<<std::flush;throw;}
   }
   row["preprocess_ms"]=preprocess;row["ocr_ms"]=ocrMs;row["api_ms"]=apiMs;row["layout_ms"]=layoutMs;
   records<<row.dump()<<'\n'<<std::flush;
   std::cout<<"END "<<stem<<" success="<<row["success"]<<" total_ms="<<row["total_ms"]<<" ocr_ms="<<ocrMs<<" api_ms="<<apiMs<<" attempts="<<row["attempts"].size()<<"\n"<<std::flush;
  }
  std::cout<<"COMPLETE failures="<<failed<<"\n";return 0;
 }catch(const AppError& e){std::cerr<<"Benchmark stopped ["<<e.stage<<"]: "<<Sanitize(e.what(),key)<<"\n";return 1;}
 catch(...){std::cerr<<"Benchmark stopped; configuration and credentials were not changed.\n";return 1;}
}
