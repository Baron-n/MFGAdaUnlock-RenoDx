#pragma once
#include <cstdint>
namespace mfgunlock::xemfg {
struct Status {
  uint32_t size = sizeof(Status), version = 1;
  uint32_t hooks = 0, model = 0, contexts = 0, initialized = 0;
  uint32_t enabled = 0, generated = 0, presented = 0;
  int32_t result = 0;
  uint64_t tensor_launches = 0;
  char detail[256] = "XeMFG companion not initialized";
};
using GetStatus = bool (*)(Status*, uint32_t);
}
