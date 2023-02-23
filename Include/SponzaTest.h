#pragma once

#include <Althea/Allocator.h>
#include <Althea/CameraController.h>
#include <Althea/ComputePipeline.h>
#include <Althea/DescriptorSet.h>
#include <Althea/IGameInstance.h>
#include <Althea/Image.h>
#include <Althea/ImageView.h>
#include <Althea/Model.h>
#include <Althea/PerFrameResources.h>
#include <Althea/RenderPass.h>
#include <Althea/Sampler.h>
#include <Althea/Skybox.h>
#include <Althea/TransientUniforms.h>
#include <glm/glm.hpp>

using namespace AltheaEngine;

namespace AltheaEngine {
class Application;
} // namespace AltheaEngine

namespace AltheaDemo {
struct GlobalUniforms {
  glm::mat4 projection;
  glm::mat4 inverseProjection;
  glm::mat4 view;
  glm::mat4 inverseView;
  glm::vec3 lightDir;
  float time;
};

struct ComputePass {
  Image image;
  ImageView view;
  Sampler sampler;

  PerFrameResources resources;
  ComputePipeline pipeline;
};

class SponzaTest : public IGameInstance {
public:
  SponzaTest();
  // virtual ~SponzaTest();

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
  void _initComputePass(Application& app);

  glm::vec3 _lightDir = glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f));
  bool _adjustingLight = false;

  std::shared_ptr<PerFrameResources> _pGlobalResources;
  std::unique_ptr<TransientUniforms<GlobalUniforms>> _pGlobalUniforms;

  std::unique_ptr<DescriptorSetAllocator> _pGltfMaterialAllocator;

  std::unique_ptr<CameraController> _pCameraController;

  std::unique_ptr<RenderPass> _pRenderPass;

  std::unique_ptr<Skybox> _pSkybox;
  std::unique_ptr<Model> _pSponzaModel;

  std::unique_ptr<ComputePass> _pComputePass;

  std::string _currentShader = "BasicGltf";
  bool _envMap = false;
};
} // namespace AltheaDemo
