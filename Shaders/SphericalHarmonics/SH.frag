#version 460 core

#include <Misc/Constants.glsl>

layout(location=0) in vec3 direction;
layout(location=1) in vec2 screenUV;

layout(location=0) out vec4 color;

#include <Global/GlobalUniforms.glsl>
#include <SphericalHarmonics/SHCommon.glsl>
#include <Misc/Input.glsl>

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
  uint displayMode;
  uint graphHandle;
} pushConstants;

SAMPLER2D(textureHeap);

#define globals RESOURCE(globalUniforms, pushConstants.globalUniforms)
#define environmentMap RESOURCE(textureHeap, pushConstants.ibl.environmentMapHandle)
#define prefilteredMap RESOURCE(textureHeap, pushConstants.ibl.prefilteredMapHandle)
#define irradianceMap RESOURCE(textureHeap, pushConstants.ibl.irradianceMapHandle)
#define brdfLut RESOURCE(textureHeap, pushConstants.ibl.brdfLutHandle)

#define locals RESOURCE(shUniforms, pushConstants.shUniforms)

#define graph RESOURCE(textureHeap, pushConstants.graphHandle)

vec3 sampleEnvMap(vec3 dir) {
  float yaw = atan(dir.z, dir.x);
  float pitch = -atan(dir.y, length(dir.xz));
  vec2 envMapUV = vec2(0.5 * yaw, pitch) / PI + 0.5;

  return textureLod(environmentMap, envMapUV, 0.0).rgb;
} 

void main() {
  uint inputMask = globals.inputMask;
  vec3 inputViz = 
      vec3(
        (inputMask >> 16) & 0xff, 
        (inputMask >> 8) & 0xff, 
        inputMask & 0xff);
  inputViz /= 255.0;
  // color = vec4(inputViz, 1.0);

  uint inputCount = bitCount(inputMask);

  // if (bool(inputMask & INPUT_BIT_LEFT_MOUSE))
  //   color = vec4(0.0, 1.0, 0.0, 1.0);
  // else if (bool(inputMask & INPUT_BIT_RIGHT_MOUSE))  
  //   color = vec4(0.0, 0.0, 1.0, 1.0);
  // else if (bool(inputMask & INPUT_BIT_A))
  //   color = vec4(1.0, 0.0, 0.0, 1.0);
  // else 
  //   color = vec4(0.0, 0.0, 0.0, 1.0);

  if (pushConstants.displayMode == DISPLAY_MODE_GRAPH) {
    color = texture(graph, screenUV);
  } else {
    vec3 envSample = sampleEnvMap(direction);
    envSample = vec3(1.0) - exp(-envSample * globals.exposure);
  
    color = vec4(envSample, 1.0);
  }
}