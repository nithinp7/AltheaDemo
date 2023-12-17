
#version 450

#include "Particle.glsl"

#define GLOBAL_UNIFORMS_SET 0
#define GLOBAL_UNIFORMS_BINDING 4
#include <GlobalUniforms.glsl>

#ifdef INSTANCED_MODE
// Per-vertex attributes
layout(location=0) in vec3 vertexPos;
#else
#extension GL_EXT_scalar_block_layout : enable

layout(scalar, set=0, binding=7) buffer readonly INDICES {
  uint indices[];
};

layout(scalar, set=0, binding=8) buffer readonly VERTICES {
  vec3 vertices[];
};
#endif 

#define SIM_RESOURCES_SET 1
#include "SimResources.glsl"

layout(location=0) out vec3 worldPos;
layout(location=1) out vec3 normal;
layout(location=2) out vec3 color;

layout(push_constant) uniform PushConstants {
  uint sphereIndexCount;
} pushConstants;

void main() {
#ifdef INSTANCED_MODE
  Particle particle = getParticle(gl_InstanceIndex);
#elif COHERENT_INSTANCED_MODE
  // subgroup coherent
  
  uint index = gl_SubgroupInvocationID
  todo...
#else
  uint instanceId = gl_VertexIndex / pushConstants.sphereIndexCount;
  Particle particle = getParticle(instanceId);
  uint index = indices[gl_VertexIndex % pushConstants.sphereIndexCount];
  vec3 vertexPos = vertices[index];
#endif 

  worldPos = particle.position + 1.3 * particleRadius * vertexPos;//vertPos; 
  normal = vertexPos;

  gl_Position = globals.projection * globals.view * vec4(worldPos, 1.0);

#if 1
  color = vec3(particle.debug >> 16, (particle.debug >> 8) & 0xff, particle.debug & 0xff) / 255.0;
#elif 0
  if (particle.debug == 1)
    color = vec3(1.0, 0.0, 0.0);
  else if (particle.debug == 2)
    color = vec3(0.0, 1.0, 0.0);
  else if (particle.debug == 3)
    color = vec3(1.0, 1.0, 0.0);
  else
    color = vec3(0.4, 0.1, 0.9);
#else
  color = vec3(0.4, 0.1, 0.9);
#endif
}