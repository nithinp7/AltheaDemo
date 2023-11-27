#ifndef _PARTICLE_
#define _PARTICLE_

struct Particle {
  vec3 position;
  float radius;
  vec3 velocity;
  float density;
  vec3 nextPosition;
  uint debug;
};

#endif // _PARTICLE_