#include "BundledOcr.h"
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <thread>

namespace sat {
namespace {
std::filesystem::path AppDirectory() {
 std::wstring path(32768,L'\0');const auto n=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
 if(!n||n>=path.size())throw AppError("ocr","Cannot locate application directory");
 path.resize(n);return std::filesystem::path(path).parent_path();
}
struct Model {
 Ort::Session session{nullptr};std::string input,output;std::vector<std::string> alphabet;
 Model(Ort::Env& env,Ort::SessionOptions& options,const std::filesystem::path& path,bool recognition)
  :session(env,path.c_str(),options){
  Ort::AllocatorWithDefaultOptions allocator;
  input=session.GetInputNameAllocated(0,allocator).get();output=session.GetOutputNameAllocated(0,allocator).get();
  if(recognition){
   auto dictionary=session.GetModelMetadata().LookupCustomMetadataMapAllocated("character",allocator);
   if(!dictionary)throw AppError("ocr","Missing recognition dictionary");
   alphabet.push_back("");std::istringstream stream(dictionary.get());std::string line;
   while(std::getline(stream,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();alphabet.push_back(line);}
   alphabet.push_back(" ");
  }
 }
 Ort::Value Run(std::vector<float>& pixels,int width,int height,Ort::RunOptions& options){
  std::array<int64_t,4> shape{1,3,height,width};auto memory=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
  auto tensor=Ort::Value::CreateTensor<float>(memory,pixels.data(),pixels.size(),shape.data(),shape.size());
  const char* ins[]{input.c_str()};const char* outs[]{output.c_str()};
  auto values=session.Run(options,ins,&tensor,1,outs,1);return std::move(values[0]);
 }
};
struct Engine {
 // Keep the module loaded for the process lifetime: ORT owns process-wide thread pools.
 HMODULE module{};std::unique_ptr<Ort::Env> env;std::unique_ptr<Ort::SessionOptions> options;
 std::unique_ptr<Model> detector,japanese,english;
 Engine(){
  const auto root=AppDirectory();module=LoadLibraryExW((root/L"onnxruntime.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
  if(!module)throw AppError("ocr","Bundled OCR runtime unavailable");
  auto getApi=reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(GetProcAddress(module,"OrtGetApiBase"));
  const auto api=getApi?getApi()->GetApi(ORT_API_VERSION):nullptr;
  if(!api)throw AppError("ocr","Bundled OCR runtime incompatible");
  Ort::InitApi(api);env=std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR,"ScreenAITranslator");options=std::make_unique<Ort::SessionOptions>();
  options->SetIntraOpNumThreads(4);options->SetInterOpNumThreads(1);options->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  options->DisableMemPattern();
  detector=std::make_unique<Model>(*env,*options,root/L"models/det.onnx",false);
 }
 Model& Recognizer(int mode){
  auto& model=mode==1?english:japanese;
  if(!model)model=std::make_unique<Model>(*env,*options,AppDirectory()/(mode==1?L"models/rec-en.onnx":L"models/rec-ja.onnx"),true);
  return *model;
 }
};
// Bilinear BGR input follows the model's Paddle/RapidOCR preprocessing convention.
std::vector<float> Tensor(const Image& image,RECT crop,int width,int height,int contentWidth,bool detection){
 std::vector<float> result(size_t(width)*height*3,0.f);
 constexpr float mean[]{.485f,.456f,.406f},stddev[]{.229f,.224f,.225f};
 for(int y=0;y<height;++y){
  float sy=std::clamp(crop.top+(y+.5f)*Height(crop)/height-.5f,float(crop.top),float(crop.bottom-1));int y0=int(sy),y1=std::min(y0+1,int(crop.bottom-1));float fy=sy-y0;
  for(int x=0;x<contentWidth;++x){
   float sx=std::clamp(crop.left+(x+.5f)*Width(crop)/contentWidth-.5f,float(crop.left),float(crop.right-1));int x0=int(sx),x1=std::min(x0+1,int(crop.right-1));float fx=sx-x0;
   for(int c=0;c<3;++c){
    auto sample=[&](int px,int py){return float(image.bgra[(size_t(py)*image.width+px)*4+c]);};
    float pixel=((sample(x0,y0)*(1-fx)+sample(x1,y0)*fx)*(1-fy)+(sample(x0,y1)*(1-fx)+sample(x1,y1)*fx)*fy)/255.f;
    result[(size_t(c)*height+y)*width+x]=detection?(pixel-mean[c])/stddev[c]:pixel*2-1;
   }
  }
 }
 return result;
}
std::vector<RECT> Boxes(const Ort::Value& output,const Image& image){
 auto shape=output.GetTensorTypeAndShapeInfo().GetShape();
 if(shape.size()!=4||shape[0]!=1||shape[1]!=1||shape[2]<=0||shape[3]<=0||shape[2]*shape[3]>4096*4096)throw AppError("ocr","Invalid detector output");
 int h=int(shape[2]),w=int(shape[3]);const float* map=output.GetTensorData<float>();
 std::vector<unsigned char> seen(size_t(w)*h);std::vector<int> pending;std::vector<RECT> boxes;
 for(int p=0;p<w*h;++p){
  if(seen[p]||map[p]<.3f)continue;
  pending.clear();pending.push_back(p);seen[p]=1;int l=p%w,r=l,t=p/w,b=t;double score=0;
  for(size_t i=0;i<pending.size();++i){
   int at=pending[i],x=at%w,y=at/w;l=std::min(l,x);r=std::max(r,x);t=std::min(t,y);b=std::max(b,y);score+=map[at];
   for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){int nx=x+dx,ny=y+dy;if(nx<0||nx>=w||ny<0||ny>=h)continue;int next=ny*w+nx;if(!seen[next]&&map[next]>=.3f){seen[next]=1;pending.push_back(next);}}
  }
  if(pending.size()<6||score/pending.size()<.55||b-t<2||r-l<2)continue;
  if(boxes.size()>=512)throw AppError("ocr","Too many text regions");
  // DB's shrunken text mask is expanded along both axes for horizontal text.
  float pad=float(pending.size())*1.6f/(2*((r-l+1)+(b-t+1)));
  RECT rect{LONG(std::floor((l-pad)*image.width/w)),LONG(std::floor((t-pad)*image.height/h)),LONG(std::ceil((r+1+pad)*image.width/w)),LONG(std::ceil((b+1+pad)*image.height/h))};
  rect.left=std::max(0L,rect.left);rect.top=std::max(0L,rect.top);rect.right=std::min<LONG>(image.width,rect.right);rect.bottom=std::min<LONG>(image.height,rect.bottom);
  if(Width(rect)<3||Height(rect)<5)continue;
  if(Height(rect)>Width(rect)*2)throw AppError("ocr","Vertical text requires vision translation");
  boxes.push_back(rect);
 }
 // Recognition expansion can touch adjacent baselines. Split the shared margin at
 // their centre midpoint so a crop cannot import ascenders/descenders from its neighbour.
 const auto expanded=boxes;
 for(size_t i=0;i<boxes.size();++i)for(size_t j=0;j<boxes.size();++j){
  if(i==j)continue;auto a=expanded[i],b=expanded[j];int overlap=std::min(a.right,b.right)-std::max(a.left,b.left);
  int ca=(a.top+a.bottom)/2,cb=(b.top+b.bottom)/2,delta=cb-ca;
  if(overlap<std::min(Width(a),Width(b))*.5f||std::abs(delta)<std::min(Height(a),Height(b))*.5f)continue;
  if(a.bottom>b.top&&b.bottom>a.top){int split=(ca+cb)/2;if(delta>0)boxes[i].bottom=std::min<LONG>(boxes[i].bottom,split);else boxes[i].top=std::max<LONG>(boxes[i].top,split);}
 }
 std::sort(boxes.begin(),boxes.end(),[](RECT a,RECT b){return a.top==b.top?a.left<b.left:a.top<b.top;});return boxes;
}
std::pair<std::string,float> Decode(const Ort::Value& output,const std::vector<std::string>& alphabet){
 auto shape=output.GetTensorTypeAndShapeInfo().GetShape();
 if(shape.size()!=3||shape[0]!=1||shape[1]<1||shape[2]!=int64_t(alphabet.size()))throw AppError("ocr","Recognition dictionary mismatch");
 const float* data=output.GetTensorData<float>();std::string text;int64_t previous=-1;double confidence=0;size_t count=0;
 for(int64_t t=0;t<shape[1];++t){auto row=data+t*shape[2];auto best=std::max_element(row,row+shape[2]);int64_t index=best-row;
  if(index&&index!=previous){text+=alphabet[size_t(index)];confidence+=*best;++count;}previous=index;
 }
 return {text,count?float(confidence/count):0.f};
}
}
std::vector<TextLine> RecognizeBundled(const Image& image,int mode,std::stop_token stop,bool geometryOnly){
 static std::timed_mutex mutex;static std::unique_ptr<Engine> engine;
 std::unique_lock lock(mutex,std::defer_lock);while(!lock.try_lock_for(std::chrono::milliseconds(20)))CheckStop(stop);CheckStop(stop);
 if(!engine)engine=std::make_unique<Engine>();auto* recognizer=geometryOnly?nullptr:&engine->Recognizer(mode);CheckStop(stop);
 Ort::RunOptions run;std::stop_callback cancel(stop,[&]{run.SetTerminate();});
 std::atomic<bool> timedOut=false;std::mutex timerMutex;std::condition_variable_any timerCv;
 std::jthread watchdog([&](std::stop_token done){std::unique_lock timerLock(timerMutex);timerCv.wait_for(timerLock,done,std::chrono::seconds(15),[]{return false;});if(!done.stop_requested()){timedOut=true;run.SetTerminate();}});
 try {
  float scale=std::min(1.f,1280.f/std::max(image.width,image.height));
  int w=std::max(32,int(std::round(image.width*scale/32))*32),h=std::max(32,int(std::round(image.height*scale/32))*32);
  auto input=Tensor(image,{0,0,image.width,image.height},w,h,w,true);auto det=engine->detector->Run(input,w,h,run);auto boxes=Boxes(det,image);
  std::vector<TextLine> lines;size_t rejected=0;
  for(auto box:boxes){
   CheckStop(stop);if(geometryOnly){lines.push_back({box," "});continue;}int contentWidth=std::max(8,int(std::ceil(48.f*Width(box)/Height(box))));
   if(contentWidth>4096){++rejected;continue;}int width=std::max(320,contentWidth);
   auto pixels=Tensor(image,box,width,48,contentWidth,false);auto output=recognizer->Run(pixels,width,48,run);auto [text,score]=Decode(output,recognizer->alphabet);
   if(score<.65f||text.empty()){++rejected;continue;}lines.push_back({box,std::move(text)});
  }
  if(timedOut)throw AppError("ocr","OCR time limit exceeded");CheckStop(stop);
  // Low confidence must not silently constrain vision translation to incomplete OCR IDs.
  if(rejected>std::max<size_t>(2,boxes.size()/5))throw AppError("ocr","OCR confidence insufficient");
  return lines;
 }catch(const Ort::Exception&){CheckStop(stop);throw AppError("ocr",timedOut?"OCR time limit exceeded":"Bundled OCR inference failed");}
}
}
