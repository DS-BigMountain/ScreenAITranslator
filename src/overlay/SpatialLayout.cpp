#include "SpatialLayout.h"
#include "capture/Capture.h"
#include <algorithm>
#include <cmath>
namespace sat {
namespace {
RECT MapBox(const Box& b,const Image& image){
 const int x=std::clamp(b.x,0,999),y=std::clamp(b.y,0,999);
 const int right=static_cast<int>(std::clamp(static_cast<long long>(b.x)+b.width,static_cast<long long>(x+1),1000LL));
 const int bottom=static_cast<int>(std::clamp(static_cast<long long>(b.y)+b.height,static_cast<long long>(y+1),1000LL));
 RECT r{MulDiv(x,image.width,1000),MulDiv(y,image.height,1000),MulDiv(right,image.width,1000),MulDiv(bottom,image.height,1000)};
 r.right=std::min<LONG>(image.width,std::max(r.left+1,r.right));r.bottom=std::min<LONG>(image.height,std::max(r.top+1,r.bottom));return r;
}
bool Overlaps(RECT a,RECT b){RECT overlap{};return IntersectRect(&overlap,&a,&b)&&Width(overlap)>1&&Height(overlap)>1;}
}
std::vector<PositionedText> LayoutPositioned(const TranslationResult& result,const Image& image,const Settings& settings,UINT dpi){
 ComPtr<IDWriteFactory> factory;CheckHR(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(factory.GetAddressOf())),"layout");
 std::vector<PositionedText> output(result.segments.size());
 std::vector<size_t> order;for(size_t i=0;i<output.size();++i){output[i].sourceRect=MapBox(result.segments[i].box,image);order.push_back(i);}
 std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return output[a].sourceRect.top<output[b].sourceRect.top;});
 std::vector<RECT> placed;
 const float scale=dpi/96.f;const int pad=std::max(2,int(std::ceil(3*scale)));
 for(auto index:order){
  const auto& segment=result.segments[index];auto& item=output[index];auto anchor=item.sourceRect;
  auto bg=AnalyzeBackground(image,anchor);item.background=bg.simple?bg.color:(bg.luminance>128?RGB(247,249,252):RGB(24,30,42));
  item.padding=float(pad);item.fontSize=settings.autoFont?std::clamp(float(Height(anchor))*.68f,14*scale,26*scale):settings.fontSize*scale;
  auto text=Wide(segment.translated);
  auto measure=[&](int width){
   ComPtr<IDWriteTextFormat> format;CheckHR(factory->CreateTextFormat(settings.font.c_str(),nullptr,DWRITE_FONT_WEIGHT_MEDIUM,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,item.fontSize,L"zh-CN",&format),"layout");
   CheckHR(format->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER),"layout");
   item.layout.Reset();CheckHR(factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format.Get(),float(std::max(1,width-2*pad)),float(image.height),&item.layout),"layout");
   DWRITE_TEXT_METRICS metrics{};CheckHR(item.layout->GetMetrics(&metrics),"layout");return metrics;
  };
  // Give every cover the measured glyph height, even when the model box is a thin strip.
  int width=std::min(image.width,std::max(Width(anchor),int(std::ceil(item.fontSize))+2*pad));
  auto metrics=measure(width);
  if(metrics.height+2*pad>Height(anchor)){
   int limit=std::min(image.width,std::max(width,std::min(image.width-int(anchor.left),width+int(160*scale))));
   for(size_t j=0;j<output.size();++j){if(j==index)continue;auto other=output[j].sourceRect;
    if(other.top<anchor.bottom&&other.bottom>anchor.top&&other.left>=anchor.right)limit=std::min(limit,std::max(width,int(other.left-anchor.left)-pad));
   }
   width=limit;metrics=measure(width);
  }
  // Prefer nearby vertical whitespace over moving a label into another column.
  auto place=[&](int w,int h)->std::optional<RECT>{
   if(w>image.width||h>image.height)return {};
   std::vector<int> xs{std::clamp(int(anchor.left),0,image.width-w),0,image.width-w};
   std::vector<int> ys{std::clamp(int(anchor.top),0,image.height-h),0,image.height-h};
   for(auto r:placed){xs.push_back(r.right+pad);xs.push_back(r.left-pad-w);ys.push_back(r.bottom+pad);ys.push_back(r.top-pad-h);}
   std::sort(xs.begin(),xs.end());xs.erase(std::unique(xs.begin(),xs.end()),xs.end());std::sort(ys.begin(),ys.end());ys.erase(std::unique(ys.begin(),ys.end()),ys.end());
   std::optional<RECT> best;long long bestScore=LLONG_MAX;
   for(int y:ys)for(int x:xs){if(x<0||y<0||x+w>image.width||y+h>image.height)continue;RECT r{x,y,x+w,y+h};
    long long score=4LL*std::abs(x-anchor.left)+std::abs(y-anchor.top);if(y<anchor.top)score+=std::abs(y-anchor.top);if(score>=bestScore)continue;
    bool collision=false;for(auto previous:placed){InflateRect(&previous,pad,pad);if(Overlaps(r,previous)){collision=true;break;}}if(collision)continue;
    bestScore=score;best=r;if(score==0)return best;
   }return best;
  };
  int height=std::max(Height(anchor),int(std::ceil(metrics.height))+2*pad);
  auto rect=place(width,height);
  if(!rect){width=image.width;metrics=measure(width);height=std::max(Height(anchor),int(std::ceil(metrics.height))+2*pad);rect=place(width,height);}
  // An exceptionally long block uses a local scroll viewport without changing mode.
  if(!rect){for(int h=std::min(height,image.height);h>=int(std::ceil(item.fontSize*1.5f))+2*pad;h-=std::max(1,pad))if((rect=place(width,h)))break;}
  if(!rect){int h=std::min(height,image.height),x=std::clamp(int(anchor.left),0,image.width-width),y=std::clamp(int(anchor.top),0,image.height-h);rect=RECT{x,y,x+width,y+h};}
  UINT32 lines{};item.layout->GetLineMetrics(nullptr,0,&lines);std::vector<DWRITE_LINE_METRICS> lineMetrics(lines);if(lines){CheckHR(item.layout->GetLineMetrics(lineMetrics.data(),lines,&lines),"layout");item.lineHeight=lineMetrics.front().height;}
  item.eraseHeight=std::max(float(Height(anchor)),item.lineHeight+2*pad);
  if(!segment.original.empty()){
   auto original=Wide(segment.original);ComPtr<IDWriteTextLayout> sourceLayout;
   CheckHR(factory->CreateTextLayout(original.c_str(),static_cast<UINT32>(original.size()),item.layout.Get(),float(std::max(1,Width(anchor))),float(image.height),&sourceLayout),"layout");
   DWRITE_TEXT_METRICS sourceMetrics{};CheckHR(sourceLayout->GetMetrics(&sourceMetrics),"layout");item.eraseHeight=std::max(item.eraseHeight,sourceMetrics.height+2*pad);
  }
  item.rect=*rect;item.contentHeight=metrics.height;item.overflow=metrics.height+2*pad>Height(item.rect)+.5f;
  placed.push_back(item.rect);
 }
 return output;
}
COLORREF ReadableTextColor(COLORREF preferred,COLORREF background){
 auto luminance=[](COLORREF c){auto linear=[](int v){double s=v/255.;return s<=.04045?s/12.92:std::pow((s+.055)/1.055,2.4);};return .2126*linear(GetRValue(c))+.7152*linear(GetGValue(c))+.0722*linear(GetBValue(c));};
 double a=luminance(preferred),b=luminance(background);if((std::max(a,b)+.05)/(std::min(a,b)+.05)>=4.5)return preferred;
 return (b+.05)/.05>=1.05/(b+.05)?RGB(0,0,0):RGB(255,255,255);
}
}

