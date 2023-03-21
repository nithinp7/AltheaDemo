#include "DemoScene.h"

#include <Althea/Application.h>
#include <Althea/Camera.h>
#include <Althea/Cubemap.h>
#include <Althea/DescriptorSet.h>
#include <Althea/GraphicsPipeline.h>
#include <Althea/InputManager.h>
#include <Althea/ModelViewProjection.h>
#include <Althea/Primitive.h>
#include <Althea/Shader.h>
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
namespace {
struct SphereInstance {
  glm::vec3 baseColor;
  float roughness;
  float metallic;
};
} // namespace

DemoScene::Sphere::Sphere() {
  constexpr uint32_t resolution = 50;
  constexpr float maxPitch = 0.499f * glm::pi<float>();

  auto sphereUvIndexToVertIndex = [resolution](uint32_t i, uint32_t j) {
    i = i % resolution;
    return i * resolution / 2 + j;
  };

  // Verts from the cylinder mapping
  uint32_t cylinderVertsCount = resolution * resolution / 2;
  // Will include cylinder mapped verts and 2 cap verts
  this->vertices.reserve(cylinderVertsCount + 2);
  this->indices.reserve(3 * resolution * resolution);
  for (uint32_t i = 0; i < resolution; ++i) {
    float theta = i * 2.0f * glm::pi<float>() / resolution;
    float cosTheta = cos(theta);
    float sinTheta = sin(theta);

    for (uint32_t j = 0; j < resolution / 2; ++j) {
      float phi = j * 2.0f * maxPitch / (resolution / 2) - maxPitch;
      float cosPhi = cos(phi);
      float sinPhi = sin(phi);

      this->vertices.emplace_back(
          cosPhi * cosTheta,
          sinPhi,
          -cosPhi * sinTheta);

      if (j < resolution / 2 - 1) {
        this->indices.push_back(sphereUvIndexToVertIndex(i, j));
        this->indices.push_back(sphereUvIndexToVertIndex(i + 1, j));
        this->indices.push_back(sphereUvIndexToVertIndex(i + 1, j + 1));

        this->indices.push_back(sphereUvIndexToVertIndex(i, j));
        this->indices.push_back(sphereUvIndexToVertIndex(i + 1, j + 1));
        this->indices.push_back(sphereUvIndexToVertIndex(i, j + 1));
      } else {
        this->indices.push_back(sphereUvIndexToVertIndex(i, j));
        this->indices.push_back(sphereUvIndexToVertIndex(i + 1, j));
        this->indices.push_back(cylinderVertsCount);
      }

      if (j == 0) {
        this->indices.push_back(sphereUvIndexToVertIndex(i, j));
        this->indices.push_back(cylinderVertsCount + 1);
        this->indices.push_back(sphereUvIndexToVertIndex(i + 1, j));
      }
    }
  }

  // Cap vertices
  this->vertices.emplace_back(0.0f, 1.0f, 0.0f);
  this->vertices.emplace_back(0.0f, -1.0f, 0.0f);
}

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

  float probeSpeed = 0.8f;
  input.addKeyBinding(
      {GLFW_KEY_LEFT, GLFW_PRESS, 0},
      [&translation = this->_probeCollection.location, probeSpeed, &input]() {
        translation.x -= probeSpeed;
      });
  input.addKeyBinding(
      {GLFW_KEY_RIGHT, GLFW_PRESS, 0},
      [&translation = this->_probeCollection.location, probeSpeed, &input]() {
        translation.x += probeSpeed;
      });
  input.addKeyBinding(
      {GLFW_KEY_DOWN, GLFW_PRESS, 0},
      [&translation = this->_probeCollection.location, probeSpeed, &input]() {
        translation.z += probeSpeed;
      });
  input.addKeyBinding(
      {GLFW_KEY_UP, GLFW_PRESS, 0},
      [&translation = this->_probeCollection.location, probeSpeed, &input]() {
        translation.z -= probeSpeed;
      });

  input.addKeyBinding(
      {GLFW_KEY_P, GLFW_PRESS, 0},
      [&dirty = this->_probeCollection.dirty]() { dirty = true; });

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

        for (Subpass& subpass :
             that->_probeCollection.pRenderPass->getSubpasses()) {
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

  this->_iblResources =
      ImageBasedLighting::createResources(app, commandBuffer, "LuxuryRoom");

  // TODO: Default color and depth-stencil clear values for attachments?
  VkClearValue colorClear;
  colorClear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkClearValue depthClear;
  depthClear.depthStencil = {1.0f, 0};

  std::vector<Attachment> attachmentDescriptions = {
      {ATTACHMENT_FLAG_COLOR,
       app.getSwapChainImageFormat(),
       colorClear,
       true,
       false},
      {ATTACHMENT_FLAG_DEPTH,
       app.getDepthImageFormat(),
       depthClear,
       false,
       true}};

  // Non-swapchain attachment views
  std::vector<VkImageView> attachmentViews = {app.getDepthImageView()};

  // Global resources
  DescriptorSetLayoutBuilder globalResourceLayout;

  // Add textures for IBL
  ImageBasedLighting::buildLayout(globalResourceLayout);
  globalResourceLayout
      // Global uniforms.
      .addUniformBufferBinding()
      // Reflection probes cube map array
      .addTextureBinding();

  this->_pGlobalResources =
      std::make_shared<PerFrameResources>(app, globalResourceLayout);
  this->_pGlobalUniforms =
      std::make_unique<TransientUniforms<GlobalUniforms>>(app, commandBuffer);

  std::vector<SubpassBuilder> subpassBuilders;

  // SKYBOX PASS
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments.push_back(0);
    Skybox::buildPipeline(subpassBuilder.pipelineBuilder);

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
        .addFragmentShader(
            GEngineDirectory + "/Shaders/GltfPBR.frag",
            {{"ENABLE_REFL_PROBES", ""}})

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_pGlobalResources->getLayout())
        // Material (per-object) resources (diffuse, normal map,
        // metallic-roughness, etc)
        .addDescriptorSet(this->_pGltfMaterialAllocator->getLayout());
  }

  // IBL PROBE VISUALIZATION
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments.push_back(0);
    subpassBuilder.depthAttachment = 1;

    subpassBuilder.pipelineBuilder
        .setPrimitiveType(PrimitiveType::TRIANGLES)

        .addVertexInputBinding<glm::vec3>()
        .addVertexAttribute(VertexAttributeType::VEC3, 0)

        // Vertex shader
        .addVertexShader(GProjectDirectory + "/Shaders/Probe.vert")
        // Fragment shader
        .addFragmentShader(GProjectDirectory + "/Shaders/Probe.frag")

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_pGlobalResources->getLayout())

        // Push constants for the model matrix
        .addPushConstants<ProbePushConstants>();
  }

  this->_pRenderPass = std::make_unique<RenderPass>(
      app,
      app.getSwapChainExtent(),
      std::move(attachmentDescriptions),
      std::move(subpassBuilders));
  this->_swapChainFrameBuffers =
      SwapChainFrameBufferCollection(app, *this->_pRenderPass, attachmentViews);

  this->_createProbes(app, commandBuffer);

#if 1
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
#endif

  std::vector<glm::vec3> sphereVerts = this->_sphere.vertices;
  this->_sphereVertexBuffer =
      VertexBuffer<glm::vec3>(app, commandBuffer, std::move(sphereVerts));

  std::vector<uint32_t> sphereIndices = this->_sphere.indices;
  this->_sphereIndexBuffer =
      IndexBuffer(app, commandBuffer, std::move(sphereIndices));

  {
    ResourcesAssignment assignment = this->_pGlobalResources->assign();

    // Bind IBL resources
    this->_iblResources.bind(assignment);

    // Bind global uniforms
    assignment.bindTransientUniforms(*this->_pGlobalUniforms);

    // Bind global reflection probe cubemap array
    assignment.bindTexture(
        this->_renderTargets.getColorTextureArrayView(),
        this->_renderTargets.getColorSampler());
  }
}

void DemoScene::destroyRenderState(Application& app) {
  this->_models.clear();

  this->_pRenderPass.reset();
  this->_swapChainFrameBuffers = {};

  this->_pGlobalResources.reset();
  this->_pGlobalUniforms.reset();

  this->_renderTargets = {};
  this->_probeCollection = {};

  this->_pGltfMaterialAllocator.reset();
  this->_iblResources = {};
  this->_sphereVertexBuffer = {};
  this->_sphereIndexBuffer = {};
}

void DemoScene::tick(Application& app, const FrameContext& frame) {
  this->_pCameraController->tick(frame.deltaTime);
  float exposure = 0.6f;
  {
    const Camera& camera = this->_pCameraController->getCamera();

    GlobalUniforms globalUniforms;
    globalUniforms.projection = camera.getProjection();
    globalUniforms.inverseProjection = glm::inverse(globalUniforms.projection);
    globalUniforms.view = camera.computeView();
    globalUniforms.inverseView = glm::inverse(globalUniforms.view);
    globalUniforms.lightDir = this->_lightDir;
    globalUniforms.time = static_cast<float>(frame.currentTime);
    globalUniforms.exposure = exposure;

    this->_pGlobalUniforms->updateUniforms(globalUniforms, frame);
  }

  for (LightProbe& probe : this->_probeCollection.probes) {
    glm::vec3 location = this->_probeCollection.location + probe.positionOffset;

    Camera sceneCaptureCamera(90.0f, 1.0f, 0.01f, 1000.0f);

    GlobalUniformsCubeRender globalUniforms;
    globalUniforms.projection = sceneCaptureCamera.getProjection();
    globalUniforms.inverseProjection = glm::inverse(globalUniforms.projection);

    glm::mat4 view = sceneCaptureCamera.computeView();
    glm::mat4 invView = glm::inverse(view);

    // front back up down right left
    // X+ X- Y+ Y- Z+ Z-
    sceneCaptureCamera.setPosition(location);
    sceneCaptureCamera.setRotation(90.0f, 0.0f);
    globalUniforms.views[0] = sceneCaptureCamera.computeView();
    globalUniforms.inverseViews[0] = glm::inverse(globalUniforms.views[0]);

    sceneCaptureCamera.setRotation(-90.0f, 0.0f);
    globalUniforms.views[1] = sceneCaptureCamera.computeView();
    globalUniforms.inverseViews[1] = glm::inverse(globalUniforms.views[1]);

    sceneCaptureCamera.setRotation(180.0f, 90.0f);
    globalUniforms.views[2] = sceneCaptureCamera.computeView();
    globalUniforms.inverseViews[2] = glm::inverse(globalUniforms.views[2]);

    sceneCaptureCamera.setRotation(180.0f, -90.0f);
    globalUniforms.views[3] = sceneCaptureCamera.computeView();
    globalUniforms.inverseViews[3] = glm::inverse(globalUniforms.views[3]);

    sceneCaptureCamera.setRotation(180.0f, 0.0f);
    globalUniforms.views[4] = sceneCaptureCamera.computeView();
    globalUniforms.inverseViews[4] = glm::inverse(globalUniforms.views[4]);

    sceneCaptureCamera.setRotation(0.0f, 0.0f);
    globalUniforms.views[5] = sceneCaptureCamera.computeView();
    globalUniforms.inverseViews[5] = glm::inverse(globalUniforms.views[5]);

    globalUniforms.lightDir = this->_lightDir;
    globalUniforms.time = static_cast<float>(frame.currentTime);
    globalUniforms.exposure = exposure;

    probe.pUniforms->updateUniforms(globalUniforms, frame);
  }
}

void DemoScene::_createProbes(
    const Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  constexpr uint32_t probesCountX = 10;
  constexpr uint32_t probesCountY = 4;
  constexpr uint32_t probesCountZ = 4;

  uint32_t count = probesCountX * probesCountY * probesCountZ;

  // Create render target resources
  VkExtent2D extent{128, 128};
  this->_renderTargets = RenderTargetCollection(
      app,
      commandBuffer,
      extent,
      RenderTargetType::SceneCaptureCube,
      count);

  // Global resources (still per-probe)
  DescriptorSetLayoutBuilder globalResourceLayout;
  // Add textures for IBL
  ImageBasedLighting::buildLayout(globalResourceLayout);
  globalResourceLayout.addUniformBufferBinding();

  this->_probeCollection.pMaterialAllocator =
      std::make_unique<DescriptorSetAllocator>(
          app,
          globalResourceLayout,
          count);

  // Create render pass
  VkClearValue colorClear;
  colorClear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkClearValue depthClear;
  depthClear.depthStencil = {1.0f, 0};

  ShaderDefines defines = {{"CUBEMAP_MULTIVIEW", ""}, {"SKIP_TONEMAP", ""}};

  std::vector<Attachment> attachmentDescriptions = {
      {ATTACHMENT_FLAG_COLOR | ATTACHMENT_FLAG_CUBEMAP,
       this->_renderTargets.getColorImage().getOptions().format,
       colorClear,
       false,
       false},
      {ATTACHMENT_FLAG_DEPTH | ATTACHMENT_FLAG_CUBEMAP,
       this->_renderTargets.getDepthImage().getOptions().format,
       depthClear,
       false,
       true}};

  std::vector<SubpassBuilder> subpassBuilders;

  // SKYBOX PASS
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments.push_back(0);

    subpassBuilder.pipelineBuilder
        .addVertexShader(GEngineDirectory + "/Shaders/Skybox.vert", defines)
        .addFragmentShader(GEngineDirectory + "/Shaders/Skybox.frag", defines)

        .setCullMode(VK_CULL_MODE_FRONT_BIT)
        .setDepthTesting(false)
        .layoutBuilder
        // Global resources (view, projection, skybox)
        .addDescriptorSet(
            this->_probeCollection.pMaterialAllocator->getLayout());
  }

  // REGULAR PASS
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments.push_back(0);
    subpassBuilder.depthAttachment = 1;

    Primitive::buildPipeline(subpassBuilder.pipelineBuilder);

    subpassBuilder
        .pipelineBuilder
        // Vertex shader
        .addVertexShader(GEngineDirectory + "/Shaders/GltfPBR.vert", defines)
        // Fragment shader
        .addFragmentShader(GEngineDirectory + "/Shaders/GltfPBR.frag", defines)

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(
            this->_probeCollection.pMaterialAllocator->getLayout())
        // Material (per-object) resources (diffuse, normal map,
        // metallic-roughness, etc)
        .addDescriptorSet(this->_pGltfMaterialAllocator->getLayout());
  }

  this->_probeCollection.pRenderPass = std::make_unique<RenderPass>(
      app,
      extent,
      std::move(attachmentDescriptions),
      std::move(subpassBuilders));

  constexpr float spacing = 10.0f;

  this->_probeCollection.probes.resize(count);
  std::vector<VkImageView> scratchViews;
  scratchViews.resize(2);
  uint32_t probeIndex = 0;
  for (uint32_t i = 0; i < probesCountX; ++i) {
    for (uint32_t j = 0; j < probesCountY; ++j) {
      for (uint32_t k = 0; k < probesCountZ; ++k) {
        LightProbe& probe = this->_probeCollection.probes[probeIndex];
        probe.positionOffset = glm::vec3(i, j, k) * spacing;

        probe.pMaterial = std::make_unique<Material>(
            app,
            *this->_probeCollection.pMaterialAllocator);
        probe.pUniforms =
            std::make_unique<TransientUniforms<GlobalUniformsCubeRender>>(
                app,
                commandBuffer);

        scratchViews[0] = this->_renderTargets.getTargetColorView(probeIndex);
        scratchViews[1] = this->_renderTargets.getTargetDepthView(probeIndex);
        // TODO: This should probably take a span instead
        probe.frameBuffer = FrameBuffer(
            app,
            *this->_probeCollection.pRenderPass,
            extent,
            scratchViews);

        ResourcesAssignment assignment = probe.pMaterial->assign();
        this->_iblResources.bind(assignment);
        assignment.bindTransientUniforms(*probe.pUniforms);

        ++probeIndex;
      }
    }
  }
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

  if (this->_probeCollection.dirty) {
    this->_probeCollection.dirty = false;

    this->_renderTargets.transitionToAttachment(commandBuffer);

    for (LightProbe& probe : this->_probeCollection.probes) {
      VkDescriptorSet globalDescriptorSet =
          probe.pMaterial->getCurrentDescriptorSet(frame);

      ActiveRenderPass pass = this->_probeCollection.pRenderPass->begin(
          app,
          commandBuffer,
          frame,
          probe.frameBuffer);
      pass
          // Bind global descriptor sets
          .setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1))

          .draw(DrawableEnvMap{})
          .nextSubpass();

      // Draw models
      for (const Model& model : this->_models) {
        pass.draw(model);
      }
    }

    this->_renderTargets.transitionToTexture(commandBuffer);
  }

  {
    VkDescriptorSet globalDescriptorSet =
        this->_pGlobalResources->getCurrentDescriptorSet(frame);

    ActiveRenderPass pass = this->_pRenderPass->begin(
        app,
        commandBuffer,
        frame,
        this->_swapChainFrameBuffers.getCurrentFrameBuffer(frame));
    pass
        // Bind global descriptor sets
        .setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1))
        // Draw skybox
        .draw(DrawableEnvMap{})
        .nextSubpass();
    // Draw models
    for (const Model& model : this->_models) {
      pass.draw(model);
    }

    pass.nextSubpass();
    pass.setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));

    for (uint32_t i = 0; i < this->_probeCollection.probes.size(); ++i) {
      const LightProbe& probe = this->_probeCollection.probes[i];
      glm::mat4 probeTransform(1.0f);
      probeTransform = glm::scale(probeTransform, glm::vec3(1.0f));
      probeTransform = glm::translate(
          probeTransform,
          this->_probeCollection.location + probe.positionOffset);
      //this->_drawProbe(probeTransform, i, pass.getDrawContext());
    }
  }
}

// TODO: should just instance
void DemoScene::_drawProbe(
    const glm::mat4& transform,
    uint32_t sceneCaptureIndex,
    const DrawContext& context) {
  ProbePushConstants constants{transform, sceneCaptureIndex};

  context.bindDescriptorSets();
  context.bindIndexBuffer(this->_sphereIndexBuffer);
  context.bindVertexBuffer(this->_sphereVertexBuffer);
  context.updatePushConstants(constants, 0);
  vkCmdDrawIndexed(
      context.getCommandBuffer(),
      static_cast<uint32_t>(this->_sphereIndexBuffer.getIndexCount()),
      1,
      0,
      0,
      0);
}
} // namespace DemoScene
} // namespace AltheaDemo