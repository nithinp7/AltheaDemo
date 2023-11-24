#include "SpatialHashUnitTests.h"
#include "ParticleSystem.h"

#include <glm/glm.hpp>

namespace AltheaDemo {
namespace ParticleSystem {

#define CELL_HASH_MASK 0xFFFF0000
#define PARTICLE_IDX_MASK 0x0000FFFF

static uint32_t hashCoords(int x, int y, int z) {
  return glm::abs((x * 92837111) ^ (y * 689287499) ^ (z * 283923481));
}

static uint32_t findSlot(
    const SimUniforms& simUniforms,
    const std::vector<uint32_t>& spatialHash,
    const std::vector<Particle>& particles,
    uint32_t particleIdx) {
  const Particle& particle = particles[particleIdx];

  glm::vec4 worldPos = particle.position;
  worldPos.w = 1.0f;
  glm::vec3 gridPos(simUniforms.worldToGrid * worldPos);
  glm::uvec3 cell(glm::floor(gridPos));
  
  uint32_t gridCellHash = hashCoords(cell.x, cell.y, cell.z);
  uint32_t startSlotIdx = (gridCellHash >> 16) % spatialHash.size();
  uint32_t slotIdx = startSlotIdx;

  bool bFoundFirst = false;
  for (uint32_t i = 0; i < simUniforms.spatialHashProbeSteps; ++i) {
    uint32_t entry = spatialHash[slotIdx];
    if (entry == 0xFFFFFFFF)
    {
      // Failed to find the slot index
      __debugbreak();
      return 0xFFFFFFFF;
    } else if ((entry & PARTICLE_IDX_MASK) == particleIdx) {
      return slotIdx;
    } else if ((entry & CELL_HASH_MASK) == (gridCellHash & CELL_HASH_MASK)) {
      bFoundFirst = true;
      ++slotIdx;
      if (slotIdx == spatialHash.size())
        slotIdx = 0;
    } else /*if (bFoundFirst)*/ {
      // __debugbreak();
      // return 0xFFFFFFFF;
      ++slotIdx;
    }
  }

  __debugbreak();
  return 0xFFFFFFFF;
}

/*static*/
void SpatialHashUnitTests::runTests(
    const SimUniforms& simUniforms,
    const std::vector<Particle>& particles,
    const std::vector<uint32_t>& spatialHash) {

  uint32_t particleCount = static_cast<uint32_t>(particles.size());
  uint32_t spatialHashSize = static_cast<uint32_t>(spatialHash.size());

  // Verfy that the non-empty slots in the spatial hash have ascending keys
  {

    uint32_t prevHash = 0;
    for (uint32_t slotIdx = 0; slotIdx < spatialHashSize; ++slotIdx) {
      uint32_t entry = spatialHash[slotIdx];

      // ignore empty slots
      if (entry == 0xFFFFFFFF)
      {
        // If we hit an empty slot, reset the prevHash tracker
        // Only non-empty regions need to be ascending
        prevHash = 0;
        continue;
      }

      uint32_t cellHash = entry & CELL_HASH_MASK;

      // uint32_t curEntryLocation = (cellHash >> 16) % spatialHashSize;
      // uint32_t prevEntryLocation = (prevHash >> 16) % spatialHashSize;
      // TODO: This fails when rollovers are inserted into the front of the map...
      // if (curEntryLocation < prevEntryLocation)
      // if (cellHash < prevHash)
      //   __debugbreak();

      // Doesn't account for rollovers
      // if (slotIdx < curEntryLocation)
      //   __debugbreak();
      prevHash = cellHash;
    }
  }

  // CPU implementation of spatial hash search
  // Verify that each particle can find itself
  {
    for (uint32_t particleIdx = 0; particleIdx < particleCount; ++particleIdx) {
      findSlot(simUniforms, spatialHash, particles, particleIdx);
    }
  }
}

} // namespace ParticleSystem
} // namespace AltheaDemo