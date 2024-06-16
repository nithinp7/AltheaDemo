#pragma once

#include <Althea/Allocator.h>
#include <Althea/BufferHeap.h>
#include <Althea/CameraController.h>
#include <Althea/ComputePipeline.h>
#include <Althea/DeferredRendering.h>
#include <Althea/DescriptorSet.h>
#include <Althea/FrameBuffer.h>
#include <Althea/GlobalHeap.h>
#include <Althea/GlobalResources.h>
#include <Althea/GlobalUniforms.h>
#include <Althea/IGameInstance.h>
#include <Althea/Image.h>
#include <Althea/ImageBasedLighting.h>
#include <Althea/ImageResource.h>
#include <Althea/ImageView.h>
#include <Althea/Model.h>
#include <Althea/PerFrameResources.h>
#include <Althea/PointLight.h>
#include <Althea/RenderPass.h>
#include <Althea/Sampler.h>
#include <Althea/ScreenSpaceReflection.h>
#include <Althea/StructuredBuffer.h>
#include <Althea/Texture.h>
#include <Althea/TransientUniforms.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

using namespace AltheaEngine;

#define PARTICLES_PER_BUCKET 16

namespace AltheaEngine {
class Application;
} // namespace AltheaEngine

namespace AltheaDemo {
namespace ParticleSystem {

struct LiveValues {
  float slider1;
  float slider2;
  bool checkbox1;
  bool checkbox2;
};

struct PushConstants {
  uint32_t globalResourcesHandle;
  uint32_t globalUniformsHandle;
  uint32_t simUniformsHandle;
  uint32_t iteration;
};

struct Particle {
  alignas(16) glm::vec3 position;
  alignas(4) uint32_t globalIndex;
  alignas(16) glm::vec3 prevPosition;
  alignas(4) uint32_t debug;
};

struct ParticleEntry {
  float positions[8]; // Don't need this to be particularly readable on the CPU
                      // anyways
};

struct ParticleBucket {
  ParticleEntry particles[PARTICLES_PER_BUCKET];
};

// TODO: Determine alignment / padding
struct SimUniforms {
  // Uniform grid params
  glm::mat4 gridToWorld;
  glm::mat4 worldToGrid;

  glm::vec3 interactionLocation;
  uint32_t padding;

  uint32_t particleCount;
  uint32_t particlesPerBuffer;
  uint32_t spatialHashSize;
  uint32_t spatialHashEntriesPerBuffer;

  uint32_t jacobiIters;
  float deltaTime;
  float particleRadius;
  float time;

  uint32_t addedParticles;
  uint32_t particleBucketCount;
  uint32_t particleBucketsPerBuffer;
  uint32_t freeListsCount;

  uint32_t particlesHeap;
  uint32_t spatialHashHeap;
  uint32_t bucketHeap;
  uint32_t nextFreeBucket;

  LiveValues liveValues;
};

#define SIM_PASS 0
#define BUCKET_ALLOC_PASS 1
#define BUCKET_INSERT_PASS 2
#define JACOBI_STEP_PASS 3

class ParticleSystem : public IGameInstance {
public:
  ParticleSystem();
  // virtual ~ParticleSystem();

  void initGame(Application& app) override;
  void shutdownGame(Application& app) override;

  void createRenderState(Application& app) override;
  void destroyRenderState(Application& app) override;

  void tick(Application& app, const FrameContext& frame) override;
  void draw(
      Application& app,
      VkCommandBuffer commandBuffer,
      const FrameContext& frame) override;

private:
  bool m_adjustingExposure = false;

  std::unique_ptr<CameraController> m_pCameraController;

  void _resetParticles(Application& app, VkCommandBuffer commandBuffer);

  void _createGlobalResources(
      Application& app,
      SingleTimeCommandBuffer& commandBuffer);
  GlobalHeap m_heap;
  GlobalResources m_globalResources;
  GlobalUniformsResource m_globalUniforms;

  void
  _createSimResources(Application& app, SingleTimeCommandBuffer& commandBuffer);
  std::vector<ComputePipeline> m_computePasses;

  TransientUniforms<SimUniforms> m_simUniforms;
  PushConstants m_push;

  StructuredBufferHeap<Particle> m_particleBuffer;
  StructuredBufferHeap<uint32_t> m_spatialHash;
  StructuredBuffer<uint32_t> m_freeBucketCounter;
  StructuredBufferHeap<ParticleBucket> m_buckets;

  struct SphereMesh {
    VertexBuffer<glm::vec3> vertices;
    IndexBuffer indices;
  };
  SphereMesh m_sphere;

  void _createModels(Application& app, SingleTimeCommandBuffer& commandBuffer);
  std::vector<Model> m_models;

  void _createGBufferPass(Application& app);
  RenderPass m_gBufferPass;
  FrameBuffer m_gBufferFrameBufferA;
  FrameBuffer m_gBufferFrameBufferB;

  void _createDeferredPass(Application& app);
  RenderPass m_deferredPass;
  SwapChainFrameBufferCollection m_swapChainFrameBuffers;

  void _renderGBufferPass(
      Application& app,
      VkCommandBuffer commandBuffer,
      const FrameContext& frame);

  void _dispatchComputePass(VkCommandBuffer commandBuffer, uint32_t passIdx, uint32_t groupCount);
  void _readAfterWriteBarrier(VkCommandBuffer commandBuffer);
  void _writeAfterReadBarrier(VkCommandBuffer commandBuffer);

  uint32_t m_writeIndex = 0;

  ScreenSpaceReflection m_ssr;
  float m_exposure = 0.3f;

  bool m_flagReset = false;
  uint32_t m_activeParticleCount = 100000; // 0;
};
} // namespace ParticleSystem
} // namespace AltheaDemo
