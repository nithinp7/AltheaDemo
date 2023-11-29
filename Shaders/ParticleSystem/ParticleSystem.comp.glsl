
#version 450

#define CELL_HASH_MASK 0xFFFF0000
#define PARTICLE_IDX_MASK 0x0000FFFF
#define INVALID_INDEX 0xFFFFFFFF

layout(local_size_x = 32) in;

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

  float maxSpeed = 10.0;

  vec3 nextPos = positionsB[particleIdx].xyz;
  particle.velocity = (nextPos - particle.position) / deltaTime; // TODO: overwrite velocity
  particle.position = nextPos;
  
  vec3 acceleration = vec3(0.0, -1.0, 0.0);
  particle.velocity += acceleration * deltaTime;

  float speed = length(particle.velocity);
  if (speed > maxSpeed)
  {
    particle.velocity *= maxSpeed / speed;
  }

  // Initial estimate of particle position
  vec3 projectedPos = particle.position + particle.velocity * deltaTime;

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
    
    if ((prevEntry & CELL_HASH_MASK) == (gridCellHash & CELL_HASH_MASK))
    {
      // An entry already exists for this grid cell, we have so far peaked it and know
      // that the key (grid cell hash) cannot change for this entry. However we need to 
      // atomically swap out the linked-list head to make sure it doesn't become stale.
      prevEntry = atomicExchange(cellToBucket[entryLocation], entry);
      particle.nextParticleLink = prevEntry & PARTICLE_IDX_MASK;
      break;
    }
    
    // This is a non-empty entry corresponding to a different grid-cell hash, continue
    // linearly probing the hashmap.
    ++entryLocation;
    if (entryLocation == spatialHashSize)
      entryLocation = 0;
  }
  
  // Write-back the modified particle data
  particles[particleIdx] = particle;
}
