
#version 450

#include "Particle.glsl"

// Per-vertex attributes
layout(location=0) in vec3 vertPos;

layout(std430, set=1, binding=1) readonly buffer PARTICLES_BUFFER {
  Particle particles[];
};

#define GLOBAL_UNIFORMS_SET 0
#define GLOBAL_UNIFORMS_BINDING 4
#include <GlobalUniforms.glsl>

layout(location=0) out vec3 worldPos;
layout(location=1) out vec3 normal;
layout(location=2) out vec3 color;

void main() {
  Particle particle = particles[gl_InstanceIndex];

  worldPos = particle.position.xyz + particle.radius * vertPos; 
  normal = vertPos;
  
  gl_Position = globals.projection * globals.view * vec4(worldPos, 1.0);

  color = particle.debug == 1 ? vec3(1.0, 0.0, 0.0) : vec3(0.4, 0.1, 0.9);
}