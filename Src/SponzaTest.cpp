#include "SponzaTest.h"

#include <Althea/Application.h>
#include <Althea/Camera.h>
#include <Althea/Cubemap.h>
#include <Althea/DescriptorSet.h>
#include <Althea/GraphicsPipeline.h>
#include <Althea/InputManager.h>
#include <Althea/ModelViewProjection.h>
#include <Althea/Primitive.h>
#include <Althea/SingleTimeCommandBuffer.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace AltheaEngine;

namespace AltheaDemo {
SponzaTest::SponzaTest() {}

void SponzaTest::initGame(Application& app) {
  const VkExtent2D& windowDims = app.getSwapChainExtent();
  this->_pCameraController = std::make_unique<CameraController>(
      app.getInputManager(),
      90.0f,
      (float)windowDims.width / (float)windowDims.height);

  // TODO: need to unbind these at shutdown
  InputManager& input = app.getInputManager();
  input.addKeyBinding(
      {GLFW_KEY_L, GLFW_PRESS, 0},
      [&adjustingLight = this->_adjustingLight, &input]() {
        adjustingLight = true;
        input.setMouseCursorHidden(false);
      });

  input.addKeyBinding(
      {GLFW_KEY_L, GLFW_RELEASE, 0},
      [&adjustingLight = this->_adjustingLight, &input]() {
        adjustingLight = false;
        input.setMouseCursorHidden(true);
      });

  // Recreate any stale pipelines (shader hot-reload)
  input.addKeyBinding(
      {GLFW_KEY_R, GLFW_PRESS, GLFW_MOD_CONTROL},
      [&app, that = this]() {
        for (Subpass& subpass : that->_pRenderPass->getSubpasses()) {
          GraphicsPipeline& pipeline = subpass.getPipeline();
          if (pipeline.recompileStaleShaders()) {
            if (pipeline.hasShaderRecompileErrors()) {
              std::cout << pipeline.getShaderRecompileErrors() << "\n";
            } else {
              pipeline.recreatePipeline(app);
            }
          }
        }
      });

  input.addMousePositionCallback(
      [&adjustingLight = this->_adjustingLight,
       &lightDir = this->_lightDir](double x, double y, bool cursorHidden) {
        if (adjustingLight) {
          // TODO: consider current camera forward direction.
          float theta = glm::pi<float>() * static_cast<float>(x);
          float height = static_cast<float>(y) + 1.0f;

          lightDir = glm::normalize(glm::vec3(cos(theta), height, sin(theta)));
        }
      });
}

void SponzaTest::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void SponzaTest::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  // TODO: Default color and depth-stencil clear values for attachments?
  VkClearValue colorClear;
  colorClear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkClearValue depthClear;
  depthClear.depthStencil = {1.0f, 0};

  std::vector<Attachment> attachments = {
      {AttachmentType::Color,
       app.getSwapChainImageFormat(),
       colorClear,
       std::nullopt,
       false},
      {AttachmentType::Depth,
       app.getDepthImageFormat(),
       depthClear,
       app.getDepthImageView(),
       true}};

  // Global resources
  DescriptorSetLayoutBuilder globalResourceLayout;

  globalResourceLayout
      // Add slot for skybox cubemap.
      .addTextureBinding()
      // Global uniforms.
      .addUniformBufferBinding();

  SingleTimeCommandBuffer commandBuffer(app);

  this->_pGlobalResources =
      std::make_shared<PerFrameResources>(app, globalResourceLayout);
  this->_pGlobalUniforms =
      std::make_unique<TransientUniforms<GlobalUniforms>>(app, commandBuffer);

  std::vector<SubpassBuilder> subpassBuilders;

  // SKYBOX PASS
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments.push_back(0);
    Skybox::buildPipeline(app, subpassBuilder.pipelineBuilder);

    subpassBuilder.pipelineBuilder
        .layoutBuilder
        // Global resources (view, projection, skybox)
        .addDescriptorSet(this->_pGlobalResources->getLayout());
  }

  // REGULAR PASS
  {
    // Per-primitive material resources
    DescriptorSetLayoutBuilder primitiveMaterialLayout;
    Primitive::buildMaterial(primitiveMaterialLayout);

    this->_pGltfMaterialAllocator =
        std::make_unique<DescriptorSetAllocator>(app, primitiveMaterialLayout);

    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments.push_back(0);
    subpassBuilder.depthAttachment = 1;

    Primitive::buildPipeline(subpassBuilder.pipelineBuilder);

    subpassBuilder
        .pipelineBuilder
        // Vertex shader
        .addVertexShader(GEngineDirectory + "/Shaders/GltfPBR.vert")
        // Fragment shader
        .addFragmentShader(GEngineDirectory + "/Shaders/GltfPBR.frag")

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_pGlobalResources->getLayout())
        // Material (per-object) resources (diffuse, normal map,
        // metallic-roughness, etc)
        .addDescriptorSet(this->_pGltfMaterialAllocator->getLayout());
  }

  this->_pRenderPass = std::make_unique<RenderPass>(
      app,
      std::move(attachments),
      std::move(subpassBuilders));

  std::vector<Subpass>& subpasses = this->_pRenderPass->getSubpasses();

  const static std::array<std::string, 6> skyboxImagePaths = {
      GEngineDirectory + "/Content/Models/Skybox/right.jpg",
      GEngineDirectory + "/Content/Models/Skybox/left.jpg",
      GEngineDirectory + "/Content/Models/Skybox/top.jpg",
      GEngineDirectory + "/Content/Models/Skybox/bottom.jpg",
      GEngineDirectory + "/Content/Models/Skybox/front.jpg",
      GEngineDirectory + "/Content/Models/Skybox/back.jpg"};

  this->_pSkybox =
      std::make_unique<Skybox>(app, commandBuffer, skyboxImagePaths, true);

  this->_pSponzaModel = std::make_unique<Model>(
      app,
      commandBuffer,
      // "/Content/Models/Sponza/glTF/Sponza.gltf",
      GEngineDirectory + "/Content/Models/FlightHelmet/FlightHelmet.gltf",
      *this->_pGltfMaterialAllocator);

  // Bind the skybox cubemap as a global resource
  const std::shared_ptr<Cubemap>& pCubemap = this->_pSkybox->getCubemap();
  this->_pGlobalResources->assign()
      .bindTexture(pCubemap->getImageView(), pCubemap->getSampler())
      .bindTransientUniforms(*this->_pGlobalUniforms);
}

void SponzaTest::destroyRenderState(Application& app) {
  this->_pSkybox.reset();
  this->_pSponzaModel.reset();
  this->_pRenderPass.reset();
  this->_pGlobalResources.reset();
  this->_pGlobalUniforms.reset();
  this->_pGltfMaterialAllocator.reset();
}

void SponzaTest::tick(Application& app, const FrameContext& frame) {
  this->_pCameraController->tick(frame.deltaTime);
  const Camera& camera = this->_pCameraController->getCamera();

  const glm::mat4& projection = camera.getProjection();

  GlobalUniforms globalUniforms;
  globalUniforms.projection = camera.getProjection();
  globalUniforms.inverseProjection = glm::inverse(globalUniforms.projection);
  globalUniforms.view = camera.computeView();
  globalUniforms.inverseView = glm::inverse(globalUniforms.view);
  globalUniforms.lightDir = this->_lightDir;
  globalUniforms.time = static_cast<float>(frame.currentTime);

  this->_pGlobalUniforms->updateUniforms(globalUniforms, frame);
}

void SponzaTest::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {

  VkDescriptorSet globalDescriptorSet =
      this->_pGlobalResources->getCurrentDescriptorSet(frame);

  this->_pRenderPass
      ->begin(app, commandBuffer, frame)
      // Bind global descriptor sets
      .setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1))
      // Draw skybox
      .draw(*this->_pSkybox)
      .nextSubpass()
      // Draw Sponza model
      .draw(*this->_pSponzaModel);
}
} // namespace AltheaDemo