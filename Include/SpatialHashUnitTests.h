#pragma once

#include <cstdint>
#include <vector>

namespace AltheaDemo {
namespace ParticleSystem {

struct SimUniforms;
struct Particle;

class SpatialHashUnitTests {
public:
  static void runTests(
      const SimUniforms& simUniforms,
      const std::vector<Particle>& particles,
      const std::vector<uint32_t>& spatialHash);
};

} // namespace ParticleSystem
} // namespace AltheaDemo