#pragma once

#include <Althea/Allocator.h>
#include <Althea/CameraController.h>
#include <Althea/ComputePipeline.h>
#include <Althea/DescriptorSet.h>
#include <Althea/IGameInstance.h>
#include <Althea/Image.h>
#include <Althea/ImageView.h>
#include <Althea/IndexBuffer.h>
#include <Althea/Model.h>
#include <Althea/PerFrameResources.h>
#include <Althea/RenderPass.h>
#include <Althea/Sampler.h>
#include <Althea/TransientUniforms.h>
#include <Althea/Texture.h>
#include <Althea/ImageBasedLighting.h>
#include <Althea/ImageResource.h>
#include <Althea/VertexBuffer.h>
#include <Althea/RenderTarget.h>
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
  glm::vec3 lightDir;
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
  glm::vec3 _lightDir = glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f));
  bool _adjustingLight = false;

  // TODO: why are these shared ptrs?
  std::shared_ptr<PerFrameResources> _pGlobalResources;
  std::shared_ptr<PerFrameResources> _pRenderTargetResources;
  
  std::unique_ptr<TransientUniforms<GlobalUniforms>> _pGlobalUniforms;

  std::unique_ptr<DescriptorSetAllocator> _pGltfMaterialAllocator;

  std::unique_ptr<CameraController> _pCameraController;

  std::unique_ptr<RenderPass> _pRenderPass;
  std::unique_ptr<RenderPass> _pSceneCaptureRenderPass;

  std::vector<Model> _models;
  
  IBLResources _iblResources;

  RenderTarget _target;

  struct Sphere {
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;

    Sphere();
  };

  Sphere _sphere{};
  VertexBuffer<glm::vec3> _sphereVertexBuffer;
  IndexBuffer _sphereIndexBuffer;

  void _createRenderTarget(const Application& app, VkCommandBuffer commandBuffer);
  void _drawProbe(const glm::mat4& transform, const DrawContext& context);
};
} // namespace DemoScene
} // namespace AltheaDemo
