
#version 450

layout(local_size_x = LOCAL_SIZE_X) in;

#include <Misc/Input.glsl>
#include "SimResources.glsl"
#include "Hash.glsl"

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= simUniforms.particleCount) {
    return;
  }

  bool newlyAdded = particleIdx >= (simUniforms.particleCount - simUniforms.addedParticles);

  float dt = simUniforms.deltaTime;

  Particle particle = getParticle(particleIdx);

  // TODO: Find better function name here...

  vec3 velocity = vec3(0.0);
  
  // if (false)
  if (!newlyAdded)
  {
    uint phase = simUniforms.jacobiIters % 2;
    ParticleBucketEntry particleEntry = getParticleEntry(particle.globalIndex);
    vec3 nextPos = particleEntry.positions[phase].xyz;
    vec3 stabilization = nextPos - particleEntry.positions[1-phase].xyz;
    // stabilization = vec3(0.0); // TODO: TEMP

    vec3 diff = nextPos - particle.prevPosition - stabilization;
    velocity = diff / dt;

    float friction = 2.0;
    vec3 projection = dot(stabilization, velocity) * stabilization;
    vec3 rejection = velocity - projection;
    // velocity -= rejection * friction * dt;

    particle.position = nextPos;
    particle.prevPosition = nextPos;
  }

  float friction = 0.;//4;//5;
  if (particle.position.y <= simUniforms.particleRadius * 1.5)
    velocity.xz -= friction * velocity.xz * dt;

  // apply gravity and drag
  float drag = 0.;//5;
  float gravity = 3;
  vec3 acceleration = vec3(0.0, -gravity, 0.0) - drag * velocity;
  velocity += acceleration * dt;

  float maxSpeed = 3;
  float speed = length(velocity);
  if (speed > maxSpeed)
    velocity *= maxSpeed / speed;

  // Initial estimate of particle position
  // particle.position += velocity * dt;

  // setPosition(particleIdx, projectedPos, 0);

  vec3 gridPos = (simUniforms.worldToGrid * vec4(particle.position, 1.0)).xyz;
  ivec3 gridCell = ivec3(floor(gridPos));

  particle.position += velocity * dt;

  // Store the particle grid cell hash
  particle.globalIndex = incrementCellParticleCount(gridCell.x, gridCell.y, gridCell.z);

  // Whether we find an entry for this grid cell, entry for a hash-colliding cell, or create a 
  // a new entry, this particle will become the new head of the particle bucket linked-list.
  // The previous entry (either invalid or a particle idx) will become the "next" link in the bucket
  // particle.nextParticleLink = spatialHashAtomicExchange(gridCell.x, gridCell.y, gridCell.z, particleIdx);

#if 1
if (particleIdx >= (simUniforms.particleCount - simUniforms.addedParticles))
{
    vec3 col = vec3(1.0, 0.2, 0.1);
    uvec3 ucol = uvec3(255.0 * col.xyz);
    particle.debug = (ucol.x << 16) | (ucol.y << 8) | ucol.z;
}
if (bool(globals.inputMask & INPUT_BIT_SPACE))
{
  vec3 col = fract(particle.position / 50.0);
  uvec3 ucol = uvec3(255.0 * col.xyz);
  particle.debug = (ucol.x << 16) | (ucol.y << 8) | ucol.z;
}
#elif 1
  vec3 col = 0.5 * velocity / speed + vec3(0.5);
  col.y = 0.0;
  uvec3 ucol = uvec3(255.0 * col);
  particle.debug = (ucol.x << 16) | (ucol.y << 8) | ucol.z;
#endif

  // Write-back the modified particle data
  getParticle(particleIdx) = particle;
}
