
#version 450

layout(local_size_x = 16, local_size_y = 16) in;

#include "Particle.glsl"

// TODO: Move to separate file
layout(set=0, binding=0) uniform SimUniforms {
  mat4 gridToWorld;
  mat4 worldToGrid;
  
  uint xCells;
  uint yCells;
  uint zCells;

  float deltaTime;
  uint particleCount;
};

layout(std430, set=0, binding=1) buffer PARTICLES_BUFFER {
  Particle particles[];
};

layout(std430, set=0, binding=2) buffer writeonly PARTICLE_TO_CELL_BUFFER {
  uint particleToCell[];
};

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= particleCount) {
    return;
  }

  Particle particle = particles[gl_GlobalInvocationID.x];
  
  vec3 acceleration = vec3(0.0, -1.0, 0.0);
  particle.velocity += acceleration * deltaTime;
  particle.position += particle.velocity * deltaTime;

  particles[particleIdx] = particle;

  vec3 gridPos = (worldToGrid * vec4(particle.position, 1.0)).xyz;
  ivec3 gridCell = ivec3(floor(gridPos));
  if (gridCell.x < 0 || gridCell.x >= xCells ||
      gridCell.y < 0 || gridCell.y >= yCells ||
      gridCell.z < 0 || gridCell.z >= zCells) {
    return;
  }

  uint gridCellIdx = (gridCell.x * yCells + gridCell.y) * zCells + gridCell.z;

  particleToCell[particleIdx] = gridCellIdx;
}
