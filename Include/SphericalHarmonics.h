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
#include <Althea/TextureHeap.h>
#include <Althea/GlobalHeap.h>
#include <Althea/GlobalUniforms.h>
#include <Althea/GlobalResources.h>
#include <Althea/ImageBasedLighting.h>
#include <glm/glm.hpp>

#include <vector>

using namespace AltheaEngine;

namespace AltheaEngine {
class Application;
} // namespace AltheaEngine

namespace AltheaDemo {
namespace SphericalHarmonics {

class SphericalHarmonics : public IGameInstance {
public:
  SphericalHarmonics();
  // virtual ~SphericalHarmonics();

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

  struct SHCoeffs {
    float w[4];
  };

  void _createGlobalResources(
      Application& app,
      SingleTimeCommandBuffer& commandBuffer);
  GlobalHeap _globalHeap;
  GlobalUniformsResource _globalUniforms;
  IBLResources _ibl;
  StructuredBuffer<SHCoeffs> _shCoeffs;  

  void _createComputePass(Application& app);
  ComputePipeline _shPass;

  void _createRenderPass(Application& app);
  RenderPass _renderPass;
  SwapChainFrameBufferCollection _swapChainFrameBuffers;

  float _exposure = 0.3f;
};
} // namespace BindlessDemo
} // namespace AltheaDemo
