#ifndef _PBFLUIDS_
#define _PBFLUIDS_

#include <Misc/Constants.glsl>


// An SPH kernel
float gaussianKernel(float d, float h) {
  float d2 = d * d;

  float h2 = h * h;
  float h3 = h * h2;

  // TODO: simplify / precompute
  return 1.0 / pow(PI, 2.0/3.0) / h3 * exp(d2 / h2);
}

// based on the video: Coding Adventures: Simulating Fluids - Sebastian Lague
float smoothingKernel(float r2, float d2) {
  float r8 = r2 * r2;
  r8 *= r8;
  
  float vol = 0.25 * PI * r8;
  float v = max(0.0, r2 - d2);
  return v * v * v / vol;
}

float smoothingKernelDeriv(float r2, float d2) {
  if (d2 > r2) return 0.0;

  float f = r2 - d2;
  
  float r8 = r2 * r2;
  r8 *= r8;
  
  float scale = -24.0 / (PI * pow(r2, 4.0));  
  return scale * f * f; 
}


#endif // _PBFLUIDS_