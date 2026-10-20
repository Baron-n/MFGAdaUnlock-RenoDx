// SPDX-License-Identifier: MIT
#include "../src/addons/mfgunlock/xemfg/native_window.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace mfgunlock::xemfg::native;
std::weak_ptr<WindowControl> resizeController;
bool retireDuringResize{}, reentryObserved{};
LRESULT CALLBACK WndProc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
  if(message==WM_SIZE && retireDuringResize) {
    retireDuringResize=false;
    if(auto controller=resizeController.lock()) {
      reentryObserved=controller->Snapshot().available;
      controller->Retire();
    }
  }
  return DefWindowProcW(hwnd,message,w,l);
}
void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void Pump() { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg); }
int main() {
  try {
    WNDCLASSW wc{}; wc.lpfnWndProc=WndProc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"MFGWindowControlTest";
    RegisterClassW(&wc);
    const auto hwnd=CreateWindowW(wc.lpszClassName,L"window control",WS_OVERLAPPEDWINDOW,45,60,640,360,nullptr,nullptr,wc.hInstance,nullptr);
    Require(hwnd,"create window");
    const DWORD originalStyle=DWORD(GetWindowLongPtrW(hwnd,GWL_STYLE)); RECT before{}; GetWindowRect(hwnd,&before);
    auto controller=WindowControl::Bind(hwnd,false); Require(bool(controller),"bind hook");
    // Request from another thread. It must remain pending and unchanged until
    // the owning thread dispatches its queued message.
    std::thread render([&]{controller->Request(true);}); render.join();
    Require(controller->Snapshot().pending && DWORD(GetWindowLongPtrW(hwnd,GWL_STYLE))==originalStyle,"request changed window off-thread");
    Pump(); auto state=controller->Snapshot();
    Require(state.borderless && state.coversMonitor && state.managed && !state.pending && !state.error,"borderless not applied");
    controller->Request(false); Pump(); state=controller->Snapshot(); RECT restored{}; GetWindowRect(hwnd,&restored);
    std::cout<<"restore managed="<<state.managed<<" error="<<state.error<<" style="<<std::hex
        <<DWORD(GetWindowLongPtrW(hwnd,GWL_STYLE))<<" original="<<originalStyle<<std::dec
        <<" rect="<<restored.left<<','<<restored.top<<','<<restored.right<<','<<restored.bottom
        <<" original="<<before.left<<','<<before.top<<','<<before.right<<','<<before.bottom<<'\n';
    Require(!state.managed && !state.error && DWORD(GetWindowLongPtrW(hwnd,GWL_STYLE))==originalStyle
        && EqualRect(&before,&restored),"original placement/style not restored");
    controller->Request(true); Pump();
    // Preserve a subsequent change made by the game/another addon.
    SetWindowLongPtrW(hwnd,GWL_STYLE,GetWindowLongPtrW(hwnd,GWL_STYLE)|WS_THICKFRAME);
    controller->Request(false); Pump();
    Require(controller->Snapshot().error==ERROR_BUSY && (GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_THICKFRAME),"foreign style overwritten");
    controller->Retire();
    auto second=WindowControl::Bind(hwnd,true); Require(bool(second),"retired generation still owns HWND"); Pump();
    second->Retire(); Pump(); Require(!second->Snapshot().available,"retirement did not remove hook");
    auto reentrant=WindowControl::Bind(hwnd,false); Require(bool(reentrant),"reentrant bind");
    resizeController=reentrant; retireDuringResize=true;
    reentrant->Request(true); Pump();
    Require(reentryObserved && !reentrant->Snapshot().available,"resize reentry/retirement did not complete");
    // A destroyed window must not be reported as active or touched later.
    auto dead=WindowControl::Bind(hwnd,true); Require(bool(dead),"bind dead-window test");
    DestroyWindow(hwnd); dead->Retire(); Require(!dead->Snapshot().available,"destroyed HWND reported active");
    std::cout<<"PASS: owner-thread borderless, pending state, placement restore, foreign ownership and retirement\n";
    return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
