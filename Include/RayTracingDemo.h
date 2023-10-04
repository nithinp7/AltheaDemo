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
#include <Althea/TextureHeap.h>
#include <Althea/StructuredBuffer.h>
#include <Althea/Primitive.h>
#include <Althea/BufferHeap.h>
#include <glm/glm.hpp>

#include <vector>

using namespace AltheaEngine;

namespace AltheaEngine {
class Application;
} // namespace AltheaEngine

namespace AltheaDemo {
namespace RayTracingDemo {

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

class RayTracingDemo : public IGameInstance {
public:
  RayTracingDemo();
  // virtual ~RayTracingDemo();

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
  StructuredBuffer<PrimitiveConstants> _primitiveConstantsBuffer; 
  TextureHeap _textureHeap;
  BufferHeap _vertexBufferHeap;
  BufferHeap _indexBufferHeap;

  void _createModels(Application& app, SingleTimeCommandBuffer& commandBuffer);
  std::vector<Model> _models;

  void _createRayTracingPass(Application& app, SingleTimeCommandBuffer& commandBuffer);
  std::unique_ptr<DescriptorSetAllocator> _pRayTracingMaterialAllocator;
  std::unique_ptr<Material> _pRayTracingMaterial;
  std::unique_ptr<RayTracingPipeline> _pRayTracingPipeline;
  ImageResource _rayTracingTarget;
  AccelerationStructure _accelerationStructure;
  std::unique_ptr<DescriptorSetAllocator> _pDisplayPassMaterialAllocator;
  std::unique_ptr<Material> _pDisplayPassMaterial;
  std::unique_ptr<RenderPass> _pDisplayPass;
  SwapChainFrameBufferCollection _displayPassSwapChainFrameBuffers;

  float _exposure = 0.3f;
};
} // namespace RayTracingDemo
} // namespace AltheaDemo
