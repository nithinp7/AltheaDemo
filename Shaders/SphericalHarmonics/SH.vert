#version 460 core

layout(location=0) smooth out vec3 direction;
layout(location=1) out vec2 screenUV;

#include <Global/GlobalUniforms.glsl>

struct IBLHandles {
  uint environmentMapHandle;
  uint prefilteredMapHandle;
  uint irradianceMapHandle;
  uint brdfLutHandle;
};

layout(push_constant) uniform PushConstants {
  IBLHandles ibl;
  uint globalUniforms;
  uint shUniforms;
} pushConstants;

#define globals RESOURCE(globalUniforms, pushConstants.globalUniforms)

void main() {
  vec2 screenPos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  vec4 pos = vec4(screenPos * 2.0 - 1.0, 0.0, 1.0);
  direction = mat3(globals.inverseView) * (globals.inverseProjection * pos).xyz;
  
  screenUV = vec2(screenPos.x, 1.0 - screenPos.y);

  gl_Position = pos;
}