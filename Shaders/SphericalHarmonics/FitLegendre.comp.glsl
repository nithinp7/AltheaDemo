#version 460 core 

#include <SphericalHarmonics/SHCommon.glsl>
#include <SphericalHarmonics/Legendre.glsl>

layout(push_constant) uniform PushConstants {
  uint legendreUniformsHandle;
} pushConstants;

#include <Misc/Constants.glsl>

#define locals RESOURCE(legendreUniforms, pushConstants.legendreUniformsHandle)
#define coeffs legendreCoeffs(locals.coeffBuffer)

layout(local_size_x=16) in;

float f(float x, float mean) {
  if (abs(x - mean) < 0.15)
    return 1.0;
  else 
    return 0.0;
}

void main() {
  // if (locals.sampleCount == 0)
  //   return;
  
  uint coeffIdx = uint(gl_GlobalInvocationID.x);
  
  float c = 0.0;
  
  float x = -1.0;
  int steps = 100;
  float stepSize = 2.0 / 100;
  for (int i = 0; i < 100; ++i)
  {
    float f_x = 0.0;
    for (uint sampleIdx = 0; sampleIdx < locals.sampleCount; ++sampleIdx) { 
      vec2 fSample = locals.samples[sampleIdx];
      fSample.x = 2.0 * fSample.x - 1.0;
      fSample.y = 1.0 - 2.0 * fSample.y;

      // f_x += f(x, fSample.x) * fSample.y / locals.sampleCount;
      f_x += f(x, fSample.x) * fSample.y;// / locals.sampleCount;
    }

    f_x = clamp(f_x, -1.0, 1.0);
    float p = P(x, coeffIdx);
    c += f_x * p * stepSize;
    x += stepSize;
  }

  coeffs[coeffIdx] = c;
}