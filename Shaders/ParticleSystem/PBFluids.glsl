#ifndef _PBFLUIDS_
#define _PBFLUIDS_

#include "Particle.glsl"

// An SPH kernel
float gaussianKernel(float d, float h) {
  float d2 = d * d;

  float h2 = h * h;
  float h3 = h * h2;

  // TODO: simplify / precompute
  return 1.0 / pow(PI, 2.0/3.0) / h3 * exp(d2 / h2);
}



#endif // _PBFLUIDS_