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
#include <Althea/Texture.h>
#include <Althea/TransientUniforms.h>
#include <glm/glm.hpp>

#include <vector>

using namespace AltheaEngine;

namespace AltheaEngine {
class Application;
} // namespace AltheaEngine

namespace AltheaDemo {
namespace DemoScene {

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

class DemoScene : public IGameInstance {
public:
  DemoScene();
  // virtual ~DemoScene();

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

  void _createModels(Application& app, SingleTimeCommandBuffer& commandBuffer);
  std::vector<Model> _models;

  void _createForwardPass(Application& app);
  std::unique_ptr<RenderPass> _pForwardPass;
  FrameBuffer _forwardFrameBuffer;

  void _createDeferredPass(Application& app);
  std::unique_ptr<RenderPass> _pDeferredPass;
  SwapChainFrameBufferCollection _swapChainFrameBuffers;
  std::unique_ptr<DescriptorSetAllocator> _pDeferredMaterialAllocator;
  // std::unique_ptr<Material> _pDeferredMaterial;

  std::unique_ptr<ScreenSpaceReflection> _pSSR;
  float _exposure = 0.3f;
};
} // namespace DemoScene
} // namespace AltheaDemo
