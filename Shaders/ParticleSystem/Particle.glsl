#ifndef _PARTICLE_
#define _PARTICLE_

struct Particle {
  vec3 position;
  uint globalIndex;
  vec3 prevPosition;
  uint debug;
};

#endif // _PARTICLE_