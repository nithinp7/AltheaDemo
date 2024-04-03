
#version 450

#include "SimResources.glsl"

// Per-vertex attributes
layout(location=0) in vec3 vertexPos;

layout(location=0) out vec3 worldPos;
layout(location=1) out vec3 normal;
layout(location=2) out vec3 color;

void main() {
  Particle particle = getParticle(gl_InstanceIndex);

  worldPos = particle.position + 1.3 * simUniforms.particleRadius * vertexPos;
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