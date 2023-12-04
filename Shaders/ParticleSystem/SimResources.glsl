#ifndef _SIMRESOURCES_
#define _SIMRESOURCES_

#include "Particle.glsl"
#include "Hash.glsl"

#define INPUT_MASK_MOUSE_LEFT 1
#define INPUT_MASK_MOUSE_RIGHT 2

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
  uint padding[3];
};

layout(std430, set=SIM_RESOURCES_SET, binding=1) buffer PARTICLES_BUFFER {
  Particle particles[];
} particlesHeap[];

layout(std430, set=SIM_RESOURCES_SET, binding=2) buffer SPATIAL_HASH_BUFFER {
  uint spatialHash[];
} spatialHashHeap[];

layout(std430, set=SIM_RESOURCES_SET, binding=3) buffer POSITIONS_A {
  vec4 positionsA[];
} positionsAHeap[];

layout(std430, set=SIM_RESOURCES_SET, binding=4) buffer POSITIONS_B {
  vec4 positionsB[];
} positionsBHeap[];

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

vec3 getPositionA(uint particleIdx)
{
  uint bufferIdx = particleIdx / particlesPerBuffer;
  uint localIdx = particleIdx % particlesPerBuffer;
  return positionsAHeap[bufferIdx].positionsA[localIdx].xyz;
}

vec3 getPositionB(uint particleIdx)
{
  uint bufferIdx = particleIdx / particlesPerBuffer;
  uint localIdx = particleIdx % particlesPerBuffer;
  return positionsBHeap[bufferIdx].positionsB[localIdx].xyz;
}

void setPositionA(uint particleIdx, vec3 pos)
{
  uint bufferIdx = particleIdx / particlesPerBuffer;
  uint localIdx = particleIdx % particlesPerBuffer;
  positionsAHeap[bufferIdx].positionsA[localIdx].xyz = pos;
}

void setPositionB(uint particleIdx, vec3 pos)
{
  uint bufferIdx = particleIdx / particlesPerBuffer;
  uint localIdx = particleIdx % particlesPerBuffer;
  positionsBHeap[bufferIdx].positionsB[localIdx].xyz = pos;
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
