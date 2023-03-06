#pragma once

#include <Althea/CameraController.h>
#include <Althea/ComputePipeline.h>
#include <Althea/IGameInstance.h>
#include <Althea/Image.h>
#include <Althea/PerFrameResources.h>
#include <Althea/RenderPass.h>
#include <Althea/Sampler.h>
#include <Althea/TransientUniforms.h>
#include <Althea/ImageResource.h>
#include <glm/glm.hpp>

#include <memory>

using namespace AltheaEngine;

namespace AltheaEngine {
class Application;
} // namespace AltheaEngine

namespace AltheaDemo {
namespace IBLPrecompute {

struct GenIrradiancePass {
  std::unique_ptr<DescriptorSetAllocator> materialAllocator{};
  std::unique_ptr<DescriptorSet> material{};

  ComputePipeline pipeline{};
};

struct PrefilterEnvMapPasses {
  std::unique_ptr<DescriptorSetAllocator> materialAllocator{};
  std::vector<std::unique_ptr<DescriptorSet>> materials;

  ComputePipeline pipeline{};
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

class IBLPrecompute : public IGameInstance {
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
  std::string _envMapName;

  std::unique_ptr<CameraController> _pCameraController;

  GenIrradiancePass _genIrradiancePass;
  PrefilterEnvMapPasses _prefilterEnvMapPasses;
  DisplayResultPass _renderPass;
  AltheaEngine::ImageResource _environmentMap;
  std::vector<AltheaEngine::ImageResource> _preFilteredMap;
  AltheaEngine::ImageResource _irradianceMap;

  bool _recomputeMaps = true;
};
} // namespace GenIrradianceMap
} // namespace AltheaDemo