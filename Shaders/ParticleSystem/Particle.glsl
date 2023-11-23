#ifndef _PARTICLE_
#define _PARTICLE_

struct Particle {
  vec4 position;
  vec4 velocity;
  uint gridCellHash;
  float radius;
  uint padding[2];
};

#endif // _PARTICLE_