
#version 450

#define INVALID_INDEX 0xFFFFFFFF

layout(local_size_x = LOCAL_SIZE_X) in;

#include "Particle.glsl"
#include "SimResources.glsl"
#include "Hash.glsl"

// Random number generator and sample warping
// from ShaderToy https://www.shadertoy.com/view/4tXyWN
// uvec2 seed;
// float rng() {
//     seed += uvec2(1);
//     uvec2 q = 1103515245U * ( (seed >> 1U) ^ (seed.yx) );
//     uint  n = 1103515245U * ( (q.x) ^ (q.y >> 3U) );
//     return float(n) * (1.0 / float(0xffffffffU));
// }

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= particleCount) {
    return;
  }

#if 0
  int uTime = int(1000.0 * time);
  uint randomParticleSwizzle = hashCoords(uTime, uTime+1, uTime+2);
  particleIdx = (particleIdx + randomParticleSwizzle) % particleCount;
#endif

  float dt = 1* deltaTime;

  Particle particle = getParticle(particleIdx);

  particle.nextParticleLink = INVALID_INDEX;

  // Clear any debug flags for this frame
  //particle.debug = 0;

  vec3 nextPos = getPositionB(particleIdx).xyz;
  vec3 stabilization = nextPos - getPositionA(particleIdx).xyz;
  // stabilization = vec3(0.0);
  vec3 diff = nextPos - particle.position - stabilization;
  vec3 velocity = diff / dt;
  // if (dot(velocity, velocity) < 0.001)
  // {
  //   velocity = vec3(0.0);
  // } 
  // else
   {
    particle.position = nextPos;
  }

  // vec3 stabilization = vec3(0.0);
  
  float friction = 0.;//5;
  if (nextPos.y <= particleRadius * 1.5)
    velocity.xz -= friction * velocity.xz * dt;

  // apply gravity and drag
  float drag = 0.;//5;
  float gravity = 3.;
  vec3 acceleration = vec3(0.0, -gravity, 0.0) - drag * velocity;
  velocity += acceleration * dt;

  float maxSpeed = 5;
  float speed = length(velocity);
  if (speed > maxSpeed)
    velocity *= maxSpeed / speed;

  // Initial estimate of particle position
  vec3 projectedPos = particle.position + velocity * dt;

  setPositionA(particleIdx, projectedPos);

  vec3 gridPos = (worldToGrid * vec4(projectedPos, 1.0)).xyz;
  ivec3 gridCell = ivec3(floor(gridPos));

  // Whether we find an entry for this grid cell, entry for a hash-colliding cell, or create a 
  // a new entry, this particle will become the new head of the particle bucket linked-list.
  // The previous entry (either invalid or a particle idx) will become the "next" link in the bucket
  particle.nextParticleLink = spatialHashAtomicExchange(gridCell.x, gridCell.y, gridCell.z, particleIdx);
  
if (bool(inputMask & INPUT_MASK_SPACEBAR))
  {
    vec3 col = fract(projectedPos / 2.0);
    uvec3 ucol = uvec3(255.0 * col.xyz);
    particle.debug = (ucol.x << 16) | (ucol.y << 8) | ucol.z;
  }
#if 0
  vec3 col = 0.5 * velocity / speed + vec3(0.5);
  uvec3 ucol = uvec3(255.0 * col);
  particle.debug = (ucol.x << 16) | (ucol.y << 8) | ucol.z;
#endif
  // Write-back the modified particle data
  setParticle(particleIdx, particle);
}
