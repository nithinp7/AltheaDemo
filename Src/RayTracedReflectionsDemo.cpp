#include "RayTracedReflectionsDemo.h"

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
namespace RayTracedReflectionsDemo {

RayTracedReflectionsDemo::RayTracedReflectionsDemo() {}

void RayTracedReflectionsDemo::initGame(Application& app) {
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
        that->_pRTR->tryRecompileShaders(app);

        for (Subpass& subpass :
             that->_pointLights.getShadowMapPass().getSubpasses()) {
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

void RayTracedReflectionsDemo::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void RayTracedReflectionsDemo::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  SingleTimeCommandBuffer commandBuffer(app);
  this->_createGlobalResources(app, commandBuffer);
  this->_createForwardPass(app);
  this->_createDeferredPass(app);
}

void RayTracedReflectionsDemo::destroyRenderState(Application& app) {
  this->_models.clear();

  this->_pForwardPass.reset();
  this->_gBufferResources = {};
  this->_forwardFrameBuffer = {};
  this->_primitiveConstantsBuffer = {};
  this->_vertexBufferHeap = {};
  this->_indexBufferHeap = {};
  this->_accelerationStructure = {};

  this->_pDeferredPass.reset();
  this->_swapChainFrameBuffers = {};
  this->_pDeferredMaterialAllocator.reset();

  this->_pGlobalResources.reset();
  this->_pGlobalUniforms.reset();
  this->_pointLights = {};
  this->_iblResources = {};

  this->_shaderDefs.clear();
  this->_pRTR.reset();
}

void RayTracedReflectionsDemo::tick(
    Application& app,
    const FrameContext& frame) {
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
    PointLight light = this->_pointLights.getLight(i);

    light.position = 40.0f * glm::vec3(
                                 static_cast<float>(i / 3),
                                 -0.1f,
                                 (static_cast<float>(i % 3) - 1.5f) * 0.5f);

    light.position.x += 5.5f * cos(1.5f * frame.currentTime + i);
    light.position.z += 5.5f * sin(1.5 * frame.currentTime + i);

    this->_pointLights.setLight(i, light);
  }

  this->_pointLights.updateResource(frame);
}

void RayTracedReflectionsDemo::_createModels(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {

  // this->_models.emplace_back(
  //     app,
  //     commandBuffer,
  //     GEngineDirectory + "/Content/Models/DamagedHelmet.glb");
  // this->_models.back().setModelTransform(glm::scale(
  //     glm::translate(glm::mat4(1.0f), glm::vec3(36.0f, 0.0f, 0.0f)),
  //     glm::vec3(4.0f)));

  // this->_models.emplace_back(
  //     app,
  //     commandBuffer,
  //     GEngineDirectory + "/Content/Models/FlightHelmet/FlightHelmet.gltf");
  // this->_models.back().setModelTransform(glm::scale(
  //     glm::translate(glm::mat4(1.0f), glm::vec3(50.0f, -1.0f, 0.0f)),
  //     glm::vec3(8.0f)));

  // this->_models.emplace_back(
  //     app,
  //     commandBuffer,
  //     GEngineDirectory + "/Content/Models/MetalRoughSpheres.glb");
  // this->_models.back().setModelTransform(glm::scale(
  //     glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)),
  //     glm::vec3(4.0f)));

  // this->_models.emplace_back(
  //     app,
  //     commandBuffer,
  //     GEngineDirectory + "/Content/Models/Sponza/glTF/Sponza.gltf");
  // this->_models.back().setModelTransform(glm::translate(
  //     glm::scale(glm::mat4(1.0f), glm::vec3(10.0f)),
  //     glm::vec3(10.0f, -1.0f, 0.0f)));
}

void RayTracedReflectionsDemo::_createGlobalResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  this->_iblResources = ImageBasedLighting::createResources(
      app,
      commandBuffer,
      "NeoclassicalInterior");
  this->_gBufferResources = GBufferResources(app);

  // Create GLTF resource heaps
  {
    this->_createModels(app, commandBuffer);

    uint32_t primCount = 0;
    for (const Model& model : this->_models)
      primCount += static_cast<uint32_t>(model.getPrimitivesCount());

    this->_primitiveConstantsBuffer =
        StructuredBuffer<PrimitiveConstants>(app, primCount);

    for (const Model& model : this->_models) {
      for (const Primitive& primitive : model.getPrimitives()) {
        this->_primitiveConstantsBuffer.setElement(
            primitive.getConstants(),
            0);
      }
    }

    this->_primitiveConstantsBuffer.upload(app, (VkCommandBuffer)commandBuffer);

    this->_vertexBufferHeap = BufferHeap::CreateVertexBufferHeap(this->_models);
    this->_indexBufferHeap = BufferHeap::CreateIndexBufferHeap(this->_models);
    this->_accelerationStructure =
        AccelerationStructure(app, commandBuffer, this->_models);
  }

  // Global resources
  {
    DescriptorSetLayoutBuilder globalResourceLayout;

    // Add textures for IBL
    ImageBasedLighting::buildLayout(globalResourceLayout, VK_SHADER_STAGE_ALL);
    globalResourceLayout
        // Global uniforms.
        .addUniformBufferBinding(VK_SHADER_STAGE_ALL)
        // Point light buffer.
        .addStorageBufferBinding(VK_SHADER_STAGE_ALL)
        // Shadow map texture.
        .addTextureBinding(VK_SHADER_STAGE_ALL)
        // Primitive constants heap.
        .addStorageBufferBinding(VK_SHADER_STAGE_ALL)
        // Vertex buffer heap
        .addBufferHeapBinding(
            this->_vertexBufferHeap.getSize(),
            VK_SHADER_STAGE_ALL)
        // Index buffer heap
        .addBufferHeapBinding(
            this->_indexBufferHeap.getSize(),
            VK_SHADER_STAGE_ALL);

    this->_pGlobalResources =
        std::make_unique<PerFrameResources>(app, globalResourceLayout);
    this->_pGlobalUniforms =
        std::make_unique<TransientUniforms<GlobalUniforms>>(app);

    // this->_pointLights = PointLightCollection(
    //     app,
    //     commandBuffer,
    //     9,
    //     true,
    //     this->_pGlobalResources->getLayout(),
    //     true,
    //     8,
    //     7,
    //     this->_textureHeap.getSize());
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
    assignment.bindStorageBuffer(
        this->_primitiveConstantsBuffer.getAllocation(),
        this->_primitiveConstantsBuffer.getSize(),
        false);
    assignment.bindBufferHeap(this->_vertexBufferHeap);
    assignment.bindBufferHeap(this->_indexBufferHeap);
  }

  this->_shaderDefs.emplace(
      "VERTEX_BUFFER_HEAP_COUNT",
      std::to_string(this->_vertexBufferHeap.getSize()));
  this->_shaderDefs.emplace(
      "INDEX_BUFFER_HEAP_COUNT",
      std::to_string(this->_indexBufferHeap.getSize()));

  // Set up reflection resources
  this->_pRTR = std::make_unique<RayTracedReflection>(
      app,
      commandBuffer,
      this->_pGlobalResources->getLayout(),
      this->_accelerationStructure.getTLAS(),
      this->_gBufferResources,
      this->_shaderDefs);

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

    // Bind G-Buffer resources as textures in the deferred pass
  }
}

void RayTracedReflectionsDemo::_createForwardPass(Application& app) {
  std::vector<SubpassBuilder> subpassBuilders;

  //  FORWARD GLTF PASS
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    GBufferResources::setupAttachments(subpassBuilder);

    Primitive::buildPipeline(subpassBuilder.pipelineBuilder);

    ShaderDefines defs;

    subpassBuilder
        .pipelineBuilder
        // Vertex shader
        .addVertexShader(GEngineDirectory + "/Shaders/GltfForwardBindless.vert")
        // Fragment shader
        .addFragmentShader(
            GEngineDirectory + "/Shaders/GltfForwardBindless.frag",
            defs)

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_pGlobalResources->getLayout());
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
      this->_gBufferResources.getAttachmentViewsA());
}

void RayTracedReflectionsDemo::_createDeferredPass(Application& app) {
  VkClearValue colorClear;
  colorClear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkClearValue depthClear;
  depthClear.depthStencil = {1.0f, 0};

  std::vector<Attachment> attachments = {
      Attachment{
          ATTACHMENT_FLAG_COLOR,
          app.getSwapChainImageFormat(),
          colorClear,
          true,
          false,
          true},

      // Depth buffer
      Attachment{
          ATTACHMENT_FLAG_DEPTH,
          app.getDepthImageFormat(),
          depthClear,
          false,
          true,
          true}};

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

  // SHOW POINT LIGHTS (kinda hacky)
  // TODO: Really light objects should be rendered in the forward
  // pass as well and an emissive channel should be added to the
  // G-Buffer
  this->_pointLights.setupPointLightMeshSubpass(
      subpassBuilders.emplace_back(),
      0,
      1,
      this->_pGlobalResources->getLayout());

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

void RayTracedReflectionsDemo::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {

  this->_pointLights.updateResource(frame);
  this->_gBufferResources.transitionToAttachment(commandBuffer);

  VkDescriptorSet globalDescriptorSet =
      this->_pGlobalResources->getCurrentDescriptorSet(frame);

  // Draw point light shadow maps
  // this->_pointLights.drawShadowMaps(app, commandBuffer, frame, this->_models,
  // globalDescriptorSet);

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
    // for (const Model& model : this->_models) {
    //   pass.draw(model);
    // }
  }

  this->_gBufferResources.transitionToTextures(commandBuffer);

  // Reflection buffer and convolution
  {
    this->_pRTR
        ->captureReflection(app, commandBuffer, globalDescriptorSet, frame);
    this->_pRTR->convolveReflectionBuffer(app, commandBuffer, frame);
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

    {
      const DrawContext& context = pass.getDrawContext();
      context.draw(3);
    }

    pass.nextSubpass();
    pass.setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));
    // pass.draw(this->_pointLights);
  }
}
} // namespace RayTracedReflectionsDemo
} // namespace AltheaDemo