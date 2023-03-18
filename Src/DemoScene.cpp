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

void DemoScene::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void DemoScene::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  SingleTimeCommandBuffer commandBuffer(app);

  this->_iblResources = ImageBasedLighting::createResources(
      app,
      commandBuffer,
      "NeoclassicalInterior");

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

  // Add textures for IBL
  ImageBasedLighting::buildLayout(globalResourceLayout);
  globalResourceLayout
      // Global uniforms.
      .addUniformBufferBinding();

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

  DescriptorSetLayoutBuilder renderTargetResourcesLayoutBuilder{};
  renderTargetResourcesLayoutBuilder.addTextureBinding(
      VK_SHADER_STAGE_FRAGMENT_BIT);

  this->_pRenderTargetResources = std::make_shared<PerFrameResources>(
      app,
      renderTargetResourcesLayoutBuilder);

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

        // Render targets
        .addDescriptorSet(this->_pRenderTargetResources->getLayout())

        // Push constants for the model matrix
        .addPushConstants<glm::mat4>();
  }

  this->_pRenderPass = std::make_unique<RenderPass>(
      app,
      app.getSwapChainExtent(),
      std::move(attachments),
      std::move(subpassBuilders));

  this->_createRenderTarget(app, commandBuffer);

#if 1
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

    this->_pRenderTargetResources->assign().bindTexture(
        this->_target.getColorView(),
        this->_target.getColorSampler());
  }
}

void DemoScene::destroyRenderState(Application& app) {
  this->_models.clear();
  this->_pRenderPass.reset();
  this->_pSceneCaptureRenderPass.reset();
  this->_pGlobalResources.reset();
  this->_pRenderTargetResources.reset();
  this->_pGlobalUniforms.reset();
  this->_pGltfMaterialAllocator.reset();
  this->_iblResources = {};
  this->_target = {};
  this->_sphereVertexBuffer = {};
  this->_sphereIndexBuffer = {};
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

void DemoScene::_createRenderTarget(
    const Application& app,
    VkCommandBuffer commandBuffer) {
  // Create render target resources
  VkExtent2D extent {100, 100};
  this->_target = RenderTarget(app, commandBuffer, extent);

  // Create render pass
  VkClearValue colorClear;
  colorClear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkClearValue depthClear;
  depthClear.depthStencil = {1.0f, 0};

  std::vector<Attachment> attachments = {
      {AttachmentType::Color,
       this->_target.getColorImage().getOptions().format,
       colorClear,
       this->_target.getColorView(),
       false},
      {AttachmentType::Depth,
       this->_target.getDepthImage().getOptions().format,
       depthClear,
       this->_target.getDepthView(),
       true}};

  std::vector<SubpassBuilder> subpassBuilders;

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

  this->_pSceneCaptureRenderPass = std::make_unique<RenderPass>(
      app,
      extent,
      std::move(attachments),
      std::move(subpassBuilders));
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

  this->_target.transitionToAttachment(commandBuffer);

  {
    VkDescriptorSet globalDescriptorSet =
        this->_pGlobalResources->getCurrentDescriptorSet(frame);

    ActiveRenderPass pass =
        this->_pSceneCaptureRenderPass->begin(app, commandBuffer, frame);
    pass
        // Bind global descriptor sets
        .setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));
    // Draw models
    for (const Model& model : this->_models) {
      pass.draw(model);
    }
  }

  this->_target.transitionToTexture(commandBuffer);

  {
    VkDescriptorSet globalResourceAndRenderTarget[2] = {
        this->_pGlobalResources->getCurrentDescriptorSet(frame),
        this->_pRenderTargetResources->getCurrentDescriptorSet(frame)};

    VkDescriptorSet globalDescriptorSet =
        this->_pGlobalResources->getCurrentDescriptorSet(frame);

    ActiveRenderPass pass =
        this->_pRenderPass->begin(app, commandBuffer, frame);
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

    pass.setGlobalDescriptorSets(gsl::span(globalResourceAndRenderTarget, 2));

    glm::mat4 probeTransform(1.0f);
    probeTransform = glm::scale(probeTransform, glm::vec3(5.0f));
    this->_drawProbe(probeTransform, pass.getDrawContext());

    probeTransform =
        glm::translate(probeTransform, glm::vec3(10.0f, 0.0f, 0.0f));
    this->_drawProbe(probeTransform, pass.getDrawContext());
  }
}

// TODO: should just instance
void DemoScene::_drawProbe(
    const glm::mat4& transform,
    const DrawContext& context) {
  context.bindDescriptorSets();
  context.bindIndexBuffer(this->_sphereIndexBuffer);
  context.bindVertexBuffer(this->_sphereVertexBuffer);
  context.updatePushConstants(transform, 0);
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