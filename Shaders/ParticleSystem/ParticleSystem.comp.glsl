
#version 450

layout(local_size_x = 16, local_size_y = 16) in;

#include "Particle.glsl"

layout(set=0, binding=0) uniform SimUniforms {
  float deltaTime;
  uint particleCount;
};

layout(std430, set=0, binding=1) buffer PARTICLES_BUFFER {
  Particle particles[];
};

void main() {
  if (uint(gl_GlobalInvocationID.x) >= particleCount) {
    return;
  }

  Particle particle = particles[gl_GlobalInvocationID.x];
  
  vec3 acceleration = vec3(0.0, -1.0, 0.0);
  particle.velocity += acceleration * deltaTime;
  particle.position += particle.velocity * deltaTime;

  particles[gl_GlobalInvocationID.x] = particle;
}
