
#version 450

#define PI 3.14159265359

#define CELL_HASH_MASK 0xFFFF0000
#define PARTICLE_IDX_MASK 0x0000FFFF

layout(local_size_x = 32) in;

#include "Hash.glsl"
#include "Particle.glsl"

// TODO: Move to separate file
layout(set=0, binding=0) uniform SimUniforms {
  mat4 gridToWorld;
  mat4 worldToGrid;
  
  uint particleCount;
  uint spatialHashSize;
  uint spatialHashProbeSteps;

  float deltaTime;
};

layout(std430, set=0, binding=1) buffer PARTICLES_BUFFER {
  Particle particles[];
};

layout(std430, set=0, binding=2) buffer readonly PARTICLE_TO_CELL_BUFFER {
  uint particleToCell[];
};

float gaussianKernel(float d, float h) {
  float d2 = d * d;

  float h2 = h * h;
  float h3 = h * h2;

  // TODO: simplify / precompute
  return 1.0 / pow(PI, 2.0/3.0) / h3 * exp(d2 / h2);
}

void checkPair(inout Particle particle, uint otherParticleIdx)
{
  Particle other = particles[otherParticleIdx];

  vec3 diff = other.position.xyz - particle.position.xyz;
  float dist = length(diff);
  float sep = dist - other.radius - particle.radius;

  if (sep <= 0.0) {
    particle.debug = 1; // mark collision
    if (dist < 0.00001)
      diff = vec3(-1.0, 0.0, 0.0);
    else 
      diff /= dist;

#ifdef BALL_COLLISIONS
    // Reflect the portion of the velocity going towards the collision
    // relative velocity
    vec3 dv = other.velocity - particle.velocity;
    float projection = dot(dv, diff);
    vec3 rejection = dv - diff * projection;

    float restitution = 0.2;
    float friction = 0.1;

    particle.nextPosition += restitution * diff * max(projection, 0.0) * deltaTime;
    particle.nextPosition.xyz += sep * diff * 0.05; 

    // friction
    particle.nextPosition += friction * rejection * deltaTime;
#endif

#ifdef SPH
    float mass = 1.0; // fixed for now
    particle.density += mass * gaussianKernel(dist, particle.radius);
#endif
  }
}

void main() {
  uint particleIdx = uint(gl_GlobalInvocationID.x);
  if (particleIdx >= particleCount) {
    return;
  }

  Particle particle = particles[particleIdx];
  
  vec3 gridPos = (worldToGrid * vec4(particle.position.xyz, 1.0)).xyz;
  //gridPos = clamp(gridPos, vec3(0.0), vec3(xCells, yCells, zCells));

  ivec3 gridCell = ivec3(floor(gridPos));

  // for (int i = max(gridCell.x - 1, 0); i < min(gridCell.x + 1, xCells); ++i) {
  //   for (int j = max(gridCell.y - 1, 0); j < min(gridCell.y + 1, yCells); ++j) {
  //     for (int k = max(gridCell.z - 1, 0); k < min(gridCell.z + 1, zCells); ++k) { 
  for (int i = gridCell.x - 1; i < (gridCell.x + 1); ++i) {
    for (int j = gridCell.y - 1; j < (gridCell.y + 1); ++j) {
      for (int k = gridCell.z - 1; k < (gridCell.z + 1); ++k) {
        uint gridCellHash = hashCoords(i, j, k);
        uint entryLocation = (gridCellHash >> 16) % spatialHashSize; 
        // gridCellHash = entryLocation << 16; // HACK

        bool foundFirst = false;
        for (uint probeStep = 0; probeStep < spatialHashProbeSteps; ++probeStep) {
          uint entry = particleToCell[entryLocation];

          // Break if we hit a key that is not the one we are looking for, this includes
          // running into an empty slot
          if (entry == 0xFFFFFFFF)
            break;

          if ((entry & CELL_HASH_MASK) == (gridCellHash & CELL_HASH_MASK)) {
            foundFirst = true;
            ++entryLocation;
            if (entryLocation == spatialHashSize)
              entryLocation = 0;

            uint otherParticleIdx = entry & PARTICLE_IDX_MASK;
            if (particleIdx != otherParticleIdx)
              checkPair(particle, otherParticleIdx);
          } else /*if (foundFirst)*/ {
            // If we already found a particle with the desired key, the rest of the particles with 
            // the same key should be in contiguous slots
            // break;
            ++entryLocation;
          }
        }
      }
    }
  }
  
  particles[particleIdx] = particle;
}
