#ifndef _PARTICLE_
#define _PARTICLE_

struct Particle {
  vec3 position;
  uint padding;
  vec3 velocity;
  uint nextParticleLink;
  vec3 REMOVE;
  uint debug;
};

#endif // _PARTICLE_