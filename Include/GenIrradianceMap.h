#pragma once

#include <Althea/CameraController.h>
#include <Althea/ComputePipeline.h>
#include <Althea/IGameInstance.h>
#include <Althea/Image.h>
#include <Althea/PerFrameResources.h>
#include <Althea/RenderPass.h>
#include <Althea/Sampler.h>
#include <Althea/TransientUniforms.h>
#include <glm/glm.hpp>

#include <memory>

using namespace AltheaEngine;

namespace AltheaEngine {
class Application;
} // namespace AltheaEngine

namespace AltheaDemo {
namespace GenIrradianceMap {

struct GenIrradiancePass {
  std::unique_ptr<DescriptorSetAllocator> materialAllocator{};
  std::unique_ptr<DescriptorSet> material{};

  ComputePipeline pipeline{};
};

struct EnvironmentMap {
  Image image;
  ImageView view;
  Sampler sampler;
};

struct IrradianceMap {
  // Output of the compute pass and input to render pass.
  Image image;
  // View and sampler for rendering the irradiance map texture.
  ImageView view;
  Sampler sampler;
};

struct CameraUniforms {
  glm::mat4 projection;
  glm::mat4 inverseProjection;
  glm::mat4 view;
  glm::mat4 inverseView;
};

struct DisplayResultPass {
  std::unique_ptr<PerFrameResources> pGlobalResources;
  std::unique_ptr<TransientUniforms<CameraUniforms>> pGlobalUniforms;
  std::unique_ptr<RenderPass> pRenderPass{};
};

class GenIrradianceMap : public IGameInstance {
public:
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

  GenIrradiancePass _computePass;
  DisplayResultPass _renderPass;
  EnvironmentMap _environmentMap;
  IrradianceMap _irradianceMap;

  bool _generateIrradianceMap = true;
};
} // namespace GenIrradianceMap
} // namespace AltheaDemo