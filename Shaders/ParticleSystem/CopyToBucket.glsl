
#version 450

layout(local_size_x = LOCAL_SIZE_X) in;

#include "SimResources.glsl"

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= simUniforms.particleCount) {
    return;
  }

  // Before this pass the global index represents the grid cell hash this particle lives in. After this pass
  // the global index represents the particle bucket location that the particle has been relocated to

  uint cellHash = getParticle(particleIdx).globalIndex;
  vec3 position = getParticle(particleIdx).position;

  getParticle(particleIdx).globalIndex = hashInsertPosition(cellHash, position);
}
