// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <windows.h>
namespace mfgunlock::xemfg::native {
struct OverlayChord {
  uint32_t key{VK_HOME},modifiers{};
  bool exact{true};
  bool operator==(const OverlayChord&) const = default;
};
// Serialized by WindowControl. Stores completed taps, not text or a keyboard
// history. ReShade's normal down/pressed state can lose an entire tap between
// output updates. Native requests consume their own tap to avoid double toggle.
class OverlayShortcut {
  struct Tap { uint64_t serial{}; bool consumed{}; };
  std::array<Tap,16> taps{};
  size_t size{};
  OverlayChord chord;
  uint8_t modifiers{};
  bool down{},armed{},consumed{};
  uint64_t serial{},dropped{};
public:
  OverlayChord Chord() const { return chord; }
  void Configure(OverlayChord value) {
    if(value==chord) return;
    chord=value; size=0; down=armed=consumed=false;
  }
  void Cancel() { size=0; down=armed=consumed=false; modifiers=0; }
  void Key(uint32_t key,bool pressed,bool repeat=false) {
    uint8_t mask{};
    if(key==VK_CONTROL || key==VK_LCONTROL || key==VK_RCONTROL) mask=1;
    if(key==VK_SHIFT || key==VK_LSHIFT || key==VK_RSHIFT) mask=2;
    if(key==VK_MENU || key==VK_LMENU || key==VK_RMENU) mask=4;
    if(mask) { if(pressed) modifiers|=mask; else modifiers&=~mask; }
    if(!chord.key || key!=chord.key) return;
    if(pressed) {
      if(!down && !repeat) {
        ++serial; consumed=false;
        armed=chord.exact?modifiers==chord.modifiers:(modifiers&chord.modifiers)==chord.modifiers;
      }
      down=true;
    } else {
      if(down && armed) {
        if(size<taps.size()) taps[size++]={serial,consumed}; else ++dropped;
      }
      down=armed=consumed=false;
    }
  }
  void ObservedNative() {
    if(down) consumed=true;
    else for(size_t i=0;i<size;++i) if(taps[i].serial==serial) taps[i].consumed=true;
  }
  uint32_t Take(bool blocked=false) {
    uint32_t count{};
    if(!blocked) for(size_t i=0;i<size;++i) if(!taps[i].consumed) ++count;
    size=0;
    if(blocked) consumed=true;
    return count;
  }
  uint64_t Dropped() const { return dropped; }
};
}
