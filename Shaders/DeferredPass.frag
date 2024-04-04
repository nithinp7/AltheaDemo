
#version 460 core

#define PI 3.14159265359

layout(location=0) in vec3 direction;
layout(location=1) in vec2 uv;

layout(location=0) out vec4 outColor;

#include <Bindless/GlobalHeap.glsl>
#include <Global/GlobalUniforms.glsl>
#include <Global/GlobalResources.glsl>
#include <PointLights.glsl>

#include <Misc/Sampling.glsl>

SAMPLER2D(textureHeap);

layout(push_constant) uniform PushConstants {
  uint globalResources;
  uint globalUniforms;
  uint reflectionBuffer;
  uint writeIndex;
} pushConstants;

#define globals RESOURCE(globalUniforms, pushConstants.globalUniforms)
#define resources RESOURCE(globalResources, pushConstants.globalResources)
#define environmentMap RESOURCE(textureHeap, resources.ibl.environmentMapHandle)
#define prefilteredMap RESOURCE(textureHeap, resources.ibl.prefilteredMapHandle)
#define irradianceMap RESOURCE(textureHeap, resources.ibl.irradianceMapHandle)
#define brdfLut RESOURCE(textureHeap, resources.ibl.brdfLutHandle)

#define gBufferDepth RESOURCE(textureHeap, (resources.gBuffer.depthAHandle + pushConstants.writeIndex))
#define gBufferNormal RESOURCE(textureHeap, resources.gBuffer.normalHandle)
#define gBufferAlbedo RESOURCE(textureHeap, resources.gBuffer.albedoHandle)
#define gBufferMetallicRoughnessOcclusion RESOURCE(textureHeap, resources.gBuffer.metallicRoughnessOcclusionHandle)

#define reflectionBuffer RESOURCE(textureHeap, pushConstants.reflectionBuffer)

SAMPLERCUBEARRAY(cubemapHeap);
#define shadowMapArray RESOURCE(cubemapHeap, resources.shadowMapArray)
#define pointLightArr RESOURCE(pointLights, globals.lightBufferHandle).pointLightArr

#include <PBR/PBRMaterial.glsl>

vec3 reconstructPosition(vec2 uv) {
  float dRaw = texture(gBufferDepth, uv).r;

  // TODO: Stop hardcoding this
  float near = 0.01;
  float far = 1000.0;
  float d = far * near / (dRaw * (far - near) - far);

  vec2 ndc = 2.0 * uv - vec2(1.0);

  vec4 camPlanePos = vec4(ndc, 2.0, 1.0);
  vec4 dirH = globals.inverseProjection * camPlanePos;
  dirH.xyz /= dirH.w;
  dirH.w = 0.0;
  dirH = globals.inverseView * dirH;
  vec3 dir = normalize(dirH.xyz);

  float f = dot(dir, globals.inverseView[2].xyz);

  return globals.inverseView[3].xyz + d * dir / f;
}

vec3 sampleEnvMap(vec3 dir) {
  float yaw = atan(dir.z, dir.x);
  float pitch = -atan(dir.y, length(dir.xz));
  vec2 envMapUV = vec2(0.5 * yaw, pitch) / PI + 0.5;

  return textureLod(environmentMap, envMapUV, 0.0).rgb;
} 

vec4 sampleReflection(float roughness) {
  return textureLod(reflectionBuffer, uv, 4.0 * roughness).rgba;
} 

// Random number generator and sample warping
// from ShaderToy https://www.shadertoy.com/view/4tXyWN
// TODO: Find better alternative??
uvec2 seed;

#define ENABLE_SSAO
#ifdef ENABLE_SSAO 
#define SSAO_RAY_COUNT 12
#define SSAO_RAYMARCH_STEPS 4
float computeSSAO(vec2 currentUV, vec3 worldPos, vec3 normal) {
  
  // vec4 projected = globals.projection * globals.view * vec4(rayDir, 0.0);
  // vec2 uvStep = (projected.xy / projected.w) / 128.0;
  float dx0 = 0.2;
  float dx = dx0;

  // currentUV += uvDir / 128.0;
  // return vec3(0.5) + 0.5 * rayDir;
  float ao = 0;

  for (int raySample = 0; raySample < SSAO_RAY_COUNT; ++raySample) {
    vec3 prevPos = worldPos;
    float prevProjection = 0.0;

    vec3 currentRayPos = worldPos;

    vec3 rayDir = normalize(LocalToWorld(normal) * sampleHemisphereCosine(seed));
    // vec3 rayDir = normalize(2.0 * vec3(rng(), rng(), rng()) - vec3(1.0) + normal);

    vec3 perpRef = cross(rayDir, normal);
    perpRef = normalize(cross(perpRef, rayDir));

    for (int i = 0; i < SSAO_RAYMARCH_STEPS; ++i) {
      // currentUV += uvStep;
      currentRayPos += dx * rayDir;
      vec4 projected = globals.projection * globals.view * vec4(currentRayPos, 1.0);
      currentUV = 0.5 * projected.xy / projected.w + vec2(0.5);

      if (currentUV.x < 0.0 || currentUV.x > 1.0 || currentUV.y < 0.0 || currentUV.y > 1.0) {
        break;
      }

      // TODO: Check for invalid position
      
      vec3 currentPos = reconstructPosition(currentUV);
      vec3 dir = currentPos - worldPos;
      float currentProjection = dot(dir, perpRef);

      float dist = length(dir);
      dir = dir / dist;

      // TODO: interpolate between last two samples
      // Step between this and the previous sample
      float worldStep = length(currentPos - prevPos);
      

      // float cosTheta = dot(dir, rayDir);
      // float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
      // if (acos(cosTheta) < 0.25) {

      if (currentProjection * prevProjection < 0.0 && worldStep <= 5 * dx && i > 0) {
        vec3 currentNormal = normalize(texture(gBufferNormal, currentUV, 0.0).xyz);
        // if (dot(currentNormal, rayDir) < 0) {
          ao += 1.0;
          break;
        // }
      }

      prevPos = currentPos;
      prevProjection = currentProjection;
    }
  }

  return 1.0 - ao / SSAO_RAY_COUNT;
}
#endif

void main() {
  seed = uvec2(gl_FragCoord.xy);

  vec4 baseColor = texture(gBufferAlbedo, uv);
  if (baseColor.a == 0.0) {
    // Nothing in the GBuffer, draw the environment map
    vec3 envMapSample = sampleEnvMap(direction);
#ifndef SKIP_TONEMAP
    envMapSample = vec3(1.0) - exp(-envMapSample * globals.exposure);
#endif
    outColor = vec4(envMapSample, 1.0);
    return;
  }

  vec3 position = reconstructPosition(uv);
  vec3 normal = normalize(texture(gBufferNormal, uv).xyz);
  vec3 metallicRoughnessOcclusion = 
      texture(gBufferMetallicRoughnessOcclusion, uv).rgb;
  // metallicRoughnessOcclusion.y *= 0.1;

  vec3 reflectedDirection = reflect(normalize(direction), normal);
  // vec4 reflectedColor = sampleReflection(0.2);//metallicRoughnessOcclusion.y);
  vec4 reflectedColor = sampleReflection(metallicRoughnessOcclusion.y);
  // reflectedColor = reflectedColor / reflectedColor.a;
  // reflectedColor.rgb = mix(baseColor, reflectedColor.rgb, reflectedColor.a);

  vec3 irradianceColor = sampleIrrMap(normal);

#ifdef ENABLE_SSAO
  metallicRoughnessOcclusion.z = computeSSAO(uv, position.xyz, normal);
#endif

  vec3 material = 
      pbrMaterial(
        position.xyz,
        normalize(direction),
        normal, 
        baseColor.rgb, 
        reflectedColor.rgb, 
        irradianceColor,
        metallicRoughnessOcclusion.x, 
        metallicRoughnessOcclusion.y, 
        metallicRoughnessOcclusion.z);

#ifndef SKIP_TONEMAP
  material = vec3(1.0) - exp(-material * globals.exposure);
#endif

  outColor = vec4(material, 1.0);
}
