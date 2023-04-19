#version 450

#define PI 3.14159265359

layout(location=0) in vec3 direction;
layout(location=1) in vec2 uv;

layout(location=0) out vec4 outColor;

layout(set=0, binding=0) uniform sampler2D environmentMap; 
layout(set=0, binding=1) uniform sampler2D prefilteredMap; 
layout(set=0, binding=2) uniform sampler2D irradianceMap;
layout(set=0, binding=3) uniform sampler2D brdfLut;

#define GLOBAL_UNIFORMS_SET 0
#define GLOBAL_UNIFORMS_BINDING 4
#include <GlobalUniforms.glsl>

// GBuffer textures
layout(set=1, binding=0) uniform sampler2D gBufferPosition;
layout(set=1, binding=1) uniform sampler2D gBufferNormal;
layout(set=1, binding=2) uniform sampler2D gBufferAlbedo;
layout(set=1, binding=3) uniform sampler2D gBufferMetallicRoughnessOcclusion;

// Prefiltered reflection buffer
layout(set=1, binding=4) uniform sampler2D reflectionBuffer;

#include <PBR/PBRMaterial.glsl>

#include "SDFUtils.glsl"

vec4 sampleReflection(float roughness) {
  return textureLod(reflectionBuffer, uv, 4.0 * roughness).rgba;
} 

vec3 sampleEnvMap(vec3 dir) {
  float yaw = atan(dir.z, dir.x);
  float pitch = -atan(dir.y, length(dir.xz));
  vec2 envMapUV = vec2(0.5 * yaw, pitch) / PI + 0.5;
   
  return textureLod(environmentMap, envMapUV, 0.0).rgb;
}           
                     
void main() {   
  vec3 cameraPos = globals.inverseView[3].xyz;
             
  vec3 envMapSample = sampleEnvMap(direction);
  vec3 rayDir = normalize(direction); 
  float t = rayMarchSdf(cameraPos, rayDir);
             
  if (t < 0.0) {     
    envMapSample = vec3(1.0) - exp(-envMapSample * globals.exposure);
    outColor = vec4(envMapSample, 1.0);
    return;  
  }    
 
  vec3 pos = cameraPos + t * rayDir; 
  vec3 normal = gradSDF(pos);

  vec3 irradianceColor = sampleIrrMap(normal);
  vec3 reflectedDirection = reflect(normalize(direction), normal);

  Material mat = SDFMaterial(pos, rayDir, normal, irradianceColor);
  vec4 reflectedColor = vec4(sampleEnvMap(reflectedDirection, mat.roughness), 1.0);

  vec3 material = 
      pbrMaterial(
        normalize(direction),
        globals.lightDir, 
        normal, 
        mat.color, 
        reflectedColor.rgb, 
        irradianceColor,
        mat.metallic, 
        mat.roughness, 
        mat.occlusion,
        mat.emission);

  material = vec3(1.0) - exp(-material * globals.exposure);
  outColor = vec4(material, 1.0);
}