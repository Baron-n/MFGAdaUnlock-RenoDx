// SPDX-License-Identifier: MIT
#include "native_window.hpp"
#include <map>
#include <vector>

namespace mfgunlock::xemfg::native {
namespace {
std::mutex registryLock;
std::map<HWND,std::shared_ptr<WindowControl>> windows;
std::atomic<uintptr_t> nextGeneration{1};
UINT ControlMessage() { static const UINT value=RegisterWindowMessageW(L"MFGAdaUnlock.XeMFG.WindowControl.1"); return value; }
constexpr auto property=L"MFGAdaUnlock.XeMFG.WindowOwner.1";
LONG_PTR Style(HWND hwnd,int index) { return DWORD(GetWindowLongPtrW(hwnd,index)); }
bool SetStyle(HWND hwnd,int index,LONG_PTR value) {
  SetLastError(0);
  return SetWindowLongPtrW(hwnd,index,value)!=0 || GetLastError()==0;
}
bool SameRect(const RECT& a,const RECT& b) {
  return a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom;
}
}
std::shared_ptr<WindowControl> WindowControl::Bind(HWND hwnd,bool force) {
  DWORD pid{}; const DWORD tid=GetWindowThreadProcessId(hwnd,&pid);
  if(!tid || pid!=GetCurrentProcessId() || !ControlMessage()) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return {}; }
  auto value=std::shared_ptr<WindowControl>(new WindowControl);
  value->hwnd=hwnd; value->thread=tid; value->generation=nextGeneration++;
  HMODULE module{};
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(&MessageHook),&module)) return {};
  {
    std::lock_guard guard(registryLock);
    // Never replace a still-owned/restoring generation of the same HWND.
    if(auto it=windows.find(hwnd); it!=windows.end()) {
      if(GetPropW(hwnd,property)==it->second.get()) { SetLastError(ERROR_ALREADY_EXISTS); return {}; }
      if(it->second->hook) UnhookWindowsHookEx(it->second->hook);
      windows.erase(it); // The old HWND was destroyed; its property is gone.
    }
    // This thread belongs to our own process (validated above). A local hook
    // needs no DLL-name lookup/injection. Keep the explicit module pin for
    // lifetime, but do not ask User32 to resolve the addon's custom extension.
    value->hook=SetWindowsHookExW(WH_GETMESSAGE,&MessageHook,nullptr,tid);
    if(!value->hook) return {};
    if(!SetPropW(hwnd,property,value.get())) { UnhookWindowsHookEx(value->hook); return {}; }
    windows.emplace(hwnd,value);
  }
  if(force) value->Request(true);
  return value;
}
void WindowControl::Request(bool force) {
  std::lock_guard guard(lock);
  if(retiring || !IsWindow(hwnd)) return;
  requested=force; pending=true; error=0;
  if(!PostMessageW(hwnd,ControlMessage(),generation,0)) { error=GetLastError(); pending=false; }
}
void WindowControl::OverlayKey(OverlayChord chord) {
  // Read registration; never register/redirect the game's input devices.
  RAWINPUTDEVICE devices[64]{}; UINT count=64; bool raw{};
  const auto n=GetRegisteredRawInputDevices(devices,&count,sizeof(RAWINPUTDEVICE));
  if(n!=UINT(-1)) for(UINT i=0;i<n;++i)
    if(devices[i].usUsagePage==1 && devices[i].usUsage==6
        && (devices[i].dwFlags&RIDEV_NOLEGACY)==RIDEV_NOLEGACY) raw=true;
  rawKeyboard=raw && chord.key!=0;
  std::lock_guard guard(lock); shortcut.Configure(chord);
}
void WindowControl::OverlayNative() { std::lock_guard guard(lock); shortcut.ObservedNative(); }
uint32_t WindowControl::OverlayTaps(uint32_t blockers,uint64_t& dropped,OverlayReceipt* receipt) {
  const bool unfocused=IsWindowVisible(hwnd) && (IsIconic(hwnd)
      || GetAncestor(GetForegroundWindow(),GA_ROOT)!=GetAncestor(hwnd,GA_ROOT));
  std::lock_guard guard(lock); dropped=shortcut.Dropped();
  if(unfocused) shortcut.Cancel();
  if(unfocused) blockers|=8;
  if(retiring) blockers|=16;
  const auto taps=shortcut.Drain(blockers!=0);
  if(receipt) *receipt={taps,childMessages,blockers};
  return taps.ready;
}
WindowStatus WindowControl::Snapshot() {
  std::lock_guard guard(lock);
  WindowStatus status{};
  status.available=hook && IsWindow(hwnd) && GetPropW(hwnd,property)==this; status.requested=requested;
  status.pending=pending; status.managed=managed; status.error=error;
  if(status.available) {
    const auto style=GetWindowLongPtrW(hwnd,GWL_STYLE);
    status.borderless=!(style&WS_CAPTION) && !(style&WS_THICKFRAME);
    MONITORINFO monitor{sizeof(MONITORINFO)}; RECT rect{};
    if(GetWindowRect(hwnd,&rect) && GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))
      status.coversMonitor=SameRect(rect,monitor.rcMonitor);
  }
  return status;
}
void WindowControl::Apply() {
  // Called ONLY by WH_GETMESSAGE, without the bridge/SDK/registry locks.
  bool force{}, owned{}; LONG_PTR oldStyle{},oldEx{},ourStyle{},ourEx{};
  WINDOWPLACEMENT saved{sizeof(WINDOWPLACEMENT)}; RECT ourRect{};
  {
    std::lock_guard guard(lock);
    if(!pending || GetCurrentThreadId()!=thread) return;
    force=requested; owned=managed; pending=false; applying=true;
    oldStyle=originalStyle; oldEx=originalEx; saved=placement;
    ourStyle=appliedStyle; ourEx=appliedEx; ourRect=appliedRect;
  }
  // Window APIs can synchronously enter the game's WndProc. No mutex is held
  // across them: a resize handler may wait for a rendering thread/overlay.
  DWORD failure{};
  if(!IsWindow(hwnd) || GetPropW(hwnd,property)!=this) failure=ERROR_INVALID_WINDOW_HANDLE;
  else if(force) {
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if(!GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor)) failure=GetLastError();
    if(!owned && !failure) {
      oldStyle=Style(hwnd,GWL_STYLE); oldEx=Style(hwnd,GWL_EXSTYLE);
      if(!GetWindowPlacement(hwnd,&saved)) failure=GetLastError();
      else if(!(oldStyle&WS_VISIBLE)) saved.showCmd=SW_HIDE;
    }
    const auto style=(Style(hwnd,GWL_STYLE)&~LONG_PTR(WS_OVERLAPPEDWINDOW))|WS_POPUP;
    const auto ex=Style(hwnd,GWL_EXSTYLE)&~LONG_PTR(WS_EX_WINDOWEDGE|WS_EX_CLIENTEDGE|WS_EX_DLGMODALFRAME);
    if(!failure && (!SetStyle(hwnd,GWL_STYLE,style) || !SetStyle(hwnd,GWL_EXSTYLE,ex))) failure=GetLastError();
    if(!failure) {
      const auto& r=monitor.rcMonitor;
      if(!SetWindowPos(hwnd,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,
          SWP_FRAMECHANGED|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER)) failure=GetLastError();
    }
    if(failure) {
      // Roll back only if this attempt actually changed styles.
      if(oldStyle) {
        SetStyle(hwnd,GWL_STYLE,oldStyle); SetStyle(hwnd,GWL_EXSTYLE,oldEx);
        SetWindowPlacement(hwnd,&saved);
        SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
      }
    } else {
      owned=true; ourStyle=style; ourEx=ex; ourRect=monitor.rcMonitor;
    }
  } else if(owned) {
    RECT now{}; GetWindowRect(hwnd,&now);
    // If the game/another addon took ownership, don't overwrite their change.
    if(Style(hwnd,GWL_STYLE)!=ourStyle || Style(hwnd,GWL_EXSTYLE)!=ourEx
        || !SameRect(now,ourRect)) { failure=ERROR_BUSY; owned=false; }
    else if(!SetStyle(hwnd,GWL_STYLE,oldStyle) || !SetStyle(hwnd,GWL_EXSTYLE,oldEx)
        || !SetWindowPlacement(hwnd,&saved)
        || !SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED)) {
      failure=GetLastError();
    } else owned=false;
  }
  std::lock_guard guard(lock);
  error=failure; managed=owned; originalStyle=oldStyle; originalEx=oldEx; placement=saved;
  appliedStyle=ourStyle; appliedEx=ourEx; appliedRect=ourRect;
  applying=false;
}
void WindowControl::Retire() {
  {
    std::lock_guard guard(lock);
    retiring=true; requested=false; pending=managed || applying; error=0;
    if(pending && IsWindow(hwnd) && PostMessageW(hwnd,ControlMessage(),generation,0)) return;
    // A dead window has no styles to restore. Keep a live pending controller
    // if posting failed; never call window-changing APIs from this thread.
    if(pending && IsWindow(hwnd)) { error=GetLastError(); return; }
  }
  std::lock_guard registry(registryLock);
  if(hook) UnhookWindowsHookEx(hook); hook=nullptr;
  if(GetPropW(hwnd,property)==this) RemovePropW(hwnd,property);
  if(auto it=windows.find(hwnd); it!=windows.end() && it->second.get()==this) windows.erase(it);
}
LRESULT CALLBACK WindowControl::MessageHook(int code,WPARAM w,LPARAM l) {
  if(code>=0 && w==PM_REMOVE) {
    auto* message=reinterpret_cast<MSG*>(l);
    if(message && (message->message==WM_KEYDOWN || message->message==WM_SYSKEYDOWN
        || message->message==WM_KEYUP || message->message==WM_SYSKEYUP || message->message==WM_INPUT)) {
      std::shared_ptr<WindowControl> value;
      { std::lock_guard guard(registryLock);
        if(auto it=windows.find(message->hwnd); it!=windows.end()) value=it->second;
        else {
          // ReShade also accepts input from a child of the render window.
          // Restrict recovery to this owner thread and one unambiguous host.
          for(const auto& [host,candidate]:windows) {
            if(candidate->thread!=GetCurrentThreadId() || !IsChild(host,message->hwnd)) continue;
            if(value) { value.reset(); break; }
            value=candidate;
          }
        }
      }
      if(value && (message->message!=WM_INPUT || value->rawKeyboard)) {
        uint32_t key=uint32_t(message->wParam);
        bool down=message->message==WM_KEYDOWN || message->message==WM_SYSKEYDOWN;
        bool repeat=(uintptr_t(message->lParam)&(uintptr_t(1)<<30))!=0;
        if(message->message==WM_INPUT) {
          RAWINPUT raw{}; UINT bytes=sizeof(raw);
          if(GET_RAWINPUT_CODE_WPARAM(message->wParam)!=RIM_INPUT
              || GetRawInputData(reinterpret_cast<HRAWINPUT>(message->lParam),RID_INPUT,&raw,&bytes,sizeof(RAWINPUTHEADER))==UINT(-1)
              || raw.header.dwType!=RIM_TYPEKEYBOARD) return CallNextHookEx(nullptr,code,w,l);
          key=raw.data.keyboard.VKey; down=!(raw.data.keyboard.Flags&RI_KEY_BREAK); repeat=false;
        }
        const bool unfocused=IsWindowVisible(value->hwnd) && (IsIconic(value->hwnd)
            || GetAncestor(GetForegroundWindow(),GA_ROOT)!=GetAncestor(value->hwnd,GA_ROOT));
        std::lock_guard guard(value->lock);
        if(unfocused) value->shortcut.Cancel();
        else if(!value->retiring) {
          if(message->hwnd!=value->hwnd && key==value->shortcut.Chord().key) ++value->childMessages;
          value->shortcut.Key(key,down,repeat);
        }
      }
      // Observe only. ReShade and the game still receive the original message.
    }
    if(message && message->message==ControlMessage()) {
      std::shared_ptr<WindowControl> value;
      { std::lock_guard guard(registryLock);
        if(auto it=windows.find(message->hwnd); it!=windows.end()) value=it->second;
      }
      if(value && value->generation==message->wParam) {
        // Consume only our own generation's private request.
        message->message=WM_NULL;
        value->Apply();
        bool retiring{}; { std::lock_guard guard(value->lock); retiring=value->retiring && !value->pending; }
        if(retiring) {
          std::lock_guard guard(registryLock);
          if(value->hook) UnhookWindowsHookEx(value->hook); value->hook=nullptr;
          if(GetPropW(value->hwnd,property)==value.get()) RemovePropW(value->hwnd,property);
          if(auto it=windows.find(value->hwnd); it!=windows.end() && it->second==value) windows.erase(it);
        }
      }
    }
  }
  return CallNextHookEx(nullptr,code,w,l);
}
}
