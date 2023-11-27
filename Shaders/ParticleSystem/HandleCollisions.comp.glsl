
#version 450

#define PI 3.14159265359

#define CELL_HASH_MASK 0xFFFF0000
#define PARTICLE_IDX_MASK 0x0000FFFF
#define INVALID_INDEX 0xFFFFFFFF

#define COLLISION_STEPS 1

layout(local_size_x = 32) in;

#include "Hash.glsl"
#include "Particle.glsl"
#include "SimUniforms.glsl"

layout(std430, set=0, binding=1) buffer PARTICLES_BUFFER {
  Particle particles[];
};

layout(std430, set=0, binding=2) buffer readonly CELL_TO_BUCKET_BUFFER {
  uint cellToBucket[];
};

void checkPair(inout Particle particle, Particle other)
{
  vec3 diff = other.position.xyz - particle.position.xyz;
  float dist = length(diff);
  float sep = dist - 2.0 * particleRadius;

  if (sep <= 0.0) {
    particle.debug = 1; // mark collision
    if (dist < 0.00001)
      diff = vec3(-1.0, 0.0, 0.0);
    else 
      diff /= dist;

    // Reflect the portion of the velocity going towards the collision
    // relative velocity
    vec3 dv = other.velocity - particle.velocity;
    float projection = dot(dv, diff);
    vec3 rejection = dv - diff * projection;

    float restitution = 0.01;
    float friction = 0.1;
    float bias = 0.01;
    float k = 1.0 / float(COLLISION_STEPS);

    particle.nextPosition -= k * restitution * diff * max(projection, 0.0) * deltaTime;
    particle.nextPosition += k * bias * sep * diff;

    // friction
    particle.nextPosition += k * friction * rejection * deltaTime;
    // adhesion ??
    // particle.nextPosition += friction * dv * deltaTime;
  }
}

void checkBucket(inout Particle particle, uint particleIdx, uint nextParticleIdx)
{
  for (int i = 0; i < 16; ++i)
  {
    if (nextParticleIdx == INVALID_INDEX)
    {
      // At the end of the linked-list
      return;
    }

    if (particleIdx == nextParticleIdx)
    {
      // Skip the current particle
      nextParticleIdx = particle.nextParticleLink;
      continue;
    }

    // Check any valid particle pairs in the bucket
    Particle other = particles[nextParticleIdx];
    checkPair(particle, other);
  }
}

void checkGridCell(inout Particle particle, uint particleIdx, int i, int j, int k)
{
  uint gridCellHash = hashCoords(i, j, k);
  uint entryLocation = (gridCellHash >> 16) % spatialHashSize;

  for (uint probeStep = 0; probeStep < spatialHashProbeSteps; ++probeStep) {
    uint entry = cellToBucket[entryLocation];

    if (entry == INVALID_INDEX)
    {
      // Break if we hit an empty slot, this should not happen
      break;
    }

    if ((entry & CELL_HASH_MASK) == (gridCellHash & CELL_HASH_MASK)) {
      // If we found an entry corresponding to this cell, check the bucket
      // for potentially colliding pairs
      uint headParticleIdx = entry & PARTICLE_IDX_MASK;
      checkBucket(particle, particleIdx, headParticleIdx);
      break;
    }

    // Otherwise continue the linear probe of the hashmap
    ++entryLocation;
    if (entryLocation == spatialHashSize)
      entryLocation = 0;
  }
}

uint findGridCell(int i, int j, int k)
{
  uint gridCellHash = hashCoords(i, j, k);
  uint entryLocation = (gridCellHash >> 16) % spatialHashSize;

  for (uint probeStep = 0; probeStep < spatialHashProbeSteps; ++probeStep) {
    uint entry = cellToBucket[entryLocation];

    if (entry == INVALID_INDEX)
    {
      // Break if we hit an empty slot, this should not happen
      return INVALID_INDEX;
    }

    if ((entry & CELL_HASH_MASK) == (gridCellHash & CELL_HASH_MASK)) {
      // Found an entry corresponding to this cell
      return entry;
    }

    // Otherwise continue the linear probe of the hashmap
    ++entryLocation;
    if (entryLocation == spatialHashSize)
      entryLocation = 0;
  }

  // Did not find an entry corresponding to this grid-cell within the max
  // number of linear probe steps
  return INVALID_INDEX;
}

void checkWallCollisions(inout Particle particle)
{
  float k = 1.0 / float(COLLISION_STEPS);

  float wallRestitution = 0.0;
  float wallFriction = 0.1;
  float wallBias = 1.0;//1;

  float gridLength = 100.0;
  float minPos = particleRadius;
  float maxPos = gridLength - particleRadius;
  for (int i = 0; i < 3; ++i)
  {
    if (particle.nextPosition[i] <= minPos)
    {
      // bias
      particle.nextPosition[i] += k * wallBias * (minPos - particle.nextPosition[i]);
      // restitution
      particle.nextPosition[i] -= k * wallRestitution * min(particle.velocity[i], 0.0) * deltaTime;
      // friction
      particle.nextPosition[(i+1)%3] -= k * wallFriction * particle.velocity[(i+1)%3] * deltaTime;
      particle.nextPosition[(i+2)%3] -= k * wallFriction * particle.velocity[(i+2)%3] * deltaTime;
    }  

    if (particle.nextPosition[i] >= maxPos)
    {
      // bias
      particle.nextPosition[i] -= k * wallBias * (particle.nextPosition[i] - maxPos);
      // restitution
      particle.nextPosition[i] -= k * wallRestitution * max(particle.velocity[i], 0.0) * deltaTime;
      // friction
      particle.nextPosition[(i+1)%3] -= k * wallFriction * particle.velocity[(i+1)%3] * deltaTime;
      particle.nextPosition[(i+2)%3] -= k * wallFriction * particle.velocity[(i+2)%3] * deltaTime;
    }
  }
}

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= particleCount) {
    return;
  }

  Particle particle = particles[particleIdx];
  
  vec3 gridPos = (worldToGrid * vec4(particle.position.xyz, 1.0)).xyz;
  ivec3 gridCell = ivec3(floor(gridPos));

  int gridCellCount = 0;
  uint gridCellEntries[27];

  // The grid cell size is setup so that a particle could be colliding with
  // other particles from any of the 27 cells immediately surrounding it, so
  // check each one for potential collisions.
  for (int i = gridCell.x - 1; i < (gridCell.x + 1); ++i) {
    for (int j = gridCell.y - 1; j < (gridCell.y + 1); ++j) {
      for (int k = gridCell.z - 1; k < (gridCell.z + 1); ++k) {
        uint entry = findGridCell(i, j, k);
        if (entry != INVALID_INDEX)
          gridCellEntries[gridCellCount++] = entry;
      }
    }
  }
  
  // This type of iteration might be useless, might need to use updated positions in each
  // iteration...

  // TODO: Put this in uniforms...
  for (int solverStep = 0; solverStep < COLLISION_STEPS; ++solverStep)
  {
    checkWallCollisions(particle);
    
    // TODO: Would it be ridiculous to cache the other particle structs instead of fetching them
    // in each solver iteration??
    for (int i = 0; i < gridCellCount; ++i)
    {
      uint entry = gridCellEntries[i];
      uint particleBucketHeadIdx = entry & PARTICLE_IDX_MASK;
      checkBucket(particle, particleIdx, particleBucketHeadIdx);
    }
  }

  // if (gridCellCount > 1)
  //   particle.debug = 2;

  particles[particleIdx] = particle;
}
