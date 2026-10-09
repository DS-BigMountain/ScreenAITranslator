#include "TextDetection.h"
#include "Capture.h"
#include "common/Platform.h"
#include "BundledOcr.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

namespace sat {
namespace {
RECT TightGlyphBounds(RECT rect,const Image& image){
 // DB expands a shrunken mask for recognition. Recover glyph height on flat backgrounds
 // before grouping; expanded recognition crops must not be mistaken for line spacing.
 std::array<size_t,32768> colors{};size_t count=0;
 auto key=[](const unsigned char* p){return (int(p[0]>>3)<<10)|(int(p[1]>>3)<<5)|(p[2]>>3);};
 for(int y=rect.top;y<rect.bottom;++y)for(int x=rect.left;x<rect.right;++x){++colors[key(&image.bgra[(size_t(y)*image.width+x)*4])];++count;}
 auto mode=std::max_element(colors.begin(),colors.end());if(!count||*mode<count*.55)return rect;
 int index=int(mode-colors.begin()),b=((index>>10)&31)*8+4,g=((index>>5)&31)*8+4,r=(index&31)*8+4;
 RECT ink{rect.right,rect.bottom,rect.left,rect.top};
 for(int y=rect.top;y<rect.bottom;++y){int pixels=0,l=rect.right,rr=rect.left;
  for(int x=rect.left;x<rect.right;++x){auto p=&image.bgra[(size_t(y)*image.width+x)*4];if(std::abs(int(p[0])-b)+std::abs(int(p[1])-g)+std::abs(int(p[2])-r)>150){++pixels;l=std::min(l,x);rr=std::max(rr,x+1);}}
  if(pixels>=std::max(2,Width(rect)/500)){ink.left=std::min<LONG>(ink.left,l);ink.right=std::max<LONG>(ink.right,rr);ink.top=std::min<LONG>(ink.top,y);ink.bottom=y+1;}
 }
 return Valid(ink)&&Height(ink)>=3?ink:rect;
}
bool HasSeparator(const Image& image, RECT a, RECT b) {
 const int left=std::max(a.left,b.left),right=std::min(a.right,b.right);
 if(right-left<20)return false;
 for(int y=a.bottom+1;y<b.top-1;++y){
  int edges=0;
  for(int x=left;x<right;++x){
   const auto p=(size_t(y)*image.width+x)*4;
   int delta=0;for(int c=0;c<3;++c)delta+=std::abs(int(image.bgra[p+c])-int(image.bgra[p-image.width*4+c]));
   if(delta>24)++edges;
  }
  if(edges>.65*(right-left))return true;
 }
 return false;
}
}
std::vector<TextRegion> GroupTextLines(std::vector<TextLine> lines,const Image& image) {
 if(image.width<=0||image.height<=0||image.bgra.size()!=size_t(image.width)*image.height*4)return {};
 const RECT bounds{0,0,image.width,image.height};
 for(auto& line:lines){RECT clipped{};IntersectRect(&clipped,&line.rect,&bounds);line.rect=clipped;}
 std::erase_if(lines,[](const TextLine& l){return !Valid(l.rect)||l.text.empty();});
 // A list marker and its text can be detected as two slightly overlapping boxes.
 // Join only minor horizontal overlaps on the same baseline; columns remain separate.
 for(size_t i=0;i<lines.size();++i)for(size_t j=i+1;j<lines.size();){
  auto a=lines[i].rect,b=lines[j].rect;RECT overlap{};
  if(IntersectRect(&overlap,&a,&b)&&Height(overlap)>=.75f*std::min(Height(a),Height(b))&&Width(overlap)<.5f*std::min(Width(a),Width(b))){
   lines[i].text=a.left<b.left?lines[i].text+" "+lines[j].text:lines[j].text+" "+lines[i].text;
   UnionRect(&lines[i].rect,&a,&b);lines.erase(lines.begin()+j);
  }else ++j;
 }
 std::stable_sort(lines.begin(),lines.end(),[](const auto& a,const auto& b){return a.rect.top==b.rect.top?a.rect.left<b.rect.left:a.rect.top<b.rect.top;});
 std::vector<TextRegion> regions;
 for(const auto& line:lines){
  TextRegion* best=nullptr;int bestGap=INT_MAX;
  for(auto& region:regions){
   const auto last=region.lines.back();const int gap=line.rect.top-last.bottom;
   const float h=region.lineHeight;
   const int overlap=std::min(last.right,line.rect.right)-std::max(last.left,line.rect.left);
   if(gap<0||gap>h*1.15f||gap>=bestGap||Height(line.rect)<h*.72f||Height(line.rect)>h*1.38f)continue;
   if(std::abs(line.rect.left-last.left)>h*2||overlap<.65f*std::min(Width(last),Width(line.rect)))continue;
   if(region.text.size()+line.text.size()+1>8192||HasSeparator(image,last,line.rect))continue;
   RECT joined{};UnionRect(&joined,&region.rect,&line.rect);bool crosses=false;
   for(const auto& other:lines){
    if(&other==&line||std::any_of(region.lines.begin(),region.lines.end(),[&](RECT r){return EqualRect(&r,&other.rect);}))continue;
    RECT intersection{};if(IntersectRect(&intersection,&joined,&other.rect)){crosses=true;break;}
   }
   if(crosses)continue;
   best=&region;bestGap=gap;
  }
  if(!best){regions.push_back({0,line.rect,line.text,{line.rect},float(Height(line.rect))});continue;}
  best->text+='\n';best->text+=line.text;best->lines.push_back(line.rect);
  RECT merged{};UnionRect(&merged,&best->rect,&line.rect);best->rect=merged;
  std::vector<int> heights;for(auto r:best->lines)heights.push_back(Height(r));std::sort(heights.begin(),heights.end());best->lineHeight=float(heights[heights.size()/2]);
 }
 for(size_t i=0;i<regions.size();++i)regions[i].id=int(i+1);
 return regions;
}
static TextDetection DetectLocal(const Image& image,std::stop_token stop,int mode,bool geometryOnly) {
 CheckStop(stop);
 if(image.width<=0||image.height<=0||image.bgra.size()!=size_t(image.width)*image.height*4)return {{},"invalid-image"};
 if(mode==3)return {{},"direct-ai"};
 if(mode<0||mode>3)return {{},"invalid-mode"};
 try {
  auto lines=RecognizeBundled(image,mode,stop,geometryOnly);
  for(auto& line:lines){line.rect=TightGlyphBounds(line.rect,image);line.rect.left=std::max(0L,line.rect.left-1);line.rect.top=std::max(0L,line.rect.top-1);line.rect.right=std::min<LONG>(image.width,line.rect.right+1);line.rect.bottom=std::min<LONG>(image.height,line.rect.bottom+1);}
  auto regions=GroupTextLines(std::move(lines),image);
  size_t textBytes=0;for(const auto& r:regions)textBytes+=r.text.size();
  if(regions.size()>256||textBytes>128*1024)return {{},"too-many-regions"};
  if(regions.empty())return {{},"no-text"};
  for(auto& r:regions){if(geometryOnly)r.text.clear();int x=std::min(999,MulDiv(r.rect.left,1000,image.width)),y=std::min(999,MulDiv(r.rect.top,1000,image.height));r.imageBox={x,y,std::max(1,MulDiv(r.rect.right,1000,image.width)-x),std::max(1,MulDiv(r.rect.bottom,1000,image.height)-y)};}
  return {std::move(regions),geometryOnly?"local-geometry-ai":"bundled-ocr"};
 }catch(const Cancelled&){throw;}
 catch(const std::exception&){CheckStop(stop);return {{},"ocr-unavailable"};}
}
TextDetection DetectText(const Image& image,std::stop_token stop,int mode){return DetectLocal(image,stop,mode,false);}
TextDetection DetectTextGeometry(const Image& image,std::stop_token stop){return DetectLocal(image,stop,0,true);}

}
