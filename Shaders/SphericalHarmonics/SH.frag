#version 460 core

#include <Misc/Constants.glsl>

layout(location=0) in vec3 direction;
layout(location=1) in vec2 screenUV;

layout(location=0) out vec4 color;

#include <Global/GlobalUniforms.glsl>

struct IBLHandles {
  uint environmentMapHandle;
  uint prefilteredMapHandle;
  uint irradianceMapHandle;
  uint brdfLutHandle;
};

layout(push_constant) uniform PushConstants {
  IBLHandles ibl;
  uint shCoeffs;
  uint globalUniforms;
} pushConstants;

SAMPLER2D(textureHeap);

#define globals RESOURCE(globalUniforms, pushConstants.globalUniforms)
#define environmentMap RESOURCE(textureHeap, pushConstants.ibl.environmentMapHandle)
#define prefilteredMap RESOURCE(textureHeap, pushConstants.ibl.prefilteredMapHandle)
#define irradianceMap RESOURCE(textureHeap, pushConstants.ibl.irradianceMapHandle)
#define brdfLut RESOURCE(textureHeap, pushConstants.ibl.brdfLutHandle)

vec3 sampleEnvMap(vec3 dir) {
  float yaw = atan(dir.z, dir.x);
  float pitch = -atan(dir.y, length(dir.xz));
  vec2 envMapUV = vec2(0.5 * yaw, pitch) / PI + 0.5;

  return textureLod(environmentMap, envMapUV, 0.0).rgb;
} 

void main() {
  vec3 envSample = sampleEnvMap(direction);
  color = vec4(envSample, 1.0);
}