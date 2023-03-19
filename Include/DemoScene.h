#pragma once

#include <Althea/Allocator.h>
#include <Althea/CameraController.h>
#include <Althea/ComputePipeline.h>
#include <Althea/DescriptorSet.h>
#include <Althea/IGameInstance.h>
#include <Althea/Image.h>
#include <Althea/ImageBasedLighting.h>
#include <Althea/ImageResource.h>
#include <Althea/ImageView.h>
#include <Althea/IndexBuffer.h>
#include <Althea/Model.h>
#include <Althea/PerFrameResources.h>
#include <Althea/RenderPass.h>
#include <Althea/RenderTarget.h>
#include <Althea/Sampler.h>
#include <Althea/Texture.h>
#include <Althea/TransientUniforms.h>
#include <Althea/VertexBuffer.h>
#include <Althea/FrameBuffer.h>
#include <Althea/Material.h>
#include <glm/glm.hpp>

#include <cstdint>
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
  glm::vec3 lightDir;
  float time;
  float exposure;
};

struct GlobalUniformsCubeRender {
  glm::mat4 projection;
  glm::mat4 inverseProjection;
  glm::mat4 views[6];
  glm::mat4 inverseViews[6];
  glm::vec3 lightDir;
  float time;
  float exposure;
};

struct ProbePushConstants {
  glm::mat4 model{};
  uint32_t sceneCaptureIndex{};
};

struct LightProbe {
  std::unique_ptr<Material> pMaterial{};
  std::unique_ptr<TransientUniforms<GlobalUniformsCubeRender>> pUniforms{};
  FrameBuffer frameBuffer{};
  glm::vec3 location{};
};

struct ProbeCollection {
  std::vector<LightProbe> probes;
  std::unique_ptr<DescriptorSetAllocator> pMaterialAllocator{};
  std::unique_ptr<RenderPass> pRenderPass{};
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
  glm::vec3 _lightDir = glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f));
  bool _adjustingLight = false;

  // TODO: why are these shared ptrs?
  std::shared_ptr<PerFrameResources> _pGlobalResources;
  std::shared_ptr<PerFrameResources> _pRenderTargetTextures;

  std::unique_ptr<TransientUniforms<GlobalUniforms>> _pGlobalUniforms;

  std::unique_ptr<DescriptorSetAllocator> _pGltfMaterialAllocator;

  std::unique_ptr<CameraController> _pCameraController;

  std::unique_ptr<RenderPass> _pRenderPass;
  SwapChainFrameBufferCollection _swapChainFrameBuffers;

  RenderTargetCollection _renderTargets{};
  ProbeCollection _probeCollection{};

  std::vector<Model> _models;

  IBLResources _iblResources;

  struct Sphere {
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;

    Sphere();
  };

  Sphere _sphere{};
  VertexBuffer<glm::vec3> _sphereVertexBuffer;
  IndexBuffer _sphereIndexBuffer;

  glm::vec3 _probeTranslation{};

  void _createProbes(
      const Application& app,
      SingleTimeCommandBuffer& commandBuffer,
      uint32_t count);
  void _drawProbe(
      const glm::mat4& transform,
      uint32_t sceneCaptureIndex,
      const DrawContext& context);
};
} // namespace DemoScene
} // namespace AltheaDemo
