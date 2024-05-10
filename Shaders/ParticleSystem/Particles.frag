
#version 450

#define GLOBAL_UNIFORMS_SET 0
#define GLOBAL_UNIFORMS_BINDING 4
#include <Global/GlobalUniforms.glsl>

layout(location=0) in vec3 normal;
layout(location=1) in vec3 color;

layout(location=0) out vec4 GBuffer_Normal;
layout(location=1) out vec4 GBuffer_Albedo;
layout(location=2) out vec4 GBuffer_MetallicRoughnessOcclusion;

void main() {
  GBuffer_Normal = vec4(normalize(normal), 1.0);
  GBuffer_Albedo = vec4(color, 1.0);
  GBuffer_MetallicRoughnessOcclusion = vec4(0.0, 0.05, 1.0, 1.0);
}
