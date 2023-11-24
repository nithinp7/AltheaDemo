#ifndef _PARTICLE_
#define _PARTICLE_

struct Particle {
  vec4 position;
  vec4 velocity;
  float radius;
  uint debug;
  uint padding[2];
};

#endif // _PARTICLE_