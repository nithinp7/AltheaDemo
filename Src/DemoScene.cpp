#include "DemoScene.h"

#include <Althea/Application.h>
#include <Althea/Camera.h>
#include <Althea/Cubemap.h>
#include <Althea/DescriptorSet.h>
#include <Althea/GraphicsPipeline.h>
#include <Althea/InputManager.h>
#include <Althea/ModelViewProjection.h>
#include <Althea/Primitive.h>
#include <Althea/SingleTimeCommandBuffer.h>
#include <Althea/Skybox.h>
#include <Althea/Utilities.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace AltheaEngine;

namespace AltheaDemo {
namespace DemoScene {

DemoScene::DemoScene() {}

void DemoScene::initGame(Application& app) {
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
        for (Subpass& subpass : that->_pForwardPass->getSubpasses()) {
          GraphicsPipeline& pipeline = subpass.getPipeline();
          if (pipeline.recompileStaleShaders()) {
            if (pipeline.hasShaderRecompileErrors()) {
              std::cout << pipeline.getShaderRecompileErrors() << "\n";
            } else {
              pipeline.recreatePipeline(app);
            }
          }
        }

        for (Subpass& subpass : that->_pDeferredPass->getSubpasses()) {
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

void DemoScene::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void DemoScene::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  SingleTimeCommandBuffer commandBuffer(app);
  this->_createGlobalResources(app, commandBuffer);
  this->_createModels(app, commandBuffer);
  this->_createForwardPass(app);
  this->_createDeferredPass(app);
}

void DemoScene::destroyRenderState(Application& app) {
  this->_models.clear();

  this->_pForwardPass.reset();
  this->_gBufferResources = {};
  this->_forwardFrameBuffer = {};

  this->_pDeferredPass.reset();
  this->_swapChainFrameBuffers = {};
  this->_pDeferredMaterial.reset();
  this->_pDeferredMaterialAllocator.reset();

  this->_pGlobalResources.reset();
  this->_pGlobalUniforms.reset();
  this->_pGltfMaterialAllocator.reset();
  this->_iblResources = {};
}

void DemoScene::tick(Application& app, const FrameContext& frame) {
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
  globalUniforms.exposure = 0.3f;

  this->_pGlobalUniforms->updateUniforms(globalUniforms, frame);
}

void DemoScene::_createModels(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {

  // Load models
  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/DamagedHelmet.glb",
      *this->_pGltfMaterialAllocator);
  this->_models.back().setModelTransform(
      glm::translate(glm::mat4(1.0f), glm::vec3(6.0f, 0.0f, 0.0f)));

  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/FlightHelmet/FlightHelmet.gltf",
      *this->_pGltfMaterialAllocator);
  this->_models.back().setModelTransform(glm::scale(
      glm::translate(glm::mat4(1.0f), glm::vec3(8.0f, -1.0f, 0.0f)),
      glm::vec3(4.0f)));

  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/MetalRoughSpheres.glb",
      *this->_pGltfMaterialAllocator);
  this->_models.back().setModelTransform(
      glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f)));
}

void DemoScene::_createGlobalResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  this->_iblResources = ImageBasedLighting::createResources(
      app,
      commandBuffer,
      "NeoclassicalInterior");

  // Per-primitive material resources
  DescriptorSetLayoutBuilder primitiveMaterialLayout;
  Primitive::buildMaterial(primitiveMaterialLayout);

  this->_pGltfMaterialAllocator =
      std::make_unique<DescriptorSetAllocator>(app, primitiveMaterialLayout);

  // Global resources
  DescriptorSetLayoutBuilder globalResourceLayout;

  // Add textures for IBL
  ImageBasedLighting::buildLayout(globalResourceLayout);
  globalResourceLayout
      // Global uniforms.
      .addUniformBufferBinding();

  this->_pGlobalResources =
      std::make_unique<PerFrameResources>(app, globalResourceLayout);
  this->_pGlobalUniforms =
      std::make_unique<TransientUniforms<GlobalUniforms>>(app, commandBuffer);

  ResourcesAssignment assignment = this->_pGlobalResources->assign();

  // Bind IBL resources
  this->_iblResources.bind(assignment);

  // Bind global uniforms
  assignment.bindTransientUniforms(*this->_pGlobalUniforms);
}

void DemoScene::_createForwardPass(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();

  // Create image resources for the GBuffer
  ImageOptions imageOptions{};
  imageOptions.width = extent.width;
  imageOptions.height = extent.height;
  imageOptions.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  imageOptions.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  imageOptions.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

  SamplerOptions samplerOptions{};

  ImageViewOptions viewOptions{};
  viewOptions.type = VK_IMAGE_VIEW_TYPE_2D;
  viewOptions.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  viewOptions.aspectFlags = VK_IMAGE_ASPECT_COLOR_BIT;

  this->_gBufferResources.position.image = Image(app, imageOptions);
  this->_gBufferResources.position.sampler = Sampler(app, samplerOptions);
  this->_gBufferResources.position.view = ImageView(
      app,
      this->_gBufferResources.position.image.getImage(),
      viewOptions);

  this->_gBufferResources.normal.image = Image(app, imageOptions);
  this->_gBufferResources.normal.sampler = Sampler(app, samplerOptions);
  this->_gBufferResources.normal.view = ImageView(
      app,
      this->_gBufferResources.normal.image.getImage(),
      viewOptions);

  imageOptions.format = VK_FORMAT_R8G8B8A8_UNORM;
  viewOptions.format = VK_FORMAT_R8G8B8A8_UNORM;
  this->_gBufferResources.albedo.image = Image(app, imageOptions);
  this->_gBufferResources.albedo.sampler = Sampler(app, samplerOptions);
  this->_gBufferResources.albedo.view = ImageView(
      app,
      this->_gBufferResources.albedo.image.getImage(),
      viewOptions);

  this->_gBufferResources.metallicRoughnessOcclusion.image =
      Image(app, imageOptions);
  this->_gBufferResources.metallicRoughnessOcclusion.sampler =
      Sampler(app, samplerOptions);
  this->_gBufferResources.metallicRoughnessOcclusion.view = ImageView(
      app,
      this->_gBufferResources.metallicRoughnessOcclusion.image.getImage(),
      viewOptions);

  VkClearValue colorClear;
  colorClear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkClearValue depthClear;
  depthClear.depthStencil = {1.0f, 0};

  std::vector<Attachment> attachments = {// GBuffer Position
                                         {ATTACHMENT_FLAG_COLOR,
                                          VK_FORMAT_R16G16B16A16_SFLOAT,
                                          colorClear,
                                          false,
                                          false},

                                         // GBuffer Normal
                                         {ATTACHMENT_FLAG_COLOR,
                                          VK_FORMAT_R16G16B16A16_SFLOAT,
                                          colorClear,
                                          false,
                                          false},

                                         // GBuffer Albedo
                                         {ATTACHMENT_FLAG_COLOR,
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          colorClear,
                                          false,
                                          false},

                                         // GBuffer Metallic-Roughness-Occlusion
                                         {ATTACHMENT_FLAG_COLOR,
                                          VK_FORMAT_R8G8B8A8_UNORM,
                                          colorClear,
                                          false,
                                          false},

                                         // Depth buffer
                                         {ATTACHMENT_FLAG_DEPTH,
                                          app.getDepthImageFormat(),
                                          depthClear,
                                          false,
                                          true}};

  std::vector<SubpassBuilder> subpassBuilders;

  // TODO: How should skybox be handled??
  // SKYBOX PASS
  // {
  //   SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
  //   subpassBuilder.colorAttachments.push_back(0);
  //   Skybox::buildPipeline(subpassBuilder.pipelineBuilder);

  //   subpassBuilder.pipelineBuilder
  //       .layoutBuilder
  //       // Global resources (view, projection, skybox)
  //       .addDescriptorSet(this->_pGlobalResources->getLayout());
  // }

  //  FORWARD GLTF PASS
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments = {0, 1, 2, 3};
    subpassBuilder.depthAttachment = 4;

    Primitive::buildPipeline(subpassBuilder.pipelineBuilder);

    subpassBuilder
        .pipelineBuilder
        // Vertex shader
        .addVertexShader(GProjectDirectory + "/Shaders/ForwardPass.vert")
        // Fragment shader
        .addFragmentShader(GProjectDirectory + "/Shaders/ForwardPass.frag")

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_pGlobalResources->getLayout())
        // Material (per-object) resources (diffuse, normal map,
        // metallic-roughness, etc)
        .addDescriptorSet(this->_pGltfMaterialAllocator->getLayout());
  }

  this->_pForwardPass = std::make_unique<RenderPass>(
      app,
      extent,
      std::move(attachments),
      std::move(subpassBuilders));

  std::vector<VkImageView> attachmentViews = {
      this->_gBufferResources.position.view,
      this->_gBufferResources.normal.view,
      this->_gBufferResources.albedo.view,
      this->_gBufferResources.metallicRoughnessOcclusion.view,
      app.getDepthImageView()};

  this->_forwardFrameBuffer =
      FrameBuffer(app, *this->_pForwardPass, extent, attachmentViews);
}

void DemoScene::_createDeferredPass(Application& app) {
  DescriptorSetLayoutBuilder layoutBuilder{};
  layoutBuilder
      // GBuffer Position
      .addTextureBinding(VK_SHADER_STAGE_FRAGMENT_BIT)
      // GBuffer Normal
      .addTextureBinding(VK_SHADER_STAGE_FRAGMENT_BIT)
      // GBuffer Albedo
      .addTextureBinding(VK_SHADER_STAGE_FRAGMENT_BIT)
      // GBuffer Metallic-Roughness-Occlusion
      .addTextureBinding(VK_SHADER_STAGE_FRAGMENT_BIT);

  this->_pDeferredMaterialAllocator =
      std::make_unique<DescriptorSetAllocator>(app, layoutBuilder, 1);
  this->_pDeferredMaterial =
      std::make_unique<Material>(app, *this->_pDeferredMaterialAllocator);

  // Bind G-Buffer resources as textures in the deferred pass
  this->_pDeferredMaterial->assign()
      .bindTexture(this->_gBufferResources.position)
      .bindTexture(this->_gBufferResources.normal)
      .bindTexture(this->_gBufferResources.albedo)
      .bindTexture(this->_gBufferResources.metallicRoughnessOcclusion);

  VkClearValue colorClear;
  colorClear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkClearValue depthClear;
  depthClear.depthStencil = {1.0f, 0};

  std::vector<Attachment> attachments = {
      {ATTACHMENT_FLAG_COLOR,
       app.getSwapChainImageFormat(),
       colorClear,
       true,
       false}};

  std::vector<SubpassBuilder> subpassBuilders;

  // // SKYBOX PASS
  // {
  //   SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
  //   subpassBuilder.colorAttachments.push_back(0);
  //   Skybox::buildPipeline(subpassBuilder.pipelineBuilder);

  //   subpassBuilder.pipelineBuilder
  //       .layoutBuilder
  //       // Global resources (view, projection, skybox)
  //       .addDescriptorSet(this->_pGlobalResources->getLayout());
  // }

  // DEFERRED PBR PASS
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments.push_back(0);

    subpassBuilder.pipelineBuilder.setCullMode(VK_CULL_MODE_FRONT_BIT)
        .setDepthTesting(false)

        // Vertex shader
        .addVertexShader(GProjectDirectory + "/Shaders/DeferredPass.vert")
        // Fragment shader
        .addFragmentShader(GProjectDirectory + "/Shaders/DeferredPass.frag")

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_pGlobalResources->getLayout())
        // GBuffer material (position, normal, albedo,
        // metallic-roughness-occlusion)
        .addDescriptorSet(this->_pDeferredMaterialAllocator->getLayout());
  }

  this->_pDeferredPass = std::make_unique<RenderPass>(
      app,
      app.getSwapChainExtent(),
      std::move(attachments),
      std::move(subpassBuilders));

  this->_swapChainFrameBuffers =
      SwapChainFrameBufferCollection(app, *this->_pDeferredPass, {});
}

namespace {
struct DrawableEnvMap {
  void draw(const DrawContext& context) const {
    context.bindDescriptorSets();
    context.draw(3);
  }
};
} // namespace

void DemoScene::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {

  // TODO: Abstract deferred rendering boiler plate

  // Transition GBuffer to attachment
  this->_gBufferResources.position.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  this->_gBufferResources.normal.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  this->_gBufferResources.albedo.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  this->_gBufferResources.metallicRoughnessOcclusion.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

  VkDescriptorSet globalDescriptorSet =
      this->_pGlobalResources->getCurrentDescriptorSet(frame);

  // Forward pass
  {
    ActiveRenderPass pass = this->_pForwardPass->begin(
        app,
        commandBuffer,
        frame,
        this->_forwardFrameBuffer);
    pass
        // Bind global descriptor sets
        .setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));
    // Draw models
    for (const Model& model : this->_models) {
      pass.draw(model);
    }
  }

  // Transition GBuffer to texture
  this->_gBufferResources.position.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  this->_gBufferResources.normal.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  this->_gBufferResources.albedo.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  this->_gBufferResources.metallicRoughnessOcclusion.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  // Deferred pass
  {
    ActiveRenderPass pass = this->_pDeferredPass->begin(
        app,
        commandBuffer,
        frame,
        this->_swapChainFrameBuffers.getCurrentFrameBuffer(frame));
    pass
        // Bind global descriptor sets
        .setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));

    const DrawContext& context = pass.getDrawContext();
    context.bindDescriptorSets(*this->_pDeferredMaterial);
    context.draw(3);
  }
}
} // namespace DemoScene
} // namespace AltheaDemo