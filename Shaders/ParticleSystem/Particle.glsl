#ifndef _PARTICLE_
#define _PARTICLE_

struct Particle {
  vec3 position;
  uint nextParticleLink;
  uint debug;
};

#endif // _PARTICLE_