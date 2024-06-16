#ifndef _SIMRESOURCES_
#define _SIMRESOURCES_

#include <Global/GlobalResources.glsl>
#include <Global/GlobalUniforms.glsl>

#include "Hash.glsl"

#define PARTICLES_PER_BUCKET 16

#define INPUT_MASK_MOUSE_LEFT 1
#define INPUT_MASK_MOUSE_RIGHT 2
#define INPUT_MASK_SPACEBAR 4

#define INVALID_INDEX 0xFFFFFFFF

#extension GL_EXT_nonuniform_qualifier : enable

layout(push_constant) uniform PushConstant {
  uint globalResourcesHandle;
  uint globalUniformsHandle;
  uint simUniformsHandle;
  uint iteration; // TODO: This is hacky, sort out how to do multiple push constants...
} pushConstants;

#define resources RESOURCE(globalResources, pushConstants.globalResourcesHandle)
#define globals RESOURCE(globalUniforms, pushConstants.globalUniformsHandle)

struct LiveValues {
  float slider1;
  float slider2;
  bool checkbox1;
  bool checkbox2;
};

UNIFORM_BUFFER(_simUniforms, SimUniforms{
  mat4 gridToWorld;
  mat4 worldToGrid;

  vec3 interactionLocation;
  uint padding;
  
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

  uint particlesHeap;
  uint spatialHashHeap;
  uint bucketHeap;
  uint nextFreeBucket;

  LiveValues liveValues;
});
#define simUniforms _simUniforms[pushConstants.simUniformsHandle]

// TODO: Would it be useful to have the previous pos cached here as well??
struct ParticleBucketEntry {
  vec4 positions[2]; // ping pong buffer
};

struct ParticleBucket {
  ParticleBucketEntry particles[PARTICLES_PER_BUCKET];
};

struct Particle {
  vec3 position;
  uint globalIndex;
  vec3 prevPosition;
  uint debug;
};

BUFFER_RW(_particlesHeap, PARTICLES_BUFFER{
  Particle particles[];
});
#define getParticle(particleIdx)                    \
    _particlesHeap[                                 \
      simUniforms.particlesHeap +                   \
      (particleIdx) / simUniforms.particlesPerBuffer] \
        .particles[                                 \
          (particleIdx) % simUniforms.particlesPerBuffer]

BUFFER_RW(_spatialHashHeap, SPATIAL_HASH_HEAP{
  uint spatialHash[];
});
#define getSpatialHashSlot(slotIdx)                    \
    _spatialHashHeap[                                       \
      simUniforms.spatialHashHeap +                         \
      (slotIdx) / simUniforms.spatialHashEntriesPerBuffer]    \
        .spatialHash[                                       \
          (slotIdx) % simUniforms.spatialHashEntriesPerBuffer]

// TODO: These should probably be heaps as well??
BUFFER_RW(_nextFreeBucket, NEXT_FREE_BUCKET{
  uint freeList[];
});
#define getBucketFreeList(freeListIdx)  \
    _nextFreeBucket[simUniforms.nextFreeBucket].freeList[freeListIdx]

BUFFER_RW(_bucketHeap, PARTICLE_BUCKETS{
  ParticleBucket buckets[];
});
#define getBucket(bucketIdx)                            \
    _bucketHeap[                                        \
      simUniforms.bucketHeap +                 \
      (bucketIdx) / simUniforms.particleBucketsPerBuffer] \
        .buckets[                                       \
          (bucketIdx) % simUniforms.particleBucketsPerBuffer]

// A "u32 globalParticleIdx" has a bottom 4-bits representing a bucket-local
// index (0-15), the rest of the bits are the index of the bucket itself
#define getParticleEntry(globalParticleIdx)        \
    getBucket((globalParticleIdx) >> 4).particles[(globalParticleIdx) & 0xF]

// #define getPosition(globalParticleIdx, phase)      \\
//     getParticleEntry(globalParticleIdx).positions[phase].xyz;

// #define setPosition(globalParticleIdx, pos, phase) \\
//   {getParticleEntry(globalParticleIdx).positions[phase].xyz = pos;}

vec3 getPosition(uint globalParticleIdx, uint phase) {
  return getParticleEntry(globalParticleIdx).positions[phase].xyz;
}

void setPosition(uint globalParticleIdx, vec3 pos, uint phase) {
  getParticleEntry(globalParticleIdx).positions[phase].xyz = pos;
}

// Increment the particle count of a cell, called during the 
// pre-sizing pass. Returns the slot idx of the grid cell
uint incrementCellParticleCount(int i, int j, int k) {
  uint gridCellHash = hashCoords(i, j, k);
  uint slotIdx = gridCellHash % simUniforms.spatialHashSize;
  
  atomicAdd(getSpatialHashSlot(slotIdx), 1);

  return slotIdx;
}

// This should get called after the sizing-pass, each cell with a non-zero
// amount of particles gets dynamically allocated a fixed-size particle bucket
void allocateBucketForCell(uint slotIdx) {
  uint particleCount = getSpatialHashSlot(slotIdx);

  if (particleCount != INVALID_INDEX) {
    uint freeListIdx = slotIdx % simUniforms.freeListsCount;
    // // TODO: Measure how much contention is seen here...
    uint freeListCounter = atomicAdd(getBucketFreeList(freeListIdx), 1);
    uint bucketIdx = (freeListCounter * simUniforms.freeListsCount + freeListIdx) % simUniforms.particleBucketCount;
    uint globalIdx = bucketIdx << 4;

    getSpatialHashSlot(slotIdx) = globalIdx;
  }
}

uint hashInsertPosition(uint slotIdx, vec3 position) {
  // Bump-allocates a particle entry within the bucket that 
  // exists in this hash slot
  uint particleGlobalIdx = atomicAdd(getSpatialHashSlot(slotIdx), 1);

  // TODO: Understand this 0-phase?
  setPosition(particleGlobalIdx, position, 0);

  return particleGlobalIdx;
}

uint spatialHashAtomicExchange(int i, int j, int k, uint newValue) {
  uint gridCellHash = hashCoords(i, j, k);
  uint slotIdx = gridCellHash % simUniforms.spatialHashSize;

  return atomicExchange(getSpatialHashSlot(slotIdx), newValue);
}
#endif // _SIMRESOURCES_
