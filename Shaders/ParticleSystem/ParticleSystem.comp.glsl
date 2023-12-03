
#version 450

#define INVALID_INDEX 0xFFFFFFFF

layout(local_size_x = LOCAL_SIZE_X) in;

#include "Particle.glsl"
#include "SimResources.glsl"

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= particleCount) {
    return;
  }

  Particle particle = getParticle(particleIdx);

  particle.nextParticleLink = INVALID_INDEX;

  // Clear any debug flags for this frame
  particle.debug = 0;

  vec3 nextPos = getPositionB(particleIdx).xyz;
  vec3 velocity = (nextPos - particle.position) / deltaTime;
  particle.position = nextPos;
  
  // apply gravity and drag
  float drag = 0.1;
  float gravity = 0.5;
  vec3 acceleration = vec3(0.0, -gravity, 0.0) - drag * velocity;
  velocity += acceleration * deltaTime;

  float friction = 0.5;
  if (nextPos.y <= 0.0)
    velocity.xz -= friction * velocity.xz * deltaTime;

  float maxSpeed = 2.0;
  float speed = length(velocity);
  if (speed > maxSpeed)
    velocity *= maxSpeed / speed;

  // Initial estimate of particle position
  vec3 projectedPos = particle.position + velocity * deltaTime;

  setPositionA(particleIdx, projectedPos);

  vec3 gridPos = (worldToGrid * vec4(projectedPos, 1.0)).xyz;
  ivec3 gridCell = ivec3(floor(gridPos));

  // Whether we find an entry for this grid cell, entry for a hash-colliding cell, or create a 
  // a new entry, this particle will become the new head of the particle bucket linked-list.
  // The previous entry (either invalid or a particle idx) will become the "next" link in the bucket
  particle.nextParticleLink = spatialHashAtomicExchange(gridCell.x, gridCell.y, gridCell.z, particleIdx);
  
  // Write-back the modified particle data
  setParticle(particleIdx, particle);
}
