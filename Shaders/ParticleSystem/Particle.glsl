#ifndef _PARTICLE_
#define _PARTICLE_

struct Particle {
  vec3 position;
  float radius;
  vec3 velocity;
  uint padding;
  vec3 nextPosition;
  uint debug;
};

#endif // _PARTICLE_