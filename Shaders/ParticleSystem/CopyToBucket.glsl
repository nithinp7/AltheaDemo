
#version 450

layout(local_size_x = LOCAL_SIZE_X) in;

#include "SimResources.glsl"

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= particleCount) {
    return;
  }

  Particle particle = getParticle(particleIdx);

  // Before this pass the global index represents the grid cell hash this particle lives in. After this pass
  // the global index represents the particle bucket location that the particle has been relocated to
  particle.globalIndex = hashInsertPosition(particle.globalIndex, particle.position);

  setParticle(particleIdx, particle);
}
