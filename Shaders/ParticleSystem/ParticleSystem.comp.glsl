
#version 450

#define CELL_HASH_MASK 0xFFFF0000
#define PARTICLE_IDX_MASK 0x0000FFFF

layout(local_size_x = 32) in;

#include "Hash.glsl"
#include "Particle.glsl"

// TODO: Move to separate file
layout(set=0, binding=0) uniform SimUniforms {
  mat4 gridToWorld;
  mat4 worldToGrid;
  
  uint xCells;
  uint yCells;
  uint zCells;

  uint particleCount;
  uint spatialHashSize;
  uint spatialHashProbeSteps;

  float deltaTime;

  float padding;
};

layout(std430, set=0, binding=1) buffer PARTICLES_BUFFER {
  Particle particles[];
};

layout(std430, set=0, binding=2) buffer PARTICLE_TO_CELL_BUFFER {
  uint particleToCell[];
};

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= particleCount) {
    return;
  }

  Particle particle = particles[particleIdx];

  // Clear any debug flags for this frame
  particle.debug = 0;
  
  vec3 acceleration = vec3(0.0, -0.1, 0.0);
  particle.velocity.xyz += acceleration * deltaTime;
  particle.position.xyz += particle.velocity.xyz * deltaTime;

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
  
  particles[particleIdx] = particle;

  uint entryLocation = (gridCellHash >> 16) % spatialHashSize;
  uint entry = (gridCellHash & CELL_HASH_MASK) | (particleIdx & PARTICLE_IDX_MASK);
  // TODO: This is a HACK
  // uint entry = (entryLocation << 16) | (particleIdx & PARTICLE_IDX_MASK);
  
  // TODO: Cleanup the documentation here
  // there is an "entryLocation" which acts as the _actual_ insertion slot
  // and there are "cur/prevEntrylocations" which are the ideal insertion
  // slot given no collisions. Regardless of the actual insertion locations,
  // we want the each entry to have weakly ascending "ideal" entry locations
  // This means when looking for a key, we can start at the ideal entry location
  // and scan in one direction

  for (uint i = 0; i < spatialHashProbeSteps; ++i) {
    uint prevEntry = atomicMin(particleToCell[entryLocation], entry);
    //uint prevEntryLocation = (prevEntry >> 16) % spatialHashSize;
    //uint curEntryLocation = (entry >> 16) % spatialHashSize;
     
    if (prevEntry == 0xFFFFFFFF) 
    {
      break;
    }
    // else if (prevEntryLocation < curEntryLocation)
    // {
    //   entryLocation = (entryLocation == 0) ? (spatialHashSize - 1) : (entryLocation - 1);
    // }
    else 
    {
      ++entryLocation;
      if (entryLocation == spatialHashSize)
        entryLocation = 0;
    }

    entry = max(entry, prevEntry);
  }
}
