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
  glm::vec4 position;
  glm::vec4 velocity;
  uint32_t gridCellHash;
  float radius;
  uint32_t padding[2];
};

// TODO: Determine alignment / padding
struct SimUniforms {
  // Uniform grid params
  glm::mat4 gridToWorld;
  glm::mat4 worldToGrid;
  
  uint32_t xCells;
  uint32_t yCells;
  uint32_t zCells;

  uint32_t particleCount;
  float deltaTime;

  float padding[3];
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
  ComputePipeline _simPass;
  ComputePipeline _velocityPass;
  TransientUniforms<SimUniforms> _simUniforms;
  StructuredBuffer<Particle> _particleBuffer;
  // maps particle idx to cell idx
  StructuredBuffer<uint32_t> _particleToCellBuffer;
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
};
} // namespace ParticleSystem
} // namespace AltheaDemo
