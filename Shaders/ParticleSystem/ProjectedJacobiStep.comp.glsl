
#version 450

#define PI 3.14159265359

// Need to reduce this...
#define CAMERA_RADIUS 10.0
#define CAMERA_STRENGTH -0.0001

#extension GL_KHR_shader_subgroup_basic : enable
#extension GL_KHR_shader_subgroup_shuffle : enable
#extension GL_KHR_shader_subgroup_arithmetic : enable 

layout(local_size_x = LOCAL_SIZE_X) in;

#include "Particle.glsl"
#include "SimResources.glsl"

layout(push_constant) uniform PushConstants {
  uint iteration;
} pushConstants;

// shared uint[LOCAL_SIZE_X] ss

vec3 getPosition(uint globalParticleIdx)
{
  uint phase = pushConstants.iteration % 2;
  return getPosition(globalParticleIdx, phase);
}

void setPosition(uint globalParticleIdx, vec3 pos)
{
  uint phase = (pushConstants.iteration + 1) % 2;
  setPosition(globalParticleIdx, pos, phase);
}

void checkPair(inout vec3 deltaPos, inout uint collidingParticlesCount, vec3 particlePos, uint particleIdx, vec3 otherParticlePos, uint otherParticleIdx)
{
  // TODO: Should use nextPos or prevPos?
  vec3 diff = otherParticlePos - particlePos;// - 0.1 * deltaPos;
  // float dist = length(diff);
  float distSq = dot(diff, diff);

// #define HACKY_ADHESION
#ifdef HACKY_ADHESION
  float adhesionBuffer = 0.01;
  float innerRadius = particleRadius - adhesionBuffer;
  float minDist = 2.0 * innerRadius;
  float minDistSq = 4.0 * innerRadius * innerRadius;
#else
  float radius = 1. * particleRadius;//0.5 * particleRadius;
  float minDist = 2.0 * radius;
  float minDistSq = 4.0 * radius * radius;
#endif
  // float sep = dist - 2.0 * innerRadius;
  // float sep = dist - 2.0 * particleRadius;

  if (distSq < minDistSq) {
    // particle.debug = 1; // mark collision
    float dist = sqrt(distSq);
    if (dist > 0.000001)
      diff /= dist;
    else if (particleIdx < otherParticleIdx)
      diff = vec3(1.0, 0.0, 0.0);
    else 
      diff = vec3(-1.0, 0.0, 0.0);

    float sep = dist - minDist;

    float bias = 0.5;
    float k = 1.0;// / float(jacobiIters);

    deltaPos += k * bias * sep * diff;
    collidingParticlesCount++;
  }
  // else 
  //   collidingParticlesCount++; // ??
}

// Assuming subgroup size of 32
struct ThisParticle
{
  vec3 pos;
  uint particleIdx;
};
shared ThisParticle thisParticle[32];

struct TaskInput {
  uint originalThreadId;
  uint otherParticleIdx;
};

struct TaskOutput {
  vec3 deltaPos;
  uint collidingParticlesCount;
};

// TODO: Use a smaller size and refuse to process more than that many?
// Or have a fallback??
// 32 * 16 = 512
shared TaskInput taskInputs[512];
shared TaskOutput taskOutputs[512];

void processTask(uint taskId)
{
  TaskInput inp = taskInputs[taskId];
  ThisParticle particle = thisParticle[inp.originalThreadId];

  vec3 otherParticlePos = getPosition(inp.otherParticleIdx);
  
  TaskOutput outp = TaskOutput(vec3(0.0), 0);
  
  if (particle.particleIdx != inp.otherParticleIdx)
    checkPair(
        outp.deltaPos, 
        outp.collidingParticlesCount, 
        particle.pos, 
        particle.particleIdx, 
        otherParticlePos, 
        inp.otherParticleIdx);

  taskOutputs[taskId] = outp;  
}

void checkBucket(inout vec3 deltaPos, inout uint collidingParticlesCount, vec3 particlePos, uint thisParticleIdx, uint bucketEnd)
{
  uint threadId = gl_SubgroupInvocationID;
  
  // TODO: Can move this step to main function, is not bucket-specific
  // Make this particle position visible to other threads

  uint bucketStart = bucketEnd & ~0xF;
  uint particleCount = bucketEnd == INVALID_INDEX ? 0 : (bucketEnd & 0xF); // ??

  // TODO: Just to be safe... try without this later...
  subgroupBarrier();

  uint taskStart = subgroupExclusiveAdd(particleCount);
  uint taskEnd = taskStart + particleCount;
  uint taskCount = subgroupShuffle(taskEnd, gl_SubgroupSize-1);

  uint iters = (taskCount-1) / gl_SubgroupSize + 1;

  for (uint i = 0; i < particleCount; ++i)
  {
    uint taskId = taskStart + i;
    uint otherParticleIdx = bucketStart + i;
    taskInputs[taskId] = TaskInput(threadId, otherParticleIdx);
  }

  subgroupBarrier();

  // for (uint taskId = threadId; taskId < taskCount; taskId += gl_SubgroupSize)
  uint givenTaskStart = iters * threadId;
  for (uint taskId = givenTaskStart; taskId < min(givenTaskStart + iters, taskCount); ++taskId)
  {    
    processTask(taskId);
  }

  subgroupBarrier();
  
  for (uint i = taskStart; i < taskEnd; ++i)
  {
    TaskOutput partialResult = taskOutputs[i];
    deltaPos += partialResult.deltaPos;
    collidingParticlesCount += partialResult.collidingParticlesCount;
  }
}

void checkBucket2(inout vec3 deltaPos, inout uint collidingParticlesCount, vec3 particlePos, uint thisParticleIdx, uint bucketEnd)
{
  uint bucketStart = bucketEnd & ~0xF;
  for (uint globalParticleIdx = bucketStart; globalParticleIdx < bucketEnd; ++globalParticleIdx)
  {
    // Check any valid particle pairs in the bucket
    if (globalParticleIdx != thisParticleIdx)
    {
      vec3 otherParticlePos = getPosition(globalParticleIdx);
      checkPair(deltaPos, collidingParticlesCount, particlePos, thisParticleIdx, otherParticlePos, globalParticleIdx);
    }
  }
}

void checkWallCollisions(inout vec3 deltaPos, inout uint collidingParticlesCount, vec3 particlePos)
{
  float k = 1.0;// / float(jacobiIters);

  float wallBias = 1.0;

  vec3 gridLength = vec3(60.0);
  // gridLength[0] = 60.0 + 20.0 * sin(0.25 * time);// 5.0
  vec3 minPos = vec3(particleRadius);
  vec3 maxPos = gridLength - vec3(particleRadius);
  // for (int i = 0; i < 3; ++i)
  int i = 1;
  {
    if (particlePos[i] <= minPos[i])
    {
      deltaPos[i] += k * wallBias * (minPos[i] - particlePos[i]);
      ++collidingParticlesCount;
    }  

#if 1
    if (i != 1 && particlePos[i] >= maxPos[i])
    {
      deltaPos[i] -= k * wallBias * (particlePos[i] - maxPos[i]);
      ++collidingParticlesCount;
    }
#endif
  }

#if 1
if (bool(inputMask & INPUT_MASK_MOUSE_LEFT))
{
  // TODO: Create the projected cam position and upload in 
  // uniforms, there is more flexibility that way and is probably
  // more efficient
  float camRadius = CAMERA_RADIUS;
  float camRadiusSq = camRadius * camRadius;

  vec3 cameraPos = inverseView[3].xyz;
  vec3 dir = normalize(-inverseView[2].xyz);

  // Solve for t to find whether and where the camera ray intersects the
  // floor plane
  // c.y + t * d.y = 0

  float t = -1.0;
  if (abs(dir.y) > 0.0001)
  {
    t = -cameraPos.y / dir.y;
  } 

  if (t < 0.0)
  {
    t = 10.0;
    //return;
  }

  t = clamp(t, 0.0, 10.0);

  vec3 userBallPos = cameraPos + t * dir;// + vec3(0.0, 5.0, 0.0);
  vec3 camDiff = particlePos - userBallPos;
  float camDistSq = dot(camDiff, camDiff) + 0.01;
  if (camDistSq < camRadiusSq)
  {
    float camDist = sqrt(camDistSq);
    if (camDistSq > 0.25 * camRadiusSq)
    {
      // if (bool(inputMask & INPUT_MASK_MOUSE_LEFT))
        deltaPos += - 1.* camDiff / camDist / camDistSq;
    }
    else 
    {
      deltaPos += 5.0 * CAMERA_STRENGTH * camDistSq * camDiff / camDist;
    }
    // deltaPos += CAMERA_STRENGTH * camRadius * camDiff / camDist;
    ++collidingParticlesCount;
  }
}
#endif
}

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= particleCount) {
    return;
  }

  Particle particle = getParticle(particleIdx);
  
  uint globalParticleIdx = particle.globalIndex;
  vec3 particlePos = getPosition(globalParticleIdx);
  
  thisParticle[gl_SubgroupInvocationID] = ThisParticle(particlePos, globalParticleIdx);

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
    uint bucketEnd = getSpatialHashSlot(gridCell.x + (i>>2), gridCell.y + ((i>>1)&1), gridCell.z + (i&1));
    //if (bucketEnd != INVALID_INDEX) 
    {
      checkBucket(deltaPos, collidingParticlesCount, particlePos, globalParticleIdx, bucketEnd);
    }
  }
  
  if (collidingParticlesCount > 0)
    anyParticleCollisions = true;

  checkWallCollisions(deltaPos, collidingParticlesCount, particlePos);

  // deltaPos /= float(jacobiIters);
  if (collidingParticlesCount > 0)
  {
    float mag = length(deltaPos) + 0.0001;
    float k = clamp(mag, 0.0, 0.25 * particleRadius) / mag;
    // k *= 1.0 / mag;
    // float k = 1./mag;//5 / float(collidingParticlesCount);
    // k /= float(pushConstants.iteration + 1);
    // k = pow(1.0 - k, 1.0 / float(pushConstants.iteration + 1));
    particlePos += k * deltaPos;
  }

  setPosition(globalParticleIdx, particlePos);
}
