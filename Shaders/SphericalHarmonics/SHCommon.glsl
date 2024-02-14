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

BUFFER_RW(_coeffSets, COEFF_SETS {
  CoeffSet arr[];
});

struct RadianceSample {
  vec3 radiance;
  float theta;
  float phi;
  float padding1;
};
struct RadianceSampleSet {
  RadianceSample samples[8];
};

// TODO: There has got to be a better way...
#define legendreCoeffs(idx) RESOURCE(_coeffSets, idx).arr[0].coeffs
#define shCoeffs(handle,i) RESOURCE(_coeffSets, handle).arr[i].coeffs

UNIFORM_BUFFER(legendreUniforms, LegendreUniforms{
  vec2 samples[10];
  uint sampleCount;
  uint coeffBuffer;
});

UNIFORM_BUFFER(shUniforms, SHUniforms{
  uint coeffBuffer;
  int displayMode;
  uint padding1;
  uint padding2;
});

float K(int l, int m) {
  int f = 1;
  for (int i = l-m+1; i <= l+m; i++)
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

float Y(int i, float theta, float phi) {
  int l = 0;
  int m = 0;
  if (i == 0) {
    l = 0;
    m = 0; 
  } else if (i < 4) {
    l = 1;
    m = i-2;
  } else if (i < 9) {
    l = 2;
    m = i-6;
  } else if (i < 16) {
    l = 3;
    m = i-12;
  } else {
    return 0.0;
  }

  return Y(l, m, theta, phi);
}
#endif // _SHCOMMON_