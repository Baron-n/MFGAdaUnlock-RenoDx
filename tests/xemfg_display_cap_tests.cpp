// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <iostream>

#include "../src/addons/mfgunlock/xemfg/display_cap.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition     \
                << '\n';                                                        \
      std::abort();                                                              \
    }                                                                           \
  } while (false)

using namespace mfgunlock::xemfg::native;

// Source frame rate implied by an interval, in thousandths of a frame per second.
constexpr uint64_t SourceMilliFps(uint32_t intervalUs) { return 1000000000ull/intervalUs; }

int main() {
  // 165 Hz at 4x: 165 * 0.97 / 4 = 40.01 source FPS -> 160.05 output FPS, under 165.
  const auto i165x4=DisplayCapIntervalUs(165000,4);
  CHECK(i165x4==24993);
  CHECK(SourceMilliFps(i165x4)*4<=165000ull*970/1000+4);
  // Every common refresh and multiplier stays below the display refresh.
  for(uint32_t hz:{60u,120u,144u,165u,240u,360u}) {
    for(uint32_t m=2;m<=6;++m) {
      const auto interval=DisplayCapIntervalUs(hz*1000,m);
      CHECK(interval>0);
      CHECK(SourceMilliFps(interval)*m<hz*1000);  // output below refresh
      CHECK(SourceMilliFps(interval)*m>hz*1000*95/100);  // and not far below it
      CHECK(!ExceedsDisplay(interval,hz*1000,m));
    }
  }
  CHECK(DisplayCapIntervalUs(60000,2)==34365);    // 29.1 FPS source
  CHECK(DisplayCapIntervalUs(239760,6)==25799);   // 239.76 Hz
  // Fractional refresh is kept (59.94 Hz).
  CHECK(DisplayCapIntervalUs(59940,2)>DisplayCapIntervalUs(60000,2));
  // Unknown refresh or multiplier: no cap.
  CHECK(DisplayCapIntervalUs(0,4)==0);
  CHECK(DisplayCapIntervalUs(5000,4)==0);
  CHECK(DisplayCapIntervalUs(165000,1)==0);
  CHECK(DisplayCapIntervalUs(165000,7)==0);
  CHECK(DisplayCapIntervalUs(165000,4,1000)==0);
  // The game's stricter limit (lower FPS, longer interval) wins; zero is no limit.
  CHECK(CombinedIntervalUs(33333,24993)==33333);
  CHECK(CombinedIntervalUs(10000,24993)==24993);
  CHECK(CombinedIntervalUs(0,24993)==24993);
  CHECK(CombinedIntervalUs(16666,0)==16666);
  CHECK(CombinedIntervalUs(0,0)==0);
  // Notice: 80 source FPS at 4x on 165 Hz exceeds it; 40 FPS does not.
  CHECK(ExceedsDisplay(12500,165000,4));
  CHECK(!ExceedsDisplay(25000,165000,4));
  CHECK(!ExceedsDisplay(0,165000,4));
  CHECK(!ExceedsDisplay(12500,0,4));
  std::cout << "xemfg_display_cap_tests passed\n";
  return 0;
}
