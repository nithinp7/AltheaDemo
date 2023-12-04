#pragma once

#include <Althea/Allocator.h>
#include <Althea/CameraController.h>
#include <Althea/ComputePipeline.h>
#include <Althea/DeferredRendering.h>
#include <Althea/DescriptorSet.h>
#include <Althea/FrameBuffer.h>
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
#include <Althea/BufferHeap.h>
#include <Althea/Texture.h>
#include <Althea/TransientUniforms.h>
#include <glm/glm.hpp>

#include <vector>
#include <cstdint>

using namespace AltheaEngine;

namespace AltheaEngine {
class Application;
} // namespace AltheaEngine

namespace AltheaDemo {
namespace ParticleSystem {

// TODO: move this into engine
struct GlobalUniforms {
  glm::mat4 projection;
  glm::mat4 inverseProjection;
  glm::mat4 view;
  glm::mat4 inverseView;
  int lightCount;
  float time;
  float exposure;
};

struct Particle {
  alignas(16) glm::vec3 position;
  alignas(4) uint32_t nextParticleLink;
  alignas(4) uint32_t debug;
};

// TODO: Determine alignment / padding
struct SimUniforms {
  // Uniform grid params
  glm::mat4 gridToWorld;
  glm::mat4 worldToGrid;
  
  glm::mat4 inverseView;

  glm::vec3 interactionLocation;
  uint32_t inputMask;

  uint32_t particleCount;
  uint32_t particlesPerBuffer;
  uint32_t spatialHashSize;
  uint32_t spatialHashEntriesPerBuffer;

  uint32_t jacobiIters;
  float deltaTime;
  float particleRadius;
  float time;

  uint32_t addedParticles;
  uint32_t padding[3];
};

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
  bool _adjustingExposure = false;

  std::unique_ptr<CameraController> _pCameraController;

  void _resetParticles(const Application& app, VkCommandBuffer commandBuffer);
  
  void _createGlobalResources(
      Application& app,
      SingleTimeCommandBuffer& commandBuffer);
  std::unique_ptr<PerFrameResources> _pGlobalResources;
  std::unique_ptr<TransientUniforms<GlobalUniforms>> _pGlobalUniforms;
  PointLightCollection _pointLights;
  std::unique_ptr<DescriptorSetAllocator> _pGltfMaterialAllocator;
  IBLResources _iblResources;
  GBufferResources _gBufferResources;

  void _createSimResources(Application& app, SingleTimeCommandBuffer& commandBuffer);
  std::unique_ptr<PerFrameResources> _pSimResources;
  ComputePipeline _simPass; // TODO: RENAME to spatialHashRegistration?
  ComputePipeline _jacobiStep;
  TransientUniforms<SimUniforms> _simUniforms;
  StructuredBufferHeap<Particle> _particleBuffer;
  StructuredBufferHeap<uint32_t> _spatialHash;
  // TODO: These could probably be part of the same heap...
  StructuredBufferHeap<glm::vec4> _positionsA;
  StructuredBufferHeap<glm::vec4> _positionsB;
  VertexBuffer<glm::vec3> _sphereVertices;
  IndexBuffer _sphereIndices;
  
  void _createModels(Application& app, SingleTimeCommandBuffer& commandBuffer);
  std::vector<Model> _models;

  void _createForwardPass(Application& app);
  std::unique_ptr<RenderPass> _pForwardPass;
  FrameBuffer _forwardFrameBuffer;

  void _createDeferredPass(Application& app);
  std::unique_ptr<RenderPass> _pDeferredPass;
  SwapChainFrameBufferCollection _swapChainFrameBuffers;
  std::unique_ptr<DescriptorSetAllocator> _pDeferredMaterialAllocator;
  std::unique_ptr<Material> _pDeferredMaterial;

  std::unique_ptr<ScreenSpaceReflection> _pSSR;
  float _exposure = 0.3f;

  uint32_t _activeParticleCount = 10000;
  uint32_t _inputMask = 0;
};
} // namespace ParticleSystem
} // namespace AltheaDemo
