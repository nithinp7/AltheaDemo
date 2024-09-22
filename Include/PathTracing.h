#pragma once

#include <Althea/Allocator.h>
#include <Althea/AccelerationStructure.h>
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
#include <Althea/RayTracingPipeline.h>
#include <Althea/RenderPass.h>
#include <Althea/Sampler.h>
#include <Althea/ScreenSpaceReflection.h>
#include <Althea/Texture.h>
#include <Althea/TransientUniforms.h>
#include <Althea/UniformBuffer.h>
#include <Althea/ShaderBindingTable.h>
#include <Althea/StructuredBuffer.h>
#include <Althea/Primitive.h>
#include <Althea/BufferHeap.h>
#include <Althea/GlobalHeap.h>
#include <Althea/GlobalResources.h>
#include <Althea/GlobalUniforms.h>
#include <Althea/Common/GlobalIllumination.h>
#include <glm/glm.hpp>

#include <vector>

using namespace AltheaEngine;

namespace AltheaEngine {
class Application;
} // namespace AltheaEngine

namespace AltheaDemo {
namespace PathTracing {

class PathTracing : public IGameInstance {
public:
  PathTracing();
  // virtual ~PathTracing();

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

  void createModels(Application& app, SingleTimeCommandBuffer& commandBuffer);
  std::vector<Model> m_models;

  void createGlobalResources(
      Application& app,
      SingleTimeCommandBuffer& commandBuffer);
  GlobalHeap m_heap;
  GlobalResources m_globalResources;
  GlobalUniformsResource m_globalUniforms;
  PointLightCollection m_pointLights;
  AccelerationStructure m_accelerationStructure;

  void createGBufferPass(Application& app, SingleTimeCommandBuffer& commandBuffer);
  RenderPass m_gBufferPass;
  FrameBuffer m_gBufferFrameBufferA;
  FrameBuffer m_gBufferFrameBufferB;
  
  void createSamplingPasses(Application& app, SingleTimeCommandBuffer& commandBuffer);
  RayTracingPipeline m_directSamplingPass;
  RayTracingPipeline m_spatialResamplingPass;

  void reservoirBarrier(VkCommandBuffer commandBuffer);

   // ping-pong buffers
  struct RtTarget {
    ImageResource target{};
    ImageHandle targetImageHandle{};
    TextureHandle targetTextureHandle{};
  };
  RtTarget m_rtTarget;
  
  TransientUniforms<GlobalIllumination::Uniforms> m_giUniforms;
  std::vector<StructuredBuffer<GlobalIllumination::Reservoir>> m_reservoirHeap;

  RenderPass m_displayPass;
  SwapChainFrameBufferCollection m_displayPassSwapChainFrameBuffers;

  bool m_freezeCamera = true;
  uint32_t m_frameNumber = 0;
  uint32_t m_targetIndex = 0;

  float m_exposure = 0.6f;
};
} // namespace PathTracing
} // namespace AltheaDemo
