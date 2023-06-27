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
  this->_pCameraController->setMaxSpeed(15.0f);

  // TODO: need to unbind these at shutdown
  InputManager& input = app.getInputManager();
  input.addKeyBinding(
      {GLFW_KEY_L, GLFW_PRESS, 0},
      [&adjustingExposure = this->_adjustingExposure, &input]() {
        adjustingExposure = true;
        input.setMouseCursorHidden(false);
      });

  input.addKeyBinding(
      {GLFW_KEY_L, GLFW_RELEASE, 0},
      [&adjustingExposure = this->_adjustingExposure, &input]() {
        adjustingExposure = false;
        input.setMouseCursorHidden(true);
      });

  // Recreate any stale pipelines (shader hot-reload)
  input.addKeyBinding(
      {GLFW_KEY_R, GLFW_PRESS, GLFW_MOD_CONTROL},
      [&app, that = this]() {
        for (Subpass& subpass : that->_pShadowPass->getSubpasses()) {
          GraphicsPipeline& pipeline = subpass.getPipeline();
          if (pipeline.recompileStaleShaders()) {
            if (pipeline.hasShaderRecompileErrors()) {
              std::cout << pipeline.getShaderRecompileErrors() << "\n";
            } else {
              pipeline.recreatePipeline(app);
            }
          }
        }

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
      [&adjustingExposure = this->_adjustingExposure,
       &exposure = this->_exposure](double x, double y, bool cursorHidden) {
        if (adjustingExposure) {
          exposure = static_cast<float>(y);
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
  this->_createShadowPass(app);
  this->_createForwardPass(app);
  this->_createDeferredPass(app);
}

void DemoScene::destroyRenderState(Application& app) {
  this->_models.clear();

  this->_pShadowPass.reset();
  this->_shadowFrameBuffers.clear();
  this->_shadowResources.clear();
  this->_shadowUniforms.clear();

  this->_pForwardPass.reset();
  this->_gBufferResources = {};
  this->_forwardFrameBuffer = {};

  this->_pDeferredPass.reset();
  this->_swapChainFrameBuffers = {};
  this->_pDeferredMaterial.reset();
  this->_pDeferredMaterialAllocator.reset();

  this->_pGlobalResources.reset();
  this->_pGlobalUniforms.reset();
  this->_pointLights = {};
  this->_pGltfMaterialAllocator.reset();
  this->_iblResources = {};

  this->_pSSR.reset();
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
  globalUniforms.lightCount = static_cast<int>(this->_pointLights.getCount());
  globalUniforms.time = static_cast<float>(frame.currentTime);
  globalUniforms.exposure = this->_exposure;

  this->_pGlobalUniforms->updateUniforms(globalUniforms, frame);

  for (uint32_t i = 0; i < this->_pointLights.getCount(); ++i) {
    const PointLight& light = this->_pointLights.getLight(i);

    Camera sceneCaptureCamera(90.0f, 1.0f, 0.01f, 1000.0f);

    ShadowMapUniforms shadowMapInfo{};
    shadowMapInfo.projection = sceneCaptureCamera.getProjection();
    shadowMapInfo.inverseProjection = glm::inverse(shadowMapInfo.projection);

    glm::mat4 view = sceneCaptureCamera.computeView();
    glm::mat4 invView = glm::inverse(view);

    // front back up down right left
    // X+ X- Y+ Y- Z+ Z-
    glm::vec3 pos = light.position;
    pos.x += 2.5f * cos(frame.currentTime);
    pos.z += 1.5f * sin(frame.currentTime);

    sceneCaptureCamera.setPosition(pos);
    sceneCaptureCamera.setRotation(90.0f, 0.0f);
    shadowMapInfo.views[0] = sceneCaptureCamera.computeView();
    shadowMapInfo.inverseViews[0] = glm::inverse(shadowMapInfo.views[0]);

    sceneCaptureCamera.setRotation(-90.0f, 0.0f);
    shadowMapInfo.views[1] = sceneCaptureCamera.computeView();
    shadowMapInfo.inverseViews[1] = glm::inverse(shadowMapInfo.views[1]);

    sceneCaptureCamera.setRotation(180.0f, 90.0f);
    shadowMapInfo.views[2] = sceneCaptureCamera.computeView();
    shadowMapInfo.inverseViews[2] = glm::inverse(shadowMapInfo.views[2]);

    sceneCaptureCamera.setRotation(180.0f, -90.0f);
    shadowMapInfo.views[3] = sceneCaptureCamera.computeView();
    shadowMapInfo.inverseViews[3] = glm::inverse(shadowMapInfo.views[3]);

    sceneCaptureCamera.setRotation(180.0f, 0.0f);
    shadowMapInfo.views[4] = sceneCaptureCamera.computeView();
    shadowMapInfo.inverseViews[4] = glm::inverse(shadowMapInfo.views[4]);

    sceneCaptureCamera.setRotation(0.0f, 0.0f);
    shadowMapInfo.views[5] = sceneCaptureCamera.computeView();
    shadowMapInfo.inverseViews[5] = glm::inverse(shadowMapInfo.views[5]);

    this->_shadowUniforms[i].updateUniforms(shadowMapInfo, frame);
  }
}

void DemoScene::_createModels(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {

  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/DamagedHelmet.glb",
      *this->_pGltfMaterialAllocator);
  this->_models.back().setModelTransform(glm::scale(
      glm::translate(glm::mat4(1.0f), glm::vec3(36.0f, 0.0f, 0.0f)),
      glm::vec3(4.0f)));

  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/FlightHelmet/FlightHelmet.gltf",
      *this->_pGltfMaterialAllocator);
  this->_models.back().setModelTransform(glm::scale(
      glm::translate(glm::mat4(1.0f), glm::vec3(50.0f, -1.0f, 0.0f)),
      glm::vec3(8.0f)));

  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/MetalRoughSpheres.glb",
      *this->_pGltfMaterialAllocator);
  this->_models.back().setModelTransform(glm::scale(
      glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)),
      glm::vec3(4.0f)));

  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/Sponza/glTF/Sponza.gltf",
      *this->_pGltfMaterialAllocator);
  this->_models.back().setModelTransform(glm::translate(
      glm::scale(glm::mat4(1.0f), glm::vec3(10.0f)),
      glm::vec3(10.0f, -1.0f, 0.0f)));
}

void DemoScene::_createGlobalResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  this->_iblResources = ImageBasedLighting::createResources(
      app,
      commandBuffer,
      "NeoclassicalInterior");
  this->_gBufferResources = GBufferResources(app);

  // Per-primitive material resources
  {
    DescriptorSetLayoutBuilder primitiveMaterialLayout;
    Primitive::buildMaterial(primitiveMaterialLayout);

    this->_pGltfMaterialAllocator =
        std::make_unique<DescriptorSetAllocator>(app, primitiveMaterialLayout);
  }

  // Global resources
  {
    DescriptorSetLayoutBuilder globalResourceLayout;

    // Add textures for IBL
    ImageBasedLighting::buildLayout(globalResourceLayout);
    globalResourceLayout
        // Global uniforms.
        .addUniformBufferBinding()
        // Point light buffer.
        .addStorageBufferBinding(VK_SHADER_STAGE_FRAGMENT_BIT)
        // Shadow map texture.
        .addTextureBinding();

    this->_pGlobalResources =
        std::make_unique<PerFrameResources>(app, globalResourceLayout);
    this->_pGlobalUniforms =
        std::make_unique<TransientUniforms<GlobalUniforms>>(app, commandBuffer);

    this->_pointLights = PointLightCollection(app, commandBuffer, 9, true);
    for (uint32_t i = 0; i < 3; ++i) {
      for (uint32_t j = 0; j < 3; ++j) {
        PointLight light;
        float t = static_cast<float>(i * 3 + j);

        light.position = 40.0f * glm::vec3(
                                     static_cast<float>(i),
                                     -0.1f,
                                     (static_cast<float>(j) - 1.5f) * 0.5f);
        light.emission =
            1000.0f * // / static_cast<float>(i + 1) *
            glm::vec3(cos(t) + 1.0f, sin(t + 1.0f) + 1.0f, sin(t) + 1.0f);

        this->_pointLights.setLight(i * 3 + j, light);
      }
    }

    ResourcesAssignment assignment = this->_pGlobalResources->assign();

    // Bind IBL resources
    this->_iblResources.bind(assignment);

    // Bind global uniforms
    assignment.bindTransientUniforms(*this->_pGlobalUniforms);
    assignment.bindStorageBuffer(
        this->_pointLights.getAllocation(),
        this->_pointLights.getByteSize(),
        true);
    assignment.bindTexture(
        this->_pointLights.getShadowMapArrayView(),
        this->_pointLights.getShadowMapSampler());

    // Shadow map resources
    DescriptorSetLayoutBuilder shadowLayoutBuilder{};
    shadowLayoutBuilder.addUniformBufferBinding(VK_SHADER_STAGE_VERTEX_BIT);

    for (uint32_t i = 0; i < this->_pointLights.getCount(); ++i) {
      this->_shadowResources.emplace_back(app, shadowLayoutBuilder);
      this->_shadowUniforms.emplace_back(app, commandBuffer);
      this->_shadowResources.back().assign().bindTransientUniforms(
          this->_shadowUniforms.back());
    }
  }

  // Set up SSR resources
  this->_pSSR = std::make_unique<ScreenSpaceReflection>(
      app,
      commandBuffer,
      this->_pGlobalResources->getLayout(),
      this->_gBufferResources);

  // Deferred pass resources (GBuffer)
  {
    DescriptorSetLayoutBuilder deferredMaterialLayout{};
    this->_gBufferResources.buildMaterial(deferredMaterialLayout);
    // Roughness-filtered reflection buffer
    deferredMaterialLayout.addTextureBinding();

    this->_pDeferredMaterialAllocator =
        std::make_unique<DescriptorSetAllocator>(
            app,
            deferredMaterialLayout,
            1);
    this->_pDeferredMaterial =
        std::make_unique<Material>(app, *this->_pDeferredMaterialAllocator);

    // Bind G-Buffer resources as textures in the deferred pass
    ResourcesAssignment& assignment = this->_pDeferredMaterial->assign();
    this->_gBufferResources.bindTextures(assignment);
    this->_pSSR->bindTexture(assignment);
  }
}

void DemoScene::_createShadowPass(Application& app) {
  std::vector<SubpassBuilder> subpassBuilders;

  //  GLTF SUBPASS
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();

    // Only interested in writing to a depth buffer
    subpassBuilder.depthAttachment = 0;

    Primitive::buildPipeline(subpassBuilder.pipelineBuilder);

    subpassBuilder
        .pipelineBuilder
        // Vertex shader
        .addVertexShader(GEngineDirectory + "/Shaders/ShadowMap.vert")
        .addFragmentShader(GEngineDirectory + "/Shaders/ShadowMap.frag")
        // TODO: Still need fragment shader for opacity masking?

        // Pipeline resource layouts
        .layoutBuilder
        // Shadow pass resources
        // TODO: Verify that this works, all the shadow resources should have
        // the same layout, but I don't know how layout-matching is checked / enforced
        // in Vulkan.
        .addDescriptorSet(this->_shadowResources[0].getLayout())
        // Material (per-object) resources (diffuse, normal map,
        // metallic-roughness, etc)
        .addDescriptorSet(this->_pGltfMaterialAllocator->getLayout());
  }

  VkClearValue depthClear;
  depthClear.depthStencil = {1.0f, 0};

  // TODO: Create shadow-map utilities in Althea
  std::vector<Attachment> attachments = {Attachment{
      ATTACHMENT_FLAG_CUBEMAP | ATTACHMENT_FLAG_DEPTH,
      app.getDepthImageFormat(),
      depthClear,
      false,
      false}};

  // TODO: Get this from the point lights object
  const VkExtent2D& extent = this->_pointLights.getShadowMapExtent();
  this->_pShadowPass = std::make_unique<RenderPass>(
      app,
      extent,
      std::move(attachments),
      std::move(subpassBuilders));

  this->_shadowFrameBuffers.resize(this->_pointLights.getCount());
  for (size_t i = 0; i < this->_pointLights.getCount(); ++i) {
    this->_shadowFrameBuffers[i] = FrameBuffer(
        app,
        *this->_pShadowPass,
        extent,
        {this->_pointLights.getShadowMapTargetView(i)});
  }
}

void DemoScene::_createForwardPass(Application& app) {
  std::vector<SubpassBuilder> subpassBuilders;

  //  FORWARD GLTF PASS
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    // The GBuffer contains the following color attachments
    // 1. Position
    // 2. Normal
    // 3. Albedo
    // 4. Metallic-Roughness-Occlusion
    subpassBuilder.colorAttachments = {0, 1, 2, 3};
    subpassBuilder.depthAttachment = 4;

    Primitive::buildPipeline(subpassBuilder.pipelineBuilder);

    subpassBuilder
        .pipelineBuilder
        // Vertex shader
        .addVertexShader(GEngineDirectory + "/Shaders/GltfForward.vert")
        // Fragment shader
        .addFragmentShader(GEngineDirectory + "/Shaders/GltfForward.frag")

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_pGlobalResources->getLayout())
        // Material (per-object) resources (diffuse, normal map,
        // metallic-roughness, etc)
        .addDescriptorSet(this->_pGltfMaterialAllocator->getLayout());
  }

  std::vector<Attachment> attachments =
      this->_gBufferResources.getAttachmentDescriptions();
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pForwardPass = std::make_unique<RenderPass>(
      app,
      extent,
      std::move(attachments),
      std::move(subpassBuilders));

  this->_forwardFrameBuffer = FrameBuffer(
      app,
      *this->_pForwardPass,
      extent,
      this->_gBufferResources.getAttachmentViews());
}

void DemoScene::_createDeferredPass(Application& app) {
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

  this->_pointLights.updateResource(frame);
  this->_gBufferResources.transitionToAttachment(commandBuffer);

  VkDescriptorSet globalDescriptorSet =
      this->_pGlobalResources->getCurrentDescriptorSet(frame);

  // Shadow passes
  this->_pointLights.transitionToAttachment(commandBuffer);
  for (uint32_t i = 0; i < this->_pointLights.getCount(); ++i) {
    const PointLight& light = this->_pointLights.getLight(i);

    ActiveRenderPass pass = this->_pShadowPass->begin(
        app,
        commandBuffer,
        frame,
        this->_shadowFrameBuffers[i]);

    VkDescriptorSet shadowDescriptorSet =
        this->_shadowResources[i].getCurrentDescriptorSet(frame);
    pass.setGlobalDescriptorSets(gsl::span(&shadowDescriptorSet, 1));

    // Draw models
    for (const Model& model : this->_models) {
      pass.draw(model);
    }
  }
  this->_pointLights.transitionToTexture(commandBuffer);

  // Forward pass
  {
    ActiveRenderPass pass = this->_pForwardPass->begin(
        app,
        commandBuffer,
        frame,
        this->_forwardFrameBuffer);
    // Bind global descriptor sets
    pass.setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));
    // Draw models
    for (const Model& model : this->_models) {
      pass.draw(model);
    }
  }

  this->_gBufferResources.transitionToTextures(commandBuffer);

  // Reflection buffer and convolution
  {
    this->_pSSR
        ->captureReflection(app, commandBuffer, globalDescriptorSet, frame);
    this->_pSSR->convolveReflectionBuffer(app, commandBuffer, frame);
  }

  // Deferred pass
  {
    ActiveRenderPass pass = this->_pDeferredPass->begin(
        app,
        commandBuffer,
        frame,
        this->_swapChainFrameBuffers.getCurrentFrameBuffer(frame));
    // Bind global descriptor sets
    pass.setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));

    const DrawContext& context = pass.getDrawContext();
    context.bindDescriptorSets(*this->_pDeferredMaterial);
    context.draw(3);
  }
}
} // namespace DemoScene
} // namespace AltheaDemo