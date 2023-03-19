#version 450

#define PI 3.14159265359

layout(location=0) in vec3 inNormal;
layout(location=1) in vec3 inDirection;

layout(location=0) out vec4 color;

layout(set=0, binding=0) uniform sampler2D environmentMap; 
layout(set=0, binding=1) uniform sampler2D prefilteredMap; 
layout(set=0, binding=2) uniform sampler2D irradianceMap;
layout(set=0, binding=3) uniform sampler2D brdfLut;

layout(set=0, binding=4) uniform UniformBufferObject {
  mat4 projection;
  mat4 inverseProjection;
  mat4 view;
  mat4 inverseView;
  vec3 lightDir;
  float time;
  float exposure;
} globals;

layout(set=1, binding=0) uniform samplerCubeArray sceneCaptureTexArr;

layout(push_constant) uniform PushConstants {
  mat4 model;
  uint sceneCaptureIndex;
} pushConstants;

#include <PBR/PBRMaterial.glsl>

void main() {
  vec3 normal = normalize(inNormal);
  vec3 direction = normalize(inDirection);

  float metallic = 0.0;
  float roughness = 0.2;
  vec3 baseColor = vec3(1.0, 1.0, 1.0);

  float ambientOcclusion = 1.0;

  vec3 reflectedDirection = reflect(direction, normal);
  vec3 reflectedColor = sampleEnvMap(reflectedDirection, roughness);
  vec3 irradianceColor = sampleIrrMap(normal);

  vec3 material = 
      pbrMaterial(
        direction,
        globals.lightDir, 
        normal, 
        baseColor.rgb, 
        reflectedColor, 
        irradianceColor,
        metallic, 
        roughness, 
        ambientOcclusion);

  color = vec4(material, 1.0);

  // Convert to OpenGL conventions.
  // Note: The captured cubemap is expected to intentionally have 
  // +X and -X switched to make this work - 
  // TODO: Look for a cleaner way...
  reflectedDirection.x *= -1.0; 
  color.rgb *= 
      texture(
        sceneCaptureTexArr, 
        vec4(reflectedDirection, pushConstants.sceneCaptureIndex)).rgb;
  
#ifndef SKIP_TONEMAP
  color.rgb = vec3(1.0) - exp(-color.rgb * globals.exposure);
#endif
}