// SPDX-License-Identifier: MIT
#include "../src/addons/mfgunlock/latency_marker_policy.hpp"
#include <iostream>
#include <stdexcept>
using namespace mfgunlock::latencymarkers;
void Check(bool ok,const char* reason) { if(!ok) throw std::runtime_error(reason); }
void Complete(Admission& a,uint64_t epoch,uint32_t thread,uint32_t id) {
  Check(a.Sleep(epoch,thread,id),"source sleep rejected");
  for(unsigned m=0;m<6;++m) {
    Check(a.Marker(epoch,thread,id,m)==Decision::Forward,"marker rejected");
    Check(a.Marker(epoch,thread,id,m)==Decision::Duplicate,"alias not deduplicated");
  }
}
int main(){try {
  Admission a;
  for(uint32_t i=0;i<31;++i) Complete(a,1,7,i);
  Check(!a.Ready() && a.Consecutive()==31,"admitted before 32 complete frames");
  Complete(a,1,7,31); Check(a.Ready(),"32 frames not admitted");
  Check(!a.Sleep(1,7,31),"duplicate frame wait admitted");
  Check(a.Marker(1,8,31,6)==Decision::Invalid && !a.Ready(),"foreign thread retained admission");
  Complete(a,2,7,40); Complete(a,2,7,42); Check(a.Consecutive()==1,"gap retained consecutive count");
  Check(a.Sleep(2,7,43),"sleep");
  Check(a.Marker(2,7,43,1)==Decision::Invalid && !a.Ready(),"END without START accepted");
  Complete(a,3,7,UINT32_MAX); Complete(a,3,7,0); Check(a.Consecutive()==2,"token wrap broke sequence");
  Check(a.Marker(2,7,0,0)==Decision::Invalid && !a.Ready(),"old epoch retained admission");
  Complete(a,4,7,100); Check(!a.Sleep(4,7,90) && !a.Ready(),"regressing source token admitted");
  Check(Classify(0,0,true,true)==Decision::Invalid,"marker allowed during own sleep");
  Check(Classify(0,0,false)==Decision::Invalid,"marker allowed before sleep");
  Check(Classify(1u<<2,3,true)==Decision::Forward,"legal late submit END rejected");
  Check(Classify(0,99,true)==Decision::Invalid,"unknown marker admitted");
  std::cout<<"PASS: 32-frame admission, alias dedup, thread/epoch invalidation, gaps, END order, token wrap\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
