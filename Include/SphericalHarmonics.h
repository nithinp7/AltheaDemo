#pragma once

#include <Althea/Allocator.h>
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

#include <vector>

using namespace AltheaEngine;

namespace AltheaEngine {
class Application;
} // namespace AltheaEngine

namespace AltheaDemo {
namespace SphericalHarmonics {

struct CoeffSet {
  float coeffs[16]{};
};

struct SHUniforms {
  float coeffs[16]{};
  uint32_t graphHandle{};
  int displayMode = 2;
  uint32_t padding2{};
  uint32_t padding3{};
};

struct LegendreUniforms {
  glm::vec2 samples[10]{};
  uint32_t sampleCount{};
  uint32_t coeffBuffer{};
};

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
  std::unique_ptr<CameraController> _pCameraController;

  void _createGlobalResources(
      Application& app,
      SingleTimeCommandBuffer& commandBuffer);
  GlobalHeap _globalHeap;
  GlobalUniformsResource _globalUniforms;
  IBLResources _ibl;
  StructuredBuffer<CoeffSet> _shCoeffs;
  StructuredBuffer<CoeffSet> _legendreCoeffs;
  TransientUniforms<SHUniforms> _shUniforms;
  TransientUniforms<LegendreUniforms> _legendreUniforms;

  void _createGraph(Application& app);
  RenderPass _graphPass;
  FrameBuffer _graphFrameBuffer;
  ImageResource _graph;
  TextureHandle _graphHandle;

  void _createComputePass(Application& app);
  ComputePipeline _fitLegendre;
  ComputePipeline _shPass;

  void _createRenderPass(Application& app);
  RenderPass _renderPass;
  SwapChainFrameBufferCollection _swapChainFrameBuffers;

  SHUniforms _shUniformValues;
  LegendreUniforms _legendreUniformValues;
  float _exposure = 0.3f;
};
} // namespace SphericalHarmonics
} // namespace AltheaDemo
