#ifndef _SIMRESOURCES_
#define _SIMRESOURCES_

#include "Particle.glsl"
#include "Hash.glsl"

#define INPUT_MASK_MOUSE_LEFT 1
#define INPUT_MASK_MOUSE_RIGHT 2
#define INPUT_MASK_SPACEBAR 4

#define INVALID_INDEX 0xFFFFFFFF

#ifndef SIM_RESOURCES_SET
#define SIM_RESOURCES_SET 0
#endif

#extension GL_EXT_nonuniform_qualifier : enable

layout(set=SIM_RESOURCES_SET, binding=0) uniform SimUniforms {
  mat4 gridToWorld;
  mat4 worldToGrid;

  mat4 inverseView;

  vec3 interactionLocation;
  uint inputMask;
  
  uint particleCount;
  uint particlesPerBuffer;
  uint spatialHashSize;
  uint spatialHashEntriesPerBuffer;

  uint jacobiIters;
  float deltaTime;
  float particleRadius;
  float time;

  uint addedParticles;
  uint particleBucketCount;
  uint particleBucketsPerBuffer;
  uint freeListsCount;
};

// TODO: Would it be useful to have the previous pos cached here as well??
struct ParticleBucketEntry {
  vec4 positions[2]; // ping pong buffer
};

struct ParticleBucket {
  ParticleBucketEntry particles[16];
};

layout(std430, set=SIM_RESOURCES_SET, binding=1) buffer PARTICLES_BUFFER {
  Particle particles[];
} particlesHeap[];

layout(std430, set=SIM_RESOURCES_SET, binding=2) buffer SPATIAL_HASH_BUFFER {
  uint spatialHash[];
} spatialHashHeap[];

// TODO: These should probably be heaps as well??

layout(std430, set=SIM_RESOURCES_SET, binding=3) buffer NEXT_FREE_BUCKET {
  uint nextFreeBucket[];
};

layout(std430, set=SIM_RESOURCES_SET, binding=4) buffer PARTICLE_BUCKETS {
  ParticleBucket particleBuckets[];
} particleBucketsHeap[];

ParticleBucketEntry getParticleEntry(uint globalParticleIdx)
{
  uint bucketIdx = globalParticleIdx >> 4;
  uint localParticleIdx = globalParticleIdx & 0xF;

  uint bucketBufferIdx = bucketIdx / particleBucketsPerBuffer;
  uint bucketLocalIdx = bucketIdx % particleBucketsPerBuffer;

  return particleBucketsHeap[bucketBufferIdx].particleBuckets[bucketLocalIdx].particles[localParticleIdx];
}

vec3 getPosition(uint globalParticleIdx, uint phase) {
  ParticleBucketEntry entry = getParticleEntry(globalParticleIdx);
  return entry.positions[phase].xyz;
}

void setPosition(uint globalParticleIdx, vec3 pos, uint phase) {
  uint bucketIdx = globalParticleIdx >> 4;
  uint localParticleIdx = globalParticleIdx & 0xF;

  uint bucketBufferIdx = bucketIdx / particleBucketsPerBuffer;
  uint bucketLocalIdx = bucketIdx % particleBucketsPerBuffer;

  particleBucketsHeap[bucketBufferIdx]
      .particleBuckets[bucketLocalIdx]
      .particles[localParticleIdx]
      .positions[phase].xyz = pos;
}

// Increment the particle count of a cell, called during the 
// pre-sizing pass. Returns the slot idx of the grid cell
uint incrementCellParticleCount(int i, int j, int k) {
  uint gridCellHash = hashCoords(i, j, k);
  uint slotIdx = gridCellHash % spatialHashSize;

  uint bufferIdx = slotIdx / spatialHashEntriesPerBuffer;
  uint localSlotIdx = slotIdx % spatialHashEntriesPerBuffer;

  atomicAdd(spatialHashHeap[bufferIdx].spatialHash[localSlotIdx], 1);

  return slotIdx;
}

// This should get called after the sizing-pass, each cell with a non-zero
// amount of particles gets dynamically allocated a fixed-size particle bucket
void allocateBucketForCell(uint slotIdx) {
  uint bufferIdx = slotIdx / spatialHashEntriesPerBuffer;
  uint localSlotIdx = slotIdx % spatialHashEntriesPerBuffer;

  uint particleCount = spatialHashHeap[bufferIdx].spatialHash[localSlotIdx];
  if (particleCount != INVALID_INDEX) {
    uint freeListIdx = slotIdx % freeListsCount;
    // // TODO: Measure how much contention is seen here...
    uint freeListCounter = atomicAdd(nextFreeBucket[freeListIdx], 1);
    uint bucketIdx = (freeListCounter * freeListsCount + freeListIdx) % particleBucketCount;
    uint globalIdx = bucketIdx << 4;

    spatialHashHeap[bufferIdx].spatialHash[localSlotIdx] = globalIdx;
  }
}

uint hashInsertPosition(uint slotIdx, vec3 position) {
  uint bufferIdx = slotIdx / spatialHashEntriesPerBuffer;
  uint localSlotIdx = slotIdx % spatialHashEntriesPerBuffer;
  
  uint particleGlobalIdx = atomicAdd(spatialHashHeap[bufferIdx].spatialHash[localSlotIdx], 1);

  uint bucketIdx = particleGlobalIdx >> 4;
  uint bucketBufferIdx = bucketIdx / particleBucketsPerBuffer;
  uint bucketLocalIdx = bucketIdx % particleBucketsPerBuffer;

  uint particleLocalIdx = particleGlobalIdx & 0xF;

  particleBucketsHeap[bucketBufferIdx]
      .particleBuckets[bucketLocalIdx]
      .particles[particleLocalIdx]
      .positions[0].xyz = position;

  return particleGlobalIdx;
}

Particle getParticle(uint particleIdx)
{
  uint bufferIdx = particleIdx / particlesPerBuffer;
  uint localIdx = particleIdx % particlesPerBuffer;
  return particlesHeap[bufferIdx].particles[localIdx];
}

void setParticle(uint particleIdx, Particle particle)
{
  uint bufferIdx = particleIdx / particlesPerBuffer;
  uint localIdx = particleIdx % particlesPerBuffer;
  particlesHeap[bufferIdx].particles[localIdx] = particle;
}

uint spatialHashAtomicExchange(int i, int j, int k, uint newValue) {
  uint gridCellHash = hashCoords(i, j, k);
  uint slotIdx = gridCellHash % spatialHashSize;

  uint bufferIdx = slotIdx / spatialHashEntriesPerBuffer;
  uint localSlotIdx = slotIdx % spatialHashEntriesPerBuffer;

  return atomicExchange(spatialHashHeap[bufferIdx].spatialHash[localSlotIdx], newValue);
}


uint getSpatialHashSlot(int i, int j, int k) {
  uint gridCellHash = hashCoords(i, j, k);
  uint slotIdx = gridCellHash % spatialHashSize;

  uint bufferIdx = slotIdx / spatialHashEntriesPerBuffer;
  uint localSlotIdx = slotIdx % spatialHashEntriesPerBuffer;

  return spatialHashHeap[bufferIdx].spatialHash[localSlotIdx];
}
#endif // _SIMRESOURCES_
