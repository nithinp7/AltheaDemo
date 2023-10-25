
#version 450

#define GLOBAL_UNIFORMS_SET 0
#define GLOBAL_UNIFORMS_BINDING 4
#include <GlobalUniforms.glsl>

#include "Particle.glsl"

layout(std430, set=1, binding=1) readonly buffer PARTICLES_BUFFER {
  Particle particles[];
};

layout(location=0) out vec3 worldPos;

void main() {
  Particle particle = particles[gl_VertexIndex];

  worldPos = particle.position;
  gl_Position = globals.projection * globals.view * vec4(particle.position, 1.0);
  gl_PointSize = 10.0; // TODO: Use distance from camera 
}