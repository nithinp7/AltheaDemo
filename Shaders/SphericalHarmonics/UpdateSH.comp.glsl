#version 460 core 

#include "SHCommon.glsl"

#include <Misc/Constants.glsl>
#include <Misc/Sampling.glsl>

layout(push_constant) uniform PushConstants {
  uint seed;
  uint coeffsHandle;
  uint envMapHandle;
} pushConstants;

SAMPLER2D(textureHeap);

#define environmentMap RESOURCE(textureHeap, pushConstants.envMapHandle)
#define coeffs shCoeffs(pushConstants.coeffsHandle, 0)

vec3 sampleEnvMap(vec3 dir) {
  float yaw = atan(dir.z, dir.x);
  float pitch = -atan(dir.y, length(dir.xz));
  vec2 envMapUV = vec2(0.5 * yaw, pitch) / PI + 0.5;

  return textureLod(environmentMap, envMapUV, 0.0).rgb;
} 

void main() {
  int coeffIdx = int(gl_GlobalInvocationID.x);
  
  uvec2 seed2 = uvec2(pushConstants.seed, pushConstants.seed+1);
  vec2 xi = randVec2(seed2);
  // bias samples towards equator to more fairly spread samples on sphere
  // TODO: Find better distrib?
  xi.y = 2.0 * xi.y - 1.0;
  xi.y *= xi.y;
  vec2 uv = vec2(xi.x, 0.5 * xi.y + 0.5);

  float sampleTheta = 2.0 * PI * xi.x;
  float samplePhi = PI * xi.y;

  float cosTheta = cos(sampleTheta);
  float sinTheta = sin(sampleTheta);
  float cosPhi = cos(samplePhi);
  float sinPhi = sin(samplePhi);

  vec3 sampleDir = vec3(cosTheta * cosPhi, sinPhi, sinTheta * cosPhi); 
  mat3 tanSpace = LocalToWorld(sampleDir);

  float y = 0.0;

  float spreadRatio = 0.1;
  for (int i = 0; i < 5; i++) {
    float x = mix(-spreadRatio, spreadRatio, float(i)/4.0);
    for (int j = 0; j < 5; ++j) {
      float y = mix(-spreadRatio, spreadRatio, float(j)/4.0);
      vec3 dir = normalize(tanSpace * vec3(x, y, 1.0));
      
      float theta = atan(dir.z, dir.x);
      float phi = -atan(dir.y, length(dir.xz));
      vec2 envMapUV = vec2(0.5 * theta, phi) / PI + 0.5;

      vec3 envMapSample = textureLod(environmentMap, envMapUV, 0.0).rgb;
      float intensity = length(envMapSample);

      y = Y(coeffIdx, theta, phi) * intensity / 25.0;
    }
  }

  coeffs[coeffIdx] += y; // ???
}