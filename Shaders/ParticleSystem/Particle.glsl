#ifndef _PARTICLE_
#define _PARTICLE_

struct Particle {
  vec3 position;
  uint nextParticleLink;
  uint debug;
  uint padding[3];
};

#endif // _PARTICLE_