#include <windows.h>
#include <iostream>
#include "../src/addons/mfgunlock/xemfg/backend_api.hpp"
int main(int argc,char** argv) {
  auto host=LoadLibraryW(L"xemfg_host_fixture.dll");
  auto plugin=LoadLibraryW(L"MFGAdaUnlock.XeMFG.asi");
  if(!host || !plugin) { std::cerr<<"Load fixture/plugin failed: "<<GetLastError()<<'\n'; return 1; }
  auto init=reinterpret_cast<void(*)()>(GetProcAddress(plugin,"InitializeASI"));
  auto run=reinterpret_cast<int(*)()>(GetProcAddress(host,"RunFixture"));
  if(!init || !run) return 2;
  init();
  auto get=reinterpret_cast<mfgunlock::xemfg::GetStatus>(GetProcAddress(plugin,"MFGAXeGetStatus"));
  mfgunlock::xemfg::Status status;
  if(!get || !get(&status,sizeof(status))) return 3;
  std::cout<<"startup="<<status.detail<<std::endl;
  if(argc>1) return status.hooks==0 ? 0:4; // inactive or refused pair: no hooks allowed
  const auto result=run();
  get(&status,sizeof(status));
  std::cout<<"hooks="<<status.hooks<<" model="<<status.model<<" contexts="<<status.contexts<<" enabled="<<status.enabled<<std::endl;
  if(!result && (!status.hooks || !status.model || status.contexts || status.enabled)) return 5;
  return result;
}
