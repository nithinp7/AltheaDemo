
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

#include <PBR/PBRMaterial.glsl>

void main() {
  // vec2 uv = gl_FragCoord.xy;

  vec3 normal = texture(gBufferNormal, uv).xyz;
  vec3 baseColor = texture(gBufferAlbedo, uv).rgb;
  vec3 metallicRoughnessOcclusion = 
      texture(gBufferMetallicRoughnessOcclusion, uv).rgb;

  vec3 reflectedDirection = reflect(normalize(direction), normal);

  // TODO: Screen-space reflections!!
  vec3 reflectedColor = sampleEnvMap(reflectedDirection, metallicRoughnessOcclusion.y);
  vec3 irradianceColor = sampleIrrMap(normal);

  vec3 material = 
      pbrMaterial(
        normalize(direction),
        globals.lightDir, 
        normal, 
        baseColor.rgb, 
        reflectedColor, 
        irradianceColor,
        metallicRoughnessOcclusion.x, 
        metallicRoughnessOcclusion.y, 
        metallicRoughnessOcclusion.z);

#ifndef SKIP_TONEMAP
  material = vec3(1.0) - exp(-material * globals.exposure);
#endif

  outColor = vec4(material, 1.0);
}
