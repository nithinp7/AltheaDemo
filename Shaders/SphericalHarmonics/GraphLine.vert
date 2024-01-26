#version 460 core

layout(location = 0) out vec4 color;

#include <Misc/Constants.glsl>

layout(push_constant) uniform PushConstants {
  uint vertexCount;
  uint shUniformsHandle;
  uint legendreUniformsHandle;
} pushConstants;

#include <SphericalHarmonics/SHCommon.glsl>
#include <SphericalHarmonics/Legendre.glsl>

#define shLocals RESOURCE(shUniforms, pushConstants.shUniformsHandle)
#define lgLocals RESOURCE(legendreUniforms, pushConstants.legendreUniformsHandle)
#define coeffs legendreCoeffs(lgLocals.coeffBuffer)

void main() {
  float x;

  // normalize between [0,1]
  // non-dotted
#ifndef DOTTED
  {
    // vertexCount = 2 + interiorPoints * 2
    // ==> 
    uint interiorPoints = (pushConstants.vertexCount - 2) / 2;
    uint segments = interiorPoints + 1;
    float segmentWidth = 1.0 / segments;

    if (gl_VertexIndex == 0) {
      x = 0.0;
    } else if (gl_VertexIndex == (pushConstants.vertexCount - 1)) {
      x = 1.0;
    } else {
      x = segmentWidth * ((gl_VertexIndex + 1) / 2);
    }
  }
#endif

  // dotted
#ifdef DOTTED
  {
    x = float(gl_VertexIndex) / float(pushConstants.vertexCount - 1);
  }
#endif

  x = 2.0 * x - 1.0;

  // sample function
  float y = 0.0;
  for (uint i = 0; i < 16; ++i) {
    y += coeffs[i] * P(x, i);
  }
 
  // convert from uv space to ndc
  gl_Position = vec4(x, y, 0.0, 1.0);

  uint randCol = gl_InstanceIndex * 0x123ab678;
  color = vec4((randCol >> 16) & 0xff, (randCol >> 8) & 0xff, randCol & 0xff, 255.0) / 255.0;
  // color = vec4(1.0, 0.0, 0.0, 1.0);
}