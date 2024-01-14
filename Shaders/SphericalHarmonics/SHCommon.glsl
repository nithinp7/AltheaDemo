#ifndef _SHCOMMON_
#define _SHCOMMON_

#include <Bindless/GlobalHeap.glsl>

#define DISPLAY_MODE_DEFAULT 0
#define DISPLAY_MODE_SH 1
#define DISPLAY_MODE_GRAPH 2

struct CoeffSet {
  float coeffs[16];
};

BUFFER_RW(_legendreCoeffs, LEGENDRE_COEFFS {
  CoeffSet legendreCoeffsArr[];
});

// TODO: There has got to be a better way...
#define legendreCoeffs(idx) RESOURCE(_legendreCoeffs, idx).legendreCoeffsArr[0].coeffs

UNIFORM_BUFFER(legendreUniforms, LegendreUniforms{
  vec2 samples[10];
  uint sampleCount;
  uint coeffBuffer;
});

UNIFORM_BUFFER(shUniforms, SHUniforms{
  float coeffs[16];
  uint graphHandle;
  int displayMode;
  uint padding1;
  uint padding2;
});

// legendre polynomial eval (up to order 4)
// #define P0 1.0
// #define P1(x) x
// float P2(float x) {

// }

#endif // _SHCOMMON_