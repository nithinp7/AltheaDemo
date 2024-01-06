#ifndef _SHCOMMON_
#define _SHCOMMON_

struct SHCoeffs {
  float w[4]; // ??
};

UNIFORM_BUFFER(shUniforms, SHUniforms{
  SHCoeffs coeffs;
  vec4 color;
  uint graphHandle;
  uint padding1;
  uint padding2;
  uint padding3;
});

// legendre polynomial eval (up to order 4)
// #define P0 1.0
// #define P1(x) x
// float P2(float x) {

// }

#endif // _SHCOMMON_