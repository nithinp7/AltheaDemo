
#version 450

#define PI 3.14159265359

#define INVALID_INDEX 0xFFFFFFFF

layout(local_size_x = LOCAL_SIZE_X) in;

#include "Particle.glsl"
#include "SimResources.glsl"

layout(push_constant) uniform PushConstants {
  uint phase;
} pushConstants;

// shared uint[LOCAL_SIZE_X] ss

vec3 getPosition(uint idx)
{
  if (pushConstants.phase == 0)
  {
    return getPositionA(idx);
  }
  else 
  {
    return getPositionB(idx);
  }
}

void setPosition(uint idx, vec3 pos)
{
  if (pushConstants.phase == 0)
  {
    setPositionB(idx, pos);
  }
  else 
  {
    setPositionA(idx, pos);
  }
}

void checkPair(inout vec3 deltaPos, inout uint collidingParticlesCount, vec3 particlePos, vec3 otherParticlePos)
{
  // TODO: Should use nextPos or prevPos?
  vec3 diff = otherParticlePos - particlePos;
  // float dist = length(diff);
  float distSq = dot(diff, diff);

  // float adhesionBuffer = 0.025;
  // float innerRadius = particleRadius - adhesionBuffer;
  float minDist = 2.0 * particleRadius;
  float minDistSq = 4.0 * particleRadius * particleRadius;

  // float sep = dist - 2.0 * innerRadius;
  // float sep = dist - 2.0 * particleRadius;

  if (distSq < minDistSq) {
    // particle.debug = 1; // mark collision
    float dist = sqrt(distSq);
    if (dist > 0.000001)
      diff /= dist;
    else
      diff = vec3(1.0, 0.0, 0.0);

    float sep = dist - minDist;

    float bias = 0.5;
    float k = 1.0;/// float(jacobiIters);

    deltaPos += k * bias * sep * diff;
    collidingParticlesCount++;
  }
}

void checkBucket(inout vec3 deltaPos, inout uint collidingParticlesCount, vec3 particlePos, uint particleIdx, uint nextParticleIdx)
{
  for (int i = 0; i < 4; ++i)
  {
    if (nextParticleIdx == INVALID_INDEX)
    {
      // At the end of the linked-list
      return;
    }

    // Check any valid particle pairs in the bucket
    if (particleIdx != nextParticleIdx)
    {
      vec3 otherParticlePos = getPosition(nextParticleIdx);
      checkPair(deltaPos, collidingParticlesCount, particlePos, otherParticlePos);
    }
    
    Particle nextParticle = getParticle(nextParticleIdx);
    nextParticleIdx = nextParticle.nextParticleLink;
  }
}

void checkWallCollisions(inout vec3 deltaPos, inout uint collidingParticlesCount, vec3 particlePos)
{
  float k = 1.0;// / float(jacobiIters);

  float wallBias = 1.0;

  vec3 gridLength = vec3(30.0);
  // gridLength[0] = 20.0 + 8.0 * sin(1.0 * time);// 5.0
  vec3 minPos = vec3(particleRadius);
  vec3 maxPos = gridLength - vec3(particleRadius);
  for (int i = 0; i < 3; ++i)
  {
    if (particlePos[i] <= minPos[i])
    {
      deltaPos[i] += k * wallBias * (minPos[i] - particlePos[i]);
      ++collidingParticlesCount;
    }  

    if (particlePos[i] >= maxPos[i])
    {
      deltaPos[i] -= k * wallBias * (particlePos[i] - maxPos[i]);
      ++collidingParticlesCount;
    }
  }

#if 0
  float camRadius = 2.0;

  vec3 camPos = inverseView[3].xyz + -inverseView[2].xyz * 2.0;
  vec3 camDiff = particlePos - camPos;
  float camDist = length(camDiff);
  if (camDist < camRadius)
  {
    deltaPos += camRadius * camDiff / camDist;
    ++collidingParticlesCount;
  }
#endif
}

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= particleCount) {
    return;
  }

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

  vec3 deltaPos = vec3(0.0);
  uint collidingParticlesCount = 0;
  bool anyParticleCollisions = false;

  // The grid cell size is setup so that a particle could be colliding with
  // other particles from any of the 8 cells immediately surrounding it, so
  // check each one for potential collisions.
  for (int i = 0; i < 8; ++i) {
    uint particleBucketHeadIdx = getSpatialHashSlot(gridCell.x + (i>>2), gridCell.y + ((i>>1)&1), gridCell.z + (i&1));
    if (particleBucketHeadIdx != INVALID_INDEX) {
      checkBucket(deltaPos, collidingParticlesCount, particlePos, particleIdx, particleBucketHeadIdx);
    }
  }
  
  if (collidingParticlesCount > 0)
    anyParticleCollisions = true;

  checkWallCollisions(deltaPos, collidingParticlesCount, particlePos);

  if (collidingParticlesCount > 0)
  {
    particlePos += deltaPos / float(collidingParticlesCount);
  }

  setPosition(particleIdx, particlePos);
}
