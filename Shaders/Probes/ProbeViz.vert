#version 460

#include <GlobalIllumination/GIResources.glsl>

layout(location = 0) in vec3 sphereVert;

layout(location = 0) out vec3 outNormal;

void main() {
  vec3 probePos = getProbe(uint(gl_InstanceIndex)).position;

  // paramaterize??
  float radius = 1.0;
  vec4 worldPos = vec4(probePos + radius * sphereVert, 1.0);
  gl_Position = globals.projection * globals.view * worldPos;

  outNormal = sphereVert;
}