
#version 450

#define PI 3.14159265359

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

layout(std430, set=0, binding=2) buffer readonly CELL_TO_BUCKET_BUFFER {
  uint cellToBucket[];
};

layout(std430, set=0, binding=3) buffer POSITIONS_A {
  vec4 positionsA[];
};

layout(std430, set=0, binding=4) buffer POSITIONS_B {
  vec4 positionsB[];
};

layout(push_constant) uniform PushConstants {
  uint phase;
} pushConstants;

vec3 getPosition(uint idx)
{
  if (pushConstants.phase == 0)
  {
    return positionsA[idx].xyz;
  }
  else 
  {
    return positionsB[idx].xyz;
  }
}

void setPosition(uint idx, vec3 pos)
{
  if (pushConstants.phase == 0)
  {
    positionsB[idx].xyz = pos;
  }
  else 
  {
    positionsA[idx].xyz = pos;
  }
}

void checkPair(inout vec3 deltaPos, inout uint collidingParticlesCount, vec3 particlePos, vec3 otherParticlePos)
{
  // TODO: Should use nextPos or prevPos?
  vec3 diff = otherParticlePos - particlePos;
  float dist = length(diff);
  float sep = dist - 2.0 * particleRadius;

  if (sep <= 0.0) {
    // particle.debug = 1; // mark collision
    if (dist < 0.00001)
      diff = vec3(1.0, 0.0, 0.0);
    else 
      diff /= dist;

    float bias = 0.5;
    float k = 1.0 / float(jacobiIters);

    deltaPos += k * bias * sep * diff;
    collidingParticlesCount++;
  }
}

void checkBucket(inout vec3 deltaPos, inout uint collidingParticlesCount, Particle thisParticle, vec3 particlePos, uint particleIdx, uint nextParticleIdx)
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
      nextParticleIdx = thisParticle.nextParticleLink;
      continue;
    }

    // Check any valid particle pairs in the bucket
    Particle nextParticle = particles[nextParticleIdx];
    vec3 otherParticlePos = getPosition(nextParticleIdx);
    checkPair(deltaPos, collidingParticlesCount, particlePos, otherParticlePos);
    
    nextParticleIdx = nextParticle.nextParticleLink;
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

#ifdef PROBE_FOR_EMPTY_SLOT
    if ((entry & CELL_HASH_MASK) == (gridCellHash & CELL_HASH_MASK)) 
#endif
    {
      // Found an entry corresponding to this cell
      return entry;
    }

#ifdef PROBE_FOR_EMPTY_SLOT
    // Otherwise continue the linear probe of the hashmap
    ++entryLocation;
    if (entryLocation == spatialHashSize)
      entryLocation = 0;
#endif
  }

  // Did not find an entry corresponding to this grid-cell within the max
  // number of linear probe steps
  return INVALID_INDEX;
}

void checkWallCollisions(inout vec3 deltaPos, inout uint collidingParticlesCount, vec3 particlePos)
{
  float k = 1.0 / float(jacobiIters);

  float wallBias = 1.0;

  float gridLength = 15.0;
  float minPos = particleRadius;
  float maxPos = gridLength - particleRadius;
  for (int i = 0; i < 3; ++i)
  {
    if (particlePos[i] <= minPos)
    {
      deltaPos[i] += k * wallBias * (minPos - particlePos[i]);
      ++collidingParticlesCount;
    }  

    if (particlePos[i] >= maxPos)
    {
      deltaPos[i] -= k * wallBias * (particlePos[i] - maxPos);
      ++collidingParticlesCount;
    }
  }
}

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= particleCount) {
    return;
  }

  Particle particle = particles[particleIdx];
  vec3 particlePos = getPosition(particleIdx);
  
  vec3 gridPos = (worldToGrid * vec4(particlePos, 1.0)).xyz;
  vec3 gridCellF = floor(gridPos);
  ivec3 gridCell = ivec3(gridCellF);
  vec3 cellLocalPos = gridPos - gridCellF;
  if (cellLocalPos.x < 0.5)
    --gridCell.x;
  if (cellLocalPos.y < 0.5)
    --gridCell.y;
  if (cellLocalPos.z < 0.5)
    --gridCell.z;

  int gridCellCount = 0;
  uint gridCellEntries[8];

  // The grid cell size is setup so that a particle could be colliding with
  // other particles from any of the 8 cells immediately surrounding it, so
  // check each one for potential collisions.
  for (int i = gridCell.x; i <= (gridCell.x + 1); ++i) {
    for (int j = gridCell.y; j <= (gridCell.y + 1); ++j) {
      for (int k = gridCell.z; k <= (gridCell.z + 1); ++k) {
        uint entry = findGridCell(i, j, k);
        if (entry != INVALID_INDEX)
          gridCellEntries[gridCellCount++] = entry;
      }
    }
  }

  vec3 deltaPos = vec3(0.0);
  uint collidingParticlesCount = 0;

  {
    checkWallCollisions(deltaPos, collidingParticlesCount, particlePos);
    
    // TODO: Would it be ridiculous to cache the other particle structs instead of fetching them
    // in each solver iteration??
    for (int i = 0; i < gridCellCount; ++i)
    {
      uint entry = gridCellEntries[i];
      uint particleBucketHeadIdx = entry & PARTICLE_IDX_MASK;
      checkBucket(deltaPos, collidingParticlesCount, particle, particlePos, particleIdx, particleBucketHeadIdx);
    }
  }

  if (collidingParticlesCount > 0)
  {
    particlePos += deltaPos / float(collidingParticlesCount);
    // particles[particleIdx].debug = 1;
  }

  setPosition(particleIdx, particlePos);
}
