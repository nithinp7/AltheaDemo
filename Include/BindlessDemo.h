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
#include <Althea/GlobalHeap.h>
#include <Althea/GlobalUniforms.h>
#include <Althea/GlobalResources.h>
#include <glm/glm.hpp>

#include <vector>

using namespace AltheaEngine;

namespace AltheaEngine {
class Application;
} // namespace AltheaEngine

namespace AltheaDemo {
namespace BindlessDemo {

class BindlessDemo : public IGameInstance {
public:
  BindlessDemo();
  // virtual ~BindlessDemo();

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
  GlobalHeap _globalHeap;
  GlobalUniformsResource _globalUniforms;
  GlobalResources _globalResources;
  PointLightCollection _pointLights;

  void _createModels(Application& app, SingleTimeCommandBuffer& commandBuffer);
  std::vector<Model> _models;

  void _createForwardPass(Application& app);
  std::unique_ptr<RenderPass> _pForwardPass;
  FrameBuffer _forwardFrameBuffer;

  void _createDeferredPass(Application& app);
  std::unique_ptr<RenderPass> _pDeferredPass;
  SwapChainFrameBufferCollection _swapChainFrameBuffers;

  ScreenSpaceReflection _SSR;
  float _exposure = 0.3f;
};
} // namespace BindlessDemo
} // namespace AltheaDemo
