
#version 450

#define PI 3.14159265359

// Need to reduce this...
#define CAMERA_RADIUS 10.0
#define CAMERA_STRENGTH -0.0001

#extension GL_KHR_shader_subgroup_basic : enable
#extension GL_KHR_shader_subgroup_shuffle : enable
#extension GL_KHR_shader_subgroup_arithmetic : enable 

layout(local_size_x = LOCAL_SIZE_X) in;

#include "SimResources.glsl"
#include <Misc/Input.glsl>

// shared uint[LOCAL_SIZE_X] ss
#define getPosition(globalParticleIdx)      \
    getParticleEntry(globalParticleIdx).positions[pushConstants.iteration % 2].xyz;

#define setPosition(globalParticleIdx, pos) \
  {getParticleEntry(globalParticleIdx).positions[(pushConstants.iteration + 1) % 2].xyz = pos;}

#include "PBFluids.glsl"

void checkPair(inout vec3 relPos, inout float partialDensity, vec3 particlePos, uint particleIdx, vec3 otherParticlePos, uint otherParticleIdx)
{
  vec3 diff = otherParticlePos - particlePos;// - 0.1 * deltaPos;
  float distSq = dot(diff, diff);

  partialDensity = 0.0;

  float r = 2.0 * simUniforms.particleRadius;//0.5 * simUniforms.particleRadius;
  float r2 = r * r;
  if (distSq < r2) 
  {
    relPos = diff * smoothingKernelDeriv(r2, distSq);
    partialDensity = smoothingKernel(r2, distSq);
  }
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
  vec3 relPos;
  float partialDensity;
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
        outp.relPos, 
        outp.partialDensity, 
        particle.pos, 
        particle.particleIdx, 
        otherParticlePos, 
        inp.otherParticleIdx);

  taskOutputs[taskId] = outp;  
}

void checkBucket(inout vec3 deltaPos, inout float density, inout uint collidingParticlesCount, vec3 particlePos, uint thisParticleIdx, uint bucketEnd)
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

  // #pragma optionNV (unroll all)
  for (uint i = 0; i < particleCount; ++i)
  {
    uint taskId = taskStart + i;
    uint otherParticleIdx = bucketStart + i;
    taskInputs[taskId] = TaskInput(threadId, otherParticleIdx);
  }

  subgroupBarrier();

  // for (uint taskId = threadId; taskId < taskCount; taskId += gl_SubgroupSize)
  uint givenTaskStart = iters * threadId;
  // #pragma optionNV (unroll all)
  for (uint taskId = givenTaskStart; taskId < min(givenTaskStart + iters, taskCount); ++taskId)
  {    
    processTask(taskId);
  }

  subgroupBarrier();
  // #pragma optionNV (unroll all)
  for (uint i = taskStart; i < taskEnd; ++i)
  {
    TaskOutput partialResult = taskOutputs[i];
    if (partialResult.partialDensity > 0.0)
    {
      density += partialResult.partialDensity;
      deltaPos += partialResult.relPos;
      collidingParticlesCount++;
    }
  }
}

void checkBucket2(inout vec3 deltaPos, inout float density, inout uint collidingParticlesCount, vec3 particlePos, uint thisParticleIdx, uint bucketEnd)
{
  uint bucketStart = bucketEnd & ~0xF;
  for (uint globalParticleIdx = bucketStart; globalParticleIdx < bucketEnd; ++globalParticleIdx)
  {
    // Check any valid particle pairs in the bucket
    if (globalParticleIdx != thisParticleIdx)
    {
      vec3 otherParticlePos = getPosition(globalParticleIdx);
      vec3 dp;
      float d;

      checkPair(
          dp, 
          d, 
          particlePos, 
          thisParticleIdx, 
          otherParticlePos, 
          globalParticleIdx);
      if (d > 0.0) {
        deltaPos += dp;
        density += d;
        collidingParticlesCount++;
      }
    }
  }
}

void checkWallCollisions(inout vec3 deltaPos, inout uint collidingParticlesCount, vec3 particlePos)
{
  float k = 1.0;// / float(jacobiIters);

  float wallBias = 1.0;

  vec3 gridLength =  vec3(60.0);
  if (simUniforms.liveValues.checkbox1)
    gridLength[0] = 60.0 * simUniforms.liveValues.slider1 + 20.0 * sin(0.25 * simUniforms.time);// 5.0
  vec3 minPos = vec3(simUniforms.particleRadius);
  vec3 maxPos = gridLength - vec3(simUniforms.particleRadius);
  for (int i = 0; i < 3; ++i)
  {
    if (particlePos[i] <= minPos[i])
    {
      deltaPos[i] += (minPos[i] - particlePos[i]);
      ++collidingParticlesCount;
    }  

    if (i != 1 && particlePos[i] >= maxPos[i])
    {
      deltaPos[i] -= (particlePos[i] - maxPos[i]);
      ++collidingParticlesCount;
    }
  }

  if (!simUniforms.liveValues.checkbox1 && bool(globals.inputMask & INPUT_BIT_LEFT_MOUSE))
  {
    // TODO: Create the projected cam position and upload in 
    // uniforms, there is more flexibility that way and is probably
    // more efficient
    float camRadius = CAMERA_RADIUS;
    float camRadiusSq = camRadius * camRadius;

    vec3 cameraPos = globals.inverseView[3].xyz;
    vec3 dir = normalize(-globals.inverseView[2].xyz);

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
}

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= simUniforms.particleCount) {
    return;
  }

  Particle particle = getParticle(particleIdx);
  
  uint globalParticleIdx = particle.globalIndex;
  vec3 particlePos = getPosition(globalParticleIdx);
  
  thisParticle[gl_SubgroupInvocationID] = ThisParticle(particlePos, globalParticleIdx);

  vec3 gridPos = (simUniforms.worldToGrid * vec4(particlePos, 1.0)).xyz;
  vec3 gridCellF = floor(gridPos);
  ivec3 gridCell = ivec3(gridCellF);
  vec3 cellLocalPos = gridPos - gridCellF;
  if (cellLocalPos.x < 0.5)
    --gridCell.x;
  if (cellLocalPos.y < 0.5)
    --gridCell.y;
  if (cellLocalPos.z < 0.5)
    --gridCell.z;

  vec3 comSum = vec3(0.0);
  float density = 0.0; // ??
  uint collidingParticlesCount = 0;
  bool anyParticleCollisions = false;

  // The grid cell size is setup so that a particle could be colliding with
  // other particles from any of the 8 cells immediately surrounding it, so
  // check each one for potential collisions.
  for (int i = 0; i < 8; ++i) {
    uint hash = hashCoords(gridCell.x + (i>>2), gridCell.y + ((i>>1)&1), gridCell.z + (i&1));
    uint bucketEnd = getSpatialHashSlot(hash % simUniforms.spatialHashSize);
    
    //if (bucketEnd != INVALID_INDEX) 
    {
      checkBucket(comSum, density, collidingParticlesCount, particlePos, globalParticleIdx, bucketEnd);
    }
  }
  
  if (collidingParticlesCount > 0)
    anyParticleCollisions = true;

  vec3 deltaPos = vec3(0.0);

  vec3 wallDisp = vec3(0.0);
  uint hasWallCollisions = 0;
  checkWallCollisions(wallDisp, hasWallCollisions, particlePos);
  if (hasWallCollisions > 0)
  {
    deltaPos += 0.5 * wallDisp;
  }

  if (collidingParticlesCount > 0)
  {
    float targetDensity = 0.2;
    vec3 com = comSum / float(collidingParticlesCount + 1);
    // deltaPos += 0.5 * (targetDensity - density) * com; 
  }

  float mag = length(deltaPos);
  if (mag > 0.001)
  {
    // deltaPos /= mag;// + 0.0001;
    // mag = clamp(mag, 0.0, 1. * simUniforms.particleRadius);
    // deltaPos *= mag;

    particlePos += deltaPos;
  }

  setPosition(globalParticleIdx, particlePos);
}
