#ifndef _PARTICLECOLLISION_
#define _PARTICLECOLLISION_

void checkParticleCollision(inout vec3 deltaPos, inout uint collidingParticlesCount, vec3 particlePos, vec3 otherParticlePos)
{
  // TODO: Should use nextPos or prevPos?
  vec3 diff = otherParticlePos - particlePos;
  float dist = length(diff);
  float sep = dist - 2.0 * particleRadius;

  if (sep <= 0.0) {
    // particle.debug = 1; // mark collision
    if (dist < 0.00001)
      diff = vec3(1.0, 0.0, 0.0);
    else 
      diff /= dist;

    float bias = 0.5;
    float k = 1.0 / float(jacobiIters);

    deltaPos += k * bias * sep * diff;
    collidingParticlesCount++;
  }
}

#endif // _PARTICLECOLLISION_