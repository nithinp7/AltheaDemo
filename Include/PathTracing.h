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
namespace PathTracing {

// TODO: move this into engine
struct GlobalUniforms {
  glm::mat4 projection;
  glm::mat4 inverseProjection;
  glm::mat4 view;
  glm::mat4 prevView;
  glm::mat4 inverseView;
  glm::mat4 prevInverseView;
  int lightCount;
  float time;
  float exposure;
};

struct GlobalIlluminationUniforms {
  uint32_t spatialHashSize;
  uint32_t spatialHashSlotsPerBuffer;
  uint32_t probeCount;
  uint32_t probesPerBuffer;

  float gridCellSize;
  float padding1;
  float padding2;
  float padding3;
};

struct FreeList {
  uint32_t counter;
};

// 32 bytes
struct ProbeSlot {
  glm::vec4 irradiance;
  int gridX;
  int gridY;
  int gridZ;
  int dbg;
};

// 128 bytes
// fits perfectly into cache line (hopefully)
struct Probe {
  ProbeSlot slots[4];
};

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
  bool _adjustingExposure = false;

  std::unique_ptr<CameraController> _pCameraController;

  void _createGlobalResources(
      Application& app,
      SingleTimeCommandBuffer& commandBuffer);
  PerFrameResources _globalResources;
  TransientUniforms<GlobalUniforms> _globalUniforms;
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
  std::unique_ptr<Material> _pRayTracingMaterial[2];
  std::unique_ptr<RayTracingPipeline> _pRayTracingPipeline;
   // ping-pong buffers
  ImageResource _rayTracingTarget[2];
  ImageResource _depthBuffer[2];
  ImageResource _debugTarget;

  ComputePipeline _probePass;

  AccelerationStructure _accelerationStructure;
  TransientUniforms<GlobalIlluminationUniforms> _giUniforms;
  StructuredBufferHeap<Probe> _probes;
  StructuredBufferHeap<uint32_t> _spatialHash;
  StructuredBuffer<FreeList> _freeList;

  std::unique_ptr<DescriptorSetAllocator> _pDisplayPassMaterialAllocator;
  std::unique_ptr<Material> _pDisplayPassMaterial[2];
  std::unique_ptr<RenderPass> _pDisplayPass;
  SwapChainFrameBufferCollection _displayPassSwapChainFrameBuffers;

  bool _freezeCamera = true;
  uint32_t _framesSinceCameraMoved = 0;
  uint32_t _targetIndex = 0;

  float _exposure = 0.6f;
};
} // namespace PathTracing
} // namespace AltheaDemo
