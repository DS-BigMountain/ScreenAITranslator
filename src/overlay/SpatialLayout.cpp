#include "SpatialLayout.h"
#include "capture/Capture.h"
#include <algorithm>
#include <cmath>
#include <array>
namespace sat {
namespace {
RECT MapBox(const Box& b,const Image& image){
 const int x=std::clamp(b.x,0,999),y=std::clamp(b.y,0,999);
 const int right=int(std::clamp(static_cast<long long>(b.x)+b.width,static_cast<long long>(x+1),1000LL));
 const int bottom=int(std::clamp(static_cast<long long>(b.y)+b.height,static_cast<long long>(y+1),1000LL));
 RECT r{MulDiv(x,image.width,1000),MulDiv(y,image.height,1000),MulDiv(right,image.width,1000),MulDiv(bottom,image.height,1000)};
 r.left=std::min<LONG>(r.left,image.width-1);r.top=std::min<LONG>(r.top,image.height-1);
 r.right=std::min<LONG>(image.width,std::max(r.left+1,r.right));r.bottom=std::min<LONG>(image.height,std::max(r.top+1,r.bottom));return r;
}
bool Overlaps(RECT a,RECT b){RECT r{};return IntersectRect(&r,&a,&b)!=FALSE;}
struct PixelText {
 std::optional<COLORREF> color;
 std::vector<RECT> lines;
 float lineHeight{};
};
PixelText InspectTextPixels(const Image& image,RECT box){
 PixelText result;if(!Valid(box))return result;
 std::array<size_t,4096> histogram{};
 auto key=[](const unsigned char* p){return (int(p[2]>>4)<<8)|(int(p[1]>>4)<<4)|(p[0]>>4);};
 const int step=std::max(1,int(std::sqrt(double(Width(box))*Height(box)/500000.)));
 size_t sampled=0;
 for(int y=box.top;y<box.bottom;y+=step)for(int x=box.left;x<box.right;x+=step){++histogram[key(&image.bgra[(size_t(y)*image.width+x)*4])];++sampled;}
 auto mode=std::max_element(histogram.begin(),histogram.end());if(!sampled||*mode<sampled*.45)return result;
 int background=int(mode-histogram.begin());
 int br=((background>>8)&15)*16+8,bg=((background>>4)&15)*16+8,bb=(background&15)*16+8;
 std::array<size_t,4096> counts{},rs{},gs{},bs{};size_t foreground=0;
 std::vector<RECT> rows;int activeTop=-1,left=box.right,right=box.left,lastRow=-1;
 auto flush=[&]{if(activeTop>=0){if(lastRow-activeTop>=2)rows.push_back({left,activeTop,right,lastRow+1});activeTop=-1;left=box.right;right=box.left;}};
 for(int y=box.top;y<box.bottom;++y){int rowCount=0,rowLeft=box.right,rowRight=box.left;
  for(int x=box.left;x<box.right;++x){auto p=&image.bgra[(size_t(y)*image.width+x)*4];int distance=std::abs(int(p[2])-br)+std::abs(int(p[1])-bg)+std::abs(int(p[0])-bb);
   if(distance<65)continue;auto k=key(p);++counts[k];rs[k]+=p[2];gs[k]+=p[1];bs[k]+=p[0];++foreground;++rowCount;rowLeft=std::min(rowLeft,x);rowRight=x+1;
  }
  // Broad filled bars and separators are not text baselines.
  if(rowCount>=2&&rowCount<Width(box)*.85){if(activeTop<0)activeTop=y;lastRow=y;left=std::min(left,rowLeft);right=std::max(right,rowRight);}
  else if(activeTop>=0&&y-lastRow>1)flush();
 }
 flush();
 if(foreground<4||foreground>size_t(Width(box))*Height(box)*.55)return result;
 double best=0;int selected=-1;
 for(int i=0;i<4096;++i)if(counts[i]>=3){double distance=std::abs(double(rs[i])/counts[i]-br)+std::abs(double(gs[i])/counts[i]-bg)+std::abs(double(bs[i])/counts[i]-bb);double weight=counts[i]*distance*distance;if(weight>best){best=weight;selected=i;}}
 if(selected>=0)result.color=RGB(rs[selected]/counts[selected],gs[selected]/counts[selected],bs[selected]/counts[selected]);
 if(rows.empty()||rows.size()>100)return result;
 std::vector<int> heights;for(auto r:rows)heights.push_back(Height(r));std::sort(heights.begin(),heights.end());const int median=heights[heights.size()/2];
 if(median<5||median>200)return result;
 // Reject diagrams and rows with incompatible heights instead of assigning a false font size.
 if(std::count_if(heights.begin(),heights.end(),[&](int h){return h>=median*.6&&h<=median*1.5;})<int(heights.size()*.8))return result;
 for(auto r:rows){if(Height(r)<median*.5)continue;r.left=std::max(box.left,r.left-1);r.right=std::min(box.right,r.right+1);r.top=std::max(box.top,r.top-1);r.bottom=std::min(box.bottom,r.bottom+1);result.lines.push_back(r);}
 result.lineHeight=float(median);return result;
}
}
std::vector<PositionedText> LayoutPositioned(const TranslationResult& result,const Image& image,const Settings& settings,UINT dpi){
 if(image.width<=0||image.height<=0)throw AppError("layout","截图尺寸无效");
 ComPtr<IDWriteFactory> factory;CheckHR(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(factory.GetAddressOf())),"layout");
 std::vector<PositionedText> output(result.segments.size());
 std::vector<float> lineHeights(result.segments.size());
 const RECT bounds{0,0,image.width,image.height};const float scale=dpi/96.f;
 std::vector<RECT> protectedRegions=result.protectedRegions;
 for(size_t i=0;i<output.size();++i){
  const auto& s=result.segments[i];auto r=s.sourcePixels.value_or(MapBox(s.box,image));RECT clipped{};IntersectRect(&clipped,&r,&bounds);
  RECT search=clipped;
  if(!s.sourcePixels){
   int extraY=std::clamp(Height(clipped)/4,8,40),extraX=std::clamp(Width(clipped)/80,3,12);
   search={std::max(0L,clipped.left-extraX),std::max(0L,clipped.top-extraY),std::min<LONG>(image.width,clipped.right+extraX),std::min<LONG>(image.height,clipped.bottom+extraY)};
   for(size_t j=0;j<result.segments.size();++j){if(i==j)continue;const auto& adjacent=result.segments[j];auto other=adjacent.sourcePixels.value_or(MapBox(adjacent.box,image));
    if(other.left<search.right&&other.right>search.left){if(other.bottom<=clipped.top)search.top=std::max(search.top,(other.bottom+clipped.top+1)/2);if(other.top>=clipped.bottom)search.bottom=std::min(search.bottom,(clipped.bottom+other.top)/2);}
    if(other.top<search.bottom&&other.bottom>search.top){if(other.right<=clipped.left)search.left=std::max(search.left,(other.right+clipped.left+1)/2);if(other.left>=clipped.right)search.right=std::min(search.right,(clipped.right+other.left)/2);}
   }
  }
  auto pixels=InspectTextPixels(image,search);
  output[i].detectedLines=s.sourceLines;lineHeights[i]=s.sourceLineHeight;
  if(!s.sourcePixels&&!pixels.lines.empty()){
   RECT measured=pixels.lines.front();for(auto line:pixels.lines){RECT merged{};UnionRect(&merged,&measured,&line);measured=merged;}
   clipped=measured;output[i].detectedLines=pixels.lines;lineHeights[i]=pixels.lineHeight;
  }
  if(settings.matchTextColor)output[i].sourceColor=pixels.color;
  output[i].sourceRect=clipped;protectedRegions.push_back(clipped);
 }
 for(size_t i=0;i<output.size();++i){
  auto& item=output[i];const auto& segment=result.segments[i];const RECT source=item.sourceRect;
  if(!Valid(source)){item.suppressed=true;continue;}
  // Ambiguous source geometry must not erase a neighboring passage. Full reading retains its text.
  for(size_t j=0;j<output.size();++j)if(i!=j&&Overlaps(source,output[j].sourceRect))item.suppressed=true;
  auto bg=AnalyzeBackground(image,source);item.background=bg.simple?bg.color:(bg.luminance>128?RGB(247,249,252):RGB(24,30,42));
  const int pad=std::max(1,int(std::ceil(2*scale)));
  const float detectedHeight=lineHeights[i];
  const int margin=detectedHeight>0?pad+int(std::ceil(detectedHeight*.18f)):pad;
  RECT area{std::max(0L,source.left-pad),std::max(0L,source.top-margin),std::min<LONG>(image.width,source.right+pad),std::min<LONG>(image.height,source.bottom+margin)};
  // Share only the whitespace between source regions. The region's origin never migrates to another paragraph.
  for(auto other:protectedRegions){
   if(!Valid(other)||EqualRect(&other,&source))continue;
   if(Overlaps(source,other)){item.suppressed=true;continue;}
   if(other.left<area.right&&other.right>area.left){
    if(other.bottom<=source.top)area.top=std::max(area.top,(source.top+other.bottom+1)/2);
    if(other.top>=source.bottom)area.bottom=std::min(area.bottom,(source.bottom+other.top)/2);
   }
   if(other.top<area.bottom&&other.bottom>area.top){
    if(other.right<=source.left)area.left=std::max(area.left,(source.left+other.right+1)/2);
    if(other.left>=source.right)area.right=std::min(area.right,(source.right+other.left)/2);
   }
  }
  item.rect=area;item.padding=float(std::max(0,std::min(pad,(std::min(Width(area),Height(area))-1)/2)));
  const float width=std::max(1.f,Width(area)-2*item.padding),height=std::max(1.f,Height(area)-2*item.padding);
  auto measure=[&](const std::wstring& text,float fontSize,ComPtr<IDWriteTextLayout>& layout){
   ComPtr<IDWriteTextFormat> format;CheckHR(factory->CreateTextFormat(settings.font.c_str(),nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,fontSize,L"zh-CN",&format),"layout");
   CheckHR(format->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER),"layout");
   layout.Reset();CheckHR(factory->CreateTextLayout(text.c_str(),UINT32(text.size()),format.Get(),width,100000.f,&layout),"layout");
   DWRITE_TEXT_METRICS metrics{};CheckHR(layout->GetMetrics(&metrics),"layout");return metrics;
  };
  const auto text=Wide(segment.translated);
  float preferred=settings.fontSize*scale;
  if(settings.autoFont){
   if(detectedHeight>0)preferred=std::clamp(detectedHeight*1.05f,10.f,144.f);
   else{
    // Fit original text to its box when OCR is unavailable; paragraph height is not a font size.
    float low=10*scale,high=26*scale;ComPtr<IDWriteTextLayout> originalLayout;
    for(int step=0;step<10;++step){float mid=(low+high)/2;auto m=measure(Wide(segment.original),mid,originalLayout);if(m.height<=height&&m.widthIncludingTrailingWhitespace<=width)low=mid;else high=mid;}
    preferred=low;
   }
  }
  item.fontSize=preferred;auto metrics=measure(text,item.fontSize,item.layout);
  if(settings.autoFont&&(metrics.height>height||metrics.widthIncludingTrailingWhitespace>width)){
   // Bounded reduction preserves readability; overflow remains local and can be read in full view.
   float low=std::min(preferred,std::max(12*scale,preferred*.75f)),high=preferred;
   for(int step=0;step<10;++step){float mid=(low+high)/2;ComPtr<IDWriteTextLayout> trial;auto m=measure(text,mid,trial);if(m.height<=height&&m.widthIncludingTrailingWhitespace<=width)low=mid;else high=mid;}
   item.fontSize=low;metrics=measure(text,low,item.layout);
  }
  UINT32 lines{};item.layout->GetLineMetrics(nullptr,0,&lines);std::vector<DWRITE_LINE_METRICS> lineMetrics(lines);
  if(lines){CheckHR(item.layout->GetLineMetrics(lineMetrics.data(),lines,&lines),"layout");item.lineHeight=lineMetrics.front().height;}
  item.eraseHeight=float(Height(source));item.contentHeight=metrics.height;
  item.overflow=metrics.height>height+.5f||metrics.widthIncludingTrailingWhitespace>width+.5f;
 }
 return output;
}
COLORREF ReadableTextColor(COLORREF preferred,COLORREF background){
 auto luminance=[](COLORREF c){auto linear=[](int v){double s=v/255.;return s<=.04045?s/12.92:std::pow((s+.055)/1.055,2.4);};return .2126*linear(GetRValue(c))+.7152*linear(GetGValue(c))+.0722*linear(GetBValue(c));};
 double a=luminance(preferred),b=luminance(background);if((std::max(a,b)+.05)/(std::min(a,b)+.05)>=4.5)return preferred;
 return (b+.05)/.05>=1.05/(b+.05)?RGB(0,0,0):RGB(255,255,255);
}
void DrawPositioned(ID2D1RenderTarget* target,const std::vector<PositionedText>& items,COLORREF textColor,bool diagnostics){
 auto color=[](COLORREF c){return D2D1::ColorF(GetRValue(c)/255.f,GetGValue(c)/255.f,GetBValue(c)/255.f);};
 auto rect=[](RECT r){return D2D1::RectF(float(r.left),float(r.top),float(r.right),float(r.bottom));};
 ComPtr<ID2D1SolidColorBrush> brush;CheckHR(target->CreateSolidColorBrush(D2D1::ColorF(0,0,0),&brush),"layout");
 target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
 for(const auto& item:items){if(item.suppressed)continue;brush->SetColor(color(item.background));target->FillRectangle(rect(item.sourceRect),brush.Get());}
 for(const auto& item:items){
  if(item.suppressed||!item.layout)continue;
  auto box=rect(item.rect);brush->SetColor(color(item.background));target->FillRectangle(box,brush.Get());
  const float pad=item.padding;brush->SetColor(color(item.sourceColor.value_or(ReadableTextColor(textColor,item.background))));
  target->PushAxisAlignedClip(D2D1::RectF(box.left+pad,box.top+pad,box.right-pad,box.bottom-pad),D2D1_ANTIALIAS_MODE_ALIASED);
  target->DrawTextLayout(D2D1::Point2F(box.left+pad,box.top+pad-item.scroll),item.layout.Get(),brush.Get());target->PopAxisAlignedClip();
  if(item.overflow){brush->SetColor(color(RGB(67,100,235)));target->FillRectangle(D2D1::RectF(std::max(box.left,box.right-18),std::max(box.top,box.bottom-2),box.right,box.bottom),brush.Get());}
 }
 if(diagnostics)for(const auto& item:items){
  brush->SetColor(color(RGB(25,170,80)));for(auto line:item.detectedLines)target->DrawRectangle(rect(line),brush.Get(),1);
  brush->SetColor(color(RGB(230,140,0)));target->DrawRectangle(rect(item.sourceRect),brush.Get(),1);
  brush->SetColor(color(item.suppressed?RGB(220,20,40):RGB(35,90,240)));target->DrawRectangle(rect(item.rect),brush.Get(),1);
 }
}
}
