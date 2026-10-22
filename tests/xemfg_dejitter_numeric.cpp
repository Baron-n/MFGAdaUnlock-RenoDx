// SPDX-License-Identifier: MIT
// Plan 4.1/4.2: a static camera with Halton jitter and jittered motion vectors
// must convert to motion ~0; true object motion must survive the conversion.
// Convention (Streamline sl_consts.h): jitterOffset is in pixels, mvecScale
// normalizes the buffer, the vector points from the current sample to the
// previous one. A static point rendered with jitter j moves by j_prev - j_cur.
#include <cmath>
#include <cstdio>
#include <vector>
namespace {
struct Float2 { float x,y; };
float Halton(unsigned index,unsigned base) {
  float f=1,r=0; for(unsigned i=index;i;i/=base) { f/=float(base); r+=f*float(i%base); } return r;
}
Float2 Jitter(unsigned frame) { return {Halton(frame%8+1,2)-.5f,Halton(frame%8+1,3)-.5f}; }
struct Case { const char* name; unsigned width,height; Float2 scale; };
// The converter under test. Until plan 4.2 the bridge forwards the game's
// vectors unchanged, which is what this placeholder reproduces.
std::vector<Float2> Convert(const std::vector<Float2>& mv,const Case&,Float2,Float2) { return mv; }
}
int main() {
  const Case cases[]={{"pixel-scale",64,36,{1.f/64,1.f/36}},{"signed-scale",64,36,{-1.f/64,1.f/36}},{"uv-units",48,48,{1.f,1.f}}};
  const Float2 object{3.25f,-1.5f}; // true motion of the moving block, pixels
  float worst=0;
  for(const auto& c:cases) for(unsigned frame=1;frame<9;++frame) {
    const Float2 prev=Jitter(frame-1),cur=Jitter(frame);
    // Buffer units: pixels = value * scale * size.
    const float toBufferX=1.f/(c.scale.x*float(c.width)),toBufferY=1.f/(c.scale.y*float(c.height));
    std::vector<Float2> mv(size_t(c.width)*c.height);
    for(unsigned y=0;y<c.height;++y) for(unsigned x=0;x<c.width;++x) {
      const bool moving=x>=8 && x<24 && y>=8 && y<20;
      const float px=(moving?object.x:0)+prev.x-cur.x,py=(moving?object.y:0)+prev.y-cur.y;
      mv[size_t(y)*c.width+x]={px*toBufferX,py*toBufferY};
    }
    const auto out=Convert(mv,c,prev,cur);
    for(unsigned y=0;y<c.height;++y) for(unsigned x=0;x<c.width;++x) {
      const bool moving=x>=8 && x<24 && y>=8 && y<20;
      const Float2 v=out[size_t(y)*c.width+x];
      const float ex=v.x/toBufferX-(moving?object.x:0),ey=v.y/toBufferY-(moving?object.y:0);
      worst=std::fmax(worst,std::fmax(std::fabs(ex),std::fabs(ey)));
    }
    if(worst>=1e-3f) { std::fprintf(stderr,"%s frame %u: residual jitter %.4f px\n",c.name,frame,worst); return 1; }
  }
  std::printf("PASS: static camera converts to zero motion, object motion preserved (max error %.2g px)\n",worst);
  return 0;
}
