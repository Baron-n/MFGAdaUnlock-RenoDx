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
int main(int argc,char** argv) {
  try {
    WNDCLASSW wc{}; wc.lpfnWndProc=WndProc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"MFGWindowControlTest";
    RegisterClassW(&wc);
    const auto hwnd=CreateWindowW(wc.lpszClassName,L"window control",WS_OVERLAPPEDWINDOW,45,60,640,360,nullptr,nullptr,wc.hInstance,nullptr);
    Require(hwnd,"create window");
    const DWORD originalStyle=DWORD(GetWindowLongPtrW(hwnd,GWL_STYLE)); RECT before{}; GetWindowRect(hwnd,&before);
    auto controller=WindowControl::Bind(hwnd,false); Require(bool(controller),"bind hook");
    if(argc>1 && std::string(argv[1])=="--hotkey") {
      ShowWindow(hwnd,SW_SHOWNORMAL); SetForegroundWindow(hwnd);
      Pump(); Require(GetForegroundWindow()==hwnd,"own test window not foreground; no shortcuts registered");
      controller->OverlayKey({VK_HOME,0,true}); Pump();
      OverlayReceipt hotkey; uint64_t overflow{};
      controller->OverlayTaps(0,overflow,&hotkey);
      Require(hotkey.hotkeyRegistered && !hotkey.hotkeyError,"foreground Home registration failed");
      controller->OverlayTextCapture(true); Pump();
      Require(RegisterHotKey(hwnd,0xb123,MOD_NOREPEAT,VK_HOME),"text editing retained Home"); UnregisterHotKey(hwnd,0xb123);
      controller->OverlayTextCapture(false); Pump();
      Require(controller->OverlayOwnsHotkey({VK_HOME,0,true}),"text editing did not restore shortcut");
      std::thread configure([&]{controller->OverlayKey({VK_F2,1,true});}); configure.join(); Pump();
      Require(controller->OverlayOwnsHotkey({VK_F2,1,true}),"cross-thread rebind failed");
      Require(RegisterHotKey(hwnd,0xb123,MOD_NOREPEAT,VK_HOME),"rebind retained old key"); UnregisterHotKey(hwnd,0xb123);
      controller->OverlayKey({0,0,true}); Pump();
      Require(RegisterHotKey(hwnd,0xb123,MOD_CONTROL|MOD_NOREPEAT,VK_F2),"disabled binding retained key"); UnregisterHotKey(hwnd,0xb123);
      Require(RegisterHotKey(hwnd,0xb123,MOD_NOREPEAT,VK_HOME),"conflict fixture");
      controller->OverlayKey({VK_HOME,0,true}); Pump(); controller->OverlayTaps(0,overflow,&hotkey);
      Require(!hotkey.hotkeyRegistered && hotkey.hotkeyError==ERROR_HOTKEY_ALREADY_REGISTERED
          && !controller->OverlayOwnsHotkey({VK_HOME,0,true}),"conflict did not preserve native fallback");
      UnregisterHotKey(hwnd,0xb123);
      controller->OverlayKey({0,0,true}); Pump(); controller->OverlayKey({VK_HOME,0,true}); Pump();
      Require(controller->OverlayOwnsHotkey({VK_HOME,0,true}),"registration did not recover after conflict");
      controller->Retire(); Pump(); Require(!controller->Snapshot().available,"foreground retirement did not finish");
      Require(RegisterHotKey(hwnd,0xb123,MOD_NOREPEAT,VK_HOME),"retirement retained global shortcut"); UnregisterHotKey(hwnd,0xb123);
      ShowWindow(hwnd,SW_HIDE); Pump();
      controller=WindowControl::Bind(hwnd,false); Require(bool(controller),"rebind after foreground tests");
      std::cout<<"PASS: foreground hotkey text suspension, cross-thread rebind, disabled binding, conflict fallback and retirement\n";
    }
    controller->OverlayKey({VK_HOME,0,true});
    const auto child=CreateWindowW(wc.lpszClassName,L"input child",WS_CHILD,0,0,20,20,hwnd,nullptr,wc.hInstance,nullptr);
    const auto unrelated=CreateWindowW(wc.lpszClassName,L"unrelated",WS_OVERLAPPEDWINDOW,0,0,20,20,nullptr,nullptr,wc.hInstance,nullptr);
    Require(child && unrelated,"input routing windows");
    auto tap=[&](HWND target) {
      PostMessageW(target,WM_KEYDOWN,VK_HOME,1); PostMessageW(target,WM_KEYUP,VK_HOME,LPARAM(1u<<31)); Pump();
    };
    uint64_t dropped{}; OverlayReceipt receipt;
    tap(child);
    Require(controller->OverlayTaps(0,dropped,&receipt)==1 && receipt.childMessages==2
        && receipt.taps.presses==1 && receipt.taps.releases==1,"child shortcut not observed once");
    tap(unrelated);
    Require(controller->OverlayTaps(0,dropped,&receipt)==0 && receipt.taps.presses==1,"unrelated window hijacked shortcut");
    tap(hwnd);
    Require(!controller->OverlayTaps(2,dropped,&receipt) && receipt.blockers==2 && receipt.taps.blocked==1,
        "widget-capture reason not retained");
    DestroyWindow(child); DestroyWindow(unrelated);
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
