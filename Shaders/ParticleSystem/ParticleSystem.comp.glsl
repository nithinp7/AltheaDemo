
#version 450

#define CELL_HASH_MASK 0xFFFF0000
#define PARTICLE_IDX_MASK 0x0000FFFF
#define INVALID_INDEX 0xFFFFFFFF

layout(local_size_x = 32) in;

#include "Hash.glsl"
#include "Particle.glsl"

// TODO: Move to separate file
layout(set=0, binding=0) uniform SimUniforms {
  mat4 gridToWorld;
  mat4 worldToGrid;
  
  uint particleCount;
  uint spatialHashSize;
  uint spatialHashProbeSteps;

  float deltaTime;
};

layout(std430, set=0, binding=1) buffer PARTICLES_BUFFER {
  Particle particles[];
};

layout(std430, set=0, binding=2) buffer PARTICLE_TO_CELL_BUFFER {
  uint cellToBucket[];
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

  float maxSpeed = 2.0;

  particle.velocity = (particle.nextPosition - particle.position) / deltaTime; // TODO: overwrite velocity
  particle.position = particle.nextPosition;
  
  vec3 acceleration = vec3(0.0, -1.0, 0.0);
  particle.velocity += acceleration * deltaTime;

  float speed = length(particle.velocity);
  if (speed > maxSpeed)
  {
    particle.velocity *= maxSpeed / speed;
  }

  particle.nextPosition += particle.velocity * deltaTime;

  float wallRestitution = 0.1;
  float wallFriction = 0.1;
  float wallBias = 0.05;

  float gridLength = 100.0;
  float minPos = particle.radius;
  float maxPos = gridLength - particle.radius;
  for (int i = 0; i < 3; ++i)
  {
    if (particle.nextPosition[i] <= minPos)
    {
      // bias
      particle.nextPosition[i] += wallBias * (minPos - particle.nextPosition[i]);
      // restitution
      particle.nextPosition[i] -= wallRestitution * min(particle.velocity[i], 0.0) * deltaTime;
      // friction
      particle.nextPosition[(i+1)%3] -= wallFriction * particle.velocity[(i+1)%3] * deltaTime;
      particle.nextPosition[(i+2)%3] -= wallFriction * particle.velocity[(i+2)%3] * deltaTime;
    }  

    if (particle.nextPosition[i] >= maxPos)
    {
      // bias
      particle.nextPosition[i] -= wallBias * (particle.nextPosition[i] - maxPos);
      // restitution
      particle.nextPosition[i] -= wallRestitution * max(particle.velocity[i], 0.0) * deltaTime;
      // friction
      particle.nextPosition[(i+1)%3] -= wallFriction * particle.velocity[(i+1)%3] * deltaTime;
      particle.nextPosition[(i+2)%3] -= wallFriction * particle.velocity[(i+2)%3] * deltaTime;
    }
  }
 
  vec3 gridPos = (worldToGrid * vec4(particle.position, 1.0)).xyz;
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
