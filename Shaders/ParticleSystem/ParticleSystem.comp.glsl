
#version 450

#define CELL_HASH_MASK 0xFFFF0000
#define PARTICLE_IDX_MASK 0x0000FFFF

layout(local_size_x = 16, local_size_y = 16) in;

#include "Particle.glsl"

// TODO: Move to separate file
layout(set=0, binding=0) uniform SimUniforms {
  mat4 gridToWorld;
  mat4 worldToGrid;
  
  uint xCells;
  uint yCells;
  uint zCells;

  uint particleCount;
  float deltaTime;

  float padding[3];
};

layout(std430, set=0, binding=1) buffer PARTICLES_BUFFER {
  Particle particles[];
};

layout(std430, set=0, binding=2) buffer PARTICLE_TO_CELL_BUFFER {
  uint particleToCell[];
};

// From Mathias Muller
uint hashCoords(int x, int y, int z) {
  return abs((x * 92837111) ^ (y * 689287499) ^ (z * 283923481));
}

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= particleCount) {
    return;
  }

  Particle particle = particles[particleIdx];
  
  vec3 acceleration = vec3(0.0, -0.01, 0.0);
  particle.velocity.xyz += acceleration * deltaTime;
  particle.position.xyz += particle.velocity.xyz * deltaTime;

  particle.position.xyz = clamp(particle.position.xyz, vec3(0.0), vec3(xCells, yCells, zCells));

  vec3 gridPos = (worldToGrid * vec4(particle.position.xyz, 1.0)).xyz;
  gridPos = clamp(gridPos, vec3(0.0), vec3(xCells, yCells, zCells));

  particle.position.xyz = (gridToWorld * vec4(gridPos, 1.0)).xyz;
  
  // if (gridCell.x < 0 || gridCell.x >= xCells ||
  //     gridCell.y < 0 || gridCell.y >= yCells ||
  //     gridCell.z < 0 || gridCell.z >= zCells) {
  //   return;
  // }

  ivec3 gridCell = ivec3(floor(gridPos));
  uint gridCellHash = hashCoords(gridCell.x, gridCell.y, gridCell.z);
  particle.gridCellHash = gridCellHash;
  
  particles[particleIdx] = particle;

  // hash map with 50% load factor 
  uint hashMapSize = 2 * particleCount;

  uint entryLocation = gridCellHash % hashMapSize;
  uint entry = gridCellHash & CELL_HASH_MASK | particleIdx & PARTICLE_IDX_MASK;
  
  // TODO: Handle a valid entry==0 case?? i.e. hash=0 and pid=0
  for (uint i = 0; i < 10; ++i) {
    uint prevEntry = atomicExchange(particleToCell[entryLocation], entry);
    // TODO: Do we need to exclusively check the grid cell hash
    // or can we just compare the whole cell/particle entry?
    if (prevEntry == 0) 
    {
      break;
    }
    else if ((prevEntry & CELL_HASH_MASK) < (entry & CELL_HASH_MASK))
    {
      entryLocation = (entryLocation == 0) ? (hashMapSize - 1) : (entryLocation - 1);
    }
    else 
    {
      ++entryLocation;
      if (entryLocation == hashMapSize)
        entryLocation = 0;
    }

    entry = prevEntry;
  }
}
