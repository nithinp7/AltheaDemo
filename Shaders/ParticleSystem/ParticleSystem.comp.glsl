
#version 450

#define CELL_HASH_MASK 0xFFFF0000
#define PARTICLE_IDX_MASK 0x0000FFFF
#define INVALID_INDEX 0xFFFFFFFF

layout(local_size_x = LOCAL_SIZE_X) in;

#include "Hash.glsl"
#include "Particle.glsl"
#include "SimUniforms.glsl"

layout(std430, set=0, binding=1) buffer PARTICLES_BUFFER {
  Particle particles[];
};

layout(std430, set=0, binding=2) buffer PARTICLE_TO_CELL_BUFFER {
  uint cellToBucket[];
};

layout(std430, set=0, binding=3) buffer POSITIONS_A {
  vec4 positionsA[];
};

layout(std430, set=0, binding=4) buffer POSITIONS_B {
  vec4 positionsB[];
};

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= particleCount) {
    return;
  }

  Particle particle = particles[particleIdx];

  particle.nextParticleLink = INVALID_INDEX;

  // Clear any debug flags for this frame
  particle.debug = 0;

  vec3 nextPos = positionsB[particleIdx].xyz;
  vec3 velocity = (nextPos - particle.position) / deltaTime;
  particle.position = nextPos;
  
  // apply gravity and drag
  float drag = 0.1;
  float gravity = 0.8;
  vec3 acceleration = vec3(0.0, -gravity, 0.0) - drag * velocity;
  velocity += acceleration * deltaTime;

  float friction = 0.5;
  if (nextPos.y <= 0.0)
    velocity.xz -= friction * velocity.xz * deltaTime;

  // Initial estimate of particle position
  vec3 projectedPos = particle.position + velocity * deltaTime;

  positionsA[particleIdx].xyz = projectedPos;

  vec3 gridPos = (worldToGrid * vec4(projectedPos, 1.0)).xyz;
  ivec3 gridCell = ivec3(floor(gridPos));
  uint gridCellHash = hashCoords(gridCell.x, gridCell.y, gridCell.z);
  
  uint entryLocation = (gridCellHash >> 16) % spatialHashSize;

  // Whether we find an entry for this grid cell or create a new one, this particle
  // will become the new head of the particle bucket linked-list.
  uint entry = (gridCellHash & CELL_HASH_MASK) | (particleIdx & PARTICLE_IDX_MASK);

  for (uint i = 0; i < spatialHashProbeSteps; ++i) {
    uint prevEntry = atomicCompSwap(cellToBucket[entryLocation], INVALID_INDEX, entry);
     
    if (prevEntry == INVALID_INDEX) 
    {
      // Encountered an empty slot and replaced it with a newly created entry for the 
      // grid cell, with this particle as linked-list head
      break;
    }
    
#ifdef PROBE_FOR_EMPTY_SLOT
    if ((prevEntry & CELL_HASH_MASK) == (gridCellHash & CELL_HASH_MASK))
#endif
    {
      // An entry already exists for this grid cell, we have so far peaked it and know
      // that the key (grid cell hash) cannot change for this entry. However we need to 
      // atomically swap out the linked-list head to make sure it doesn't become stale.
      prevEntry = atomicExchange(cellToBucket[entryLocation], entry);
      particle.nextParticleLink = prevEntry & PARTICLE_IDX_MASK;
      break;
    }
    
#ifdef PROBE_FOR_EMPTY_SLOT
    // This is a non-empty entry corresponding to a different grid-cell hash, continue
    // linearly probing the hashmap.
    ++entryLocation;
    if (entryLocation == spatialHashSize)
      entryLocation = 0;
#endif
  }
  
  // Write-back the modified particle data
  particles[particleIdx] = particle;
}
