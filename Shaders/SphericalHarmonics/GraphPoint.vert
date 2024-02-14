#version 460 core 

layout(location=0) out vec2 uv;

#include <SphericalHarmonics/SHCommon.glsl>

layout(push_constant) uniform PushConstants {
  uint legendreUniformsHandle;
} pushConstants;

#define locals RESOURCE(legendreUniforms, pushConstants.legendreUniformsHandle)

void main() {
  uv = locals.samples[gl_VertexIndex];

  gl_PointSize = 4.0;
  gl_Position = vec4(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, 0.0, 1.0);
}