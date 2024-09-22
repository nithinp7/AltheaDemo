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
#include <Althea/AccelerationStructure.h>
#include <Althea/BufferHeap.h>
#include <Althea/RayTracedReflection.h>
#include <glm/glm.hpp>

#include <vector>

using namespace AltheaEngine;

namespace AltheaEngine {
class Application;
} // namespace AltheaEngine

namespace AltheaDemo {
namespace RayTracedReflectionsDemo {

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

class RayTracedReflectionsDemo : public IGameInstance {
public:
  RayTracedReflectionsDemo();
  // virtual ~RayTracedReflectionsDemo();

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
  IBLResources _iblResources;
  GBufferResources _gBufferResources;

  void _createModels(Application& app, SingleTimeCommandBuffer& commandBuffer);
  std::vector<Model> _models;

  void _createForwardPass(Application& app);
  StructuredBuffer<PrimitiveConstants> _primitiveConstantsBuffer; 
  BufferHeap _vertexBufferHeap;
  BufferHeap _indexBufferHeap;
  AccelerationStructure _accelerationStructure;
  std::unique_ptr<RenderPass> _pForwardPass;
  FrameBuffer _forwardFrameBuffer;

  void _createDeferredPass(Application& app);
  std::unique_ptr<RenderPass> _pDeferredPass;
  SwapChainFrameBufferCollection _swapChainFrameBuffers;
  std::unique_ptr<DescriptorSetAllocator> _pDeferredMaterialAllocator;

  ShaderDefines _shaderDefs;
  std::unique_ptr<RayTracedReflection> _pRTR;
  float _exposure = 0.3f;
};
} // namespace RayTracedReflectionsDemo
} // namespace AltheaDemo
