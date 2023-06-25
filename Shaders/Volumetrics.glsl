
bool intersectSphere(vec3 origin, vec3 direction, vec3 center, float radius, float distLimit, out float t0, out float t1) {
  vec3 co = origin - center;

  // Solve quadratic equation (with a = 1)
  float b = 2.0 * dot(direction, co);
  float c = dot(co, co) - radius * radius;

  float b2_4ac = b * b - 4.0 * c;
  if (b2_4ac < 0.0) {
    // No real roots
    return false;
  }

  float sqrt_b2_4ac = sqrt(b2_4ac);
  t0 = max(0.5 * (-b - sqrt_b2_4ac), 0.0);
  t1 = min(0.5 * (-b + sqrt_b2_4ac), distLimit);

  if (t1 <= 0.0 || t0 >= distLimit) {
    // The entire sphere is behind the camera or occluded by the
    // depth buffer.
    return false;
  }

  return true;
}

float sampleVolumeDensity(vec3 curPos) {
  return 0.1;
}

#define VOLUME_RAYMARCH_STEPS 24
#define VOLUME_LIGHT_RAYMARCH_STEPS 8
vec4 raymarchVolume(vec3 cameraPos, vec4 worldPos, vec3 direction) {
  vec4 color = vec4(1.0, 1.0, 1.0, 0.0);

  float throughput = 1.0;

  float t0;
  float t1;
  if (!intersectSphere(
        cameraPos, 
        direction, 
        vec3(0.0), 
        1000.0, 
        worldPos.a == 0.0 ? 10000000.0 : dot(worldPos.xyz - cameraPos, direction), 
        t0, 
        t1)) {
    return color;
  }

  const float mu = 0.1;
  float dt = (t1 - t0) / float(VOLUME_RAYMARCH_STEPS);
  for (int rayStep = 0; rayStep < VOLUME_RAYMARCH_STEPS; ++rayStep) {
    vec3 curPos = cameraPos + (t0 + rayStep * dt) * direction;
    float density = sampleVolumeDensity(curPos);
    throughput *= exp(-mu * density * dt);
  }

  color.a = 1.0 - throughput;

  return color;
}