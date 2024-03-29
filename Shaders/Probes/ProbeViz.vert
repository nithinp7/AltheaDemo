#version 460

#define IS_RT_SHADER 0
#include <GlobalIllumination/GIResources.glsl>

layout(location = 0) in vec3 sphereVert;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outColor;

void main() {
  vec3 probePos = getProbe(uint(gl_InstanceIndex)).position;

  vec3 color = getLightColor(gl_InstanceIndex);

  // paramaterize??
  float radius = 0.5;
  vec4 worldPos = vec4(probePos + radius * sphereVert, 1.0);
  gl_Position = globals.projection * globals.view * worldPos;

  outNormal = sphereVert;
  outColor = color;
}