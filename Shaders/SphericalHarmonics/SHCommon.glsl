#ifndef _SHCOMMON_
#define _SHCOMMON_

#include <Bindless/GlobalHeap.glsl>
#include <Misc/Constants.glsl>

#include "Legendre.glsl"

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

float K(int l, int m) {
  int f = 1;
  for (i = l-m+1; i <= l+m; i++)
    f *= i;
  return sqrt(float(2 * l + 1) / (4.0 * PI) / float(f));
}

// SH functions
float Y(int l, int m, float theta, float phi) {
  float cosTheta = cos(theta);
  if (m < 0) {
    return sqrt(2.0) * K(l, -m) * P(l, -m, cosTheta) * sin(-m * phi);
  } else if (m == 0) {
    return K(l, 0) * P(l, 0, cosTheta);
  } else {
    return sqrt(2.0) * K(l, m) * P(l, m, cosTheta) * cos(m * phi);
  }
}
#endif // _SHCOMMON_