#include "RayTracingDemo.h"

#include <Althea/Application.h>
#include <Althea/BufferUtilities.h>
#include <Althea/Camera.h>
#include <Althea/Cubemap.h>
#include <Althea/DescriptorSet.h>
#include <Althea/GraphicsPipeline.h>
#include <Althea/IndexBuffer.h>
#include <Althea/InputManager.h>
#include <Althea/ModelViewProjection.h>
#include <Althea/Primitive.h>
#include <Althea/SingleTimeCommandBuffer.h>
#include <Althea/Skybox.h>
#include <Althea/Utilities.h>
#include <Althea/VertexBuffer.h>
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
namespace RayTracingDemo {

RayTracingDemo::RayTracingDemo() {}

void RayTracingDemo::initGame(Application& app) {
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
        // TODO: Hot-reload RT pipelines

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

void RayTracingDemo::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void RayTracingDemo::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  SingleTimeCommandBuffer commandBuffer(app);
  this->_createGlobalResources(app, commandBuffer);
  this->_createRayTracingPass(app, commandBuffer);
  this->_createDeferredPass(app);
}

void RayTracingDemo::destroyRenderState(Application& app) {
  this->_models.clear();

  this->_gBufferResources = {};

  this->_pDeferredPass.reset();
  this->_swapChainFrameBuffers = {};
  this->_pDeferredMaterial.reset();
  this->_pDeferredMaterialAllocator.reset();

  this->_accelerationStructure = {};
  this->_shaderBindingTable = {};
  this->_rayTracingTarget = {};
  this->_pRayTracingMaterial.reset();
  this->_pRayTracingMaterialAllocator.reset();
  this->_pRayTracingPipeline.reset();
  this->_pDisplayPassMaterial.reset();
  this->_pDisplayPassMaterialAllocator.reset();
  this->_pDisplayPass.reset();
  this->_displayPassSwapChainFrameBuffers = {};

  this->_pGlobalResources.reset();
  this->_pGlobalUniforms.reset();
  this->_pointLights = {};
  this->_textureHeap = {};
  this->_vertexBufferHeap = {};
  this->_indexBufferHeap = {};
  this->_primitiveConstantsBuffer = {};
  this->_iblResources = {};

  this->_pSSR.reset();
}

void RayTracingDemo::tick(Application& app, const FrameContext& frame) {
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

void RayTracingDemo::_createModels(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {

  // TODO: BINDLESS!!
  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/DamagedHelmet.glb");
  this->_models.back().setModelTransform(glm::scale(
      glm::translate(glm::mat4(1.0f), glm::vec3(36.0f, 0.0f, 0.0f)),
      glm::vec3(4.0f)));

  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/FlightHelmet/FlightHelmet.gltf");
  this->_models.back().setModelTransform(glm::scale(
      glm::translate(glm::mat4(1.0f), glm::vec3(50.0f, -1.0f, 0.0f)),
      glm::vec3(8.0f)));

  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/MetalRoughSpheres.glb");
  this->_models.back().setModelTransform(glm::scale(
      glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)),
      glm::vec3(4.0f)));

  /*this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/Sponza/glTF/Sponza.gltf");
  this->_models.back().setModelTransform(glm::translate(
      glm::scale(glm::mat4(1.0f), glm::vec3(10.0f)),
      glm::vec3(10.0f, -1.0f, 0.0f)));*/
}

void RayTracingDemo::_createGlobalResources(
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
            primitive.getPrimitiveIndex());
      }
    }

    this->_primitiveConstantsBuffer.upload();

    this->_textureHeap = TextureHeap(this->_models);
    this->_vertexBufferHeap = BufferHeap::CreateVertexBufferHeap(this->_models);
    this->_indexBufferHeap = BufferHeap::CreateIndexBufferHeap(this->_models);
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
        .addStorageBufferBinding(
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
        // Shadow map texture.
        .addTextureBinding()
        // Texture heap.
        .addTextureHeapBinding(
            this->_textureHeap.getSize(),
            VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR)
        // Primitive constants heap.
        .addStorageBufferBinding(
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR)
        // Vertex buffer heap.
        .addBufferHeapBinding(this->_vertexBufferHeap.getSize(), VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR)
        // Index buffer heap.
        .addBufferHeapBinding(this->_indexBufferHeap.getSize(), VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);

    this->_pGlobalResources =
        std::make_unique<PerFrameResources>(app, globalResourceLayout);
    this->_pGlobalUniforms =
        std::make_unique<TransientUniforms<GlobalUniforms>>(app);

    this->_pointLights = PointLightCollection(
        app,
        commandBuffer,
        9,
        true,
        this->_pGlobalResources->getLayout(),
        true,
        8,
        7,
        this->_textureHeap.getSize());
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
    assignment.bindTextureHeap(this->_textureHeap);
    assignment.bindStorageBuffer(
        this->_primitiveConstantsBuffer.getAllocation(),
        this->_primitiveConstantsBuffer.getSize(),
        false);
    assignment.bindBufferHeap(this->_vertexBufferHeap);
    assignment.bindBufferHeap(this->_indexBufferHeap);
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

static uint32_t alignUp(uint32_t size, uint32_t align) {
  return (size + (align - 1)) & ~(align - 1);
}

void RayTracingDemo::_createRayTracingPass(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {

  this->_accelerationStructure =
      AccelerationStructure(app, commandBuffer, this->_models);

  //DescriptorSetLayoutBuilder rayTracingMaterialLayout{};
  // TODO: Use ray queries in deferred pass instead
  ImageOptions imageOptions{};
  imageOptions.width = 1080;
  imageOptions.height = 960;
  imageOptions.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

  this->_rayTracingTarget.image = Image(app, imageOptions);
  this->_rayTracingTarget.view =
      ImageView(app, this->_rayTracingTarget.image, {});
  this->_rayTracingTarget.sampler = Sampler(app, {});

  // Material layout
  DescriptorSetLayoutBuilder matBuilder{};
  matBuilder.addAccelerationStructureBinding(VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  matBuilder.addStorageImageBinding(VK_SHADER_STAGE_RAYGEN_BIT_KHR);

  this->_pRayTracingMaterialAllocator =
      std::make_unique<DescriptorSetAllocator>(app, matBuilder, 1);
  this->_pRayTracingMaterial =
      std::make_unique<Material>(app, *this->_pRayTracingMaterialAllocator);

  this->_pRayTracingMaterial->assign()
      .bindAccelerationStructure(this->_accelerationStructure.getTLAS())
      .bindStorageImage(
          this->_rayTracingTarget.view,
          this->_rayTracingTarget.sampler);

  ShaderDefines defs;
  defs.emplace(
      "TEXTURE_HEAP_COUNT",
      std::to_string(this->_textureHeap.getSize()));
  defs.emplace(
      "VERTEX_BUFFER_HEAP_COUNT",
      std::to_string(this->_vertexBufferHeap.getSize()));
  defs.emplace(
      "INDEX_BUFFER_HEAP_COUNT",
      std::to_string(this->_indexBufferHeap.getSize()));

  RayTracingPipelineBuilder builder{};
  builder.setRayGenShader(
      GEngineDirectory + "/Shaders/RayTracing/RayGen.glsl",
      defs);
  builder.addMissShader(
      GEngineDirectory + "/Shaders/RayTracing/Miss.glsl",
      defs);
  builder.addClosestHitShader(
      GEngineDirectory + "/Shaders/RayTracing/ClosestHit.glsl",
      defs);

  builder.layoutBuilder.addDescriptorSet(this->_pGlobalResources->getLayout())
      .addDescriptorSet(this->_pRayTracingMaterialAllocator->getLayout());

  this->_pRayTracingPipeline =
      std::make_unique<RayTracingPipeline>(app, std::move(builder));

  // Create Shader Binding Table
  this->_shaderBindingTable =
      ShaderBindingTable(app, *this->_pRayTracingPipeline);

  // Display Pass
  DescriptorSetLayoutBuilder displayPassMatLayout{};
  displayPassMatLayout.addTextureBinding();

  this->_pDisplayPassMaterialAllocator =
      std::make_unique<DescriptorSetAllocator>(app, displayPassMatLayout, 1);
  this->_pDisplayPassMaterial =
      std::make_unique<Material>(app, *this->_pDisplayPassMaterialAllocator);

  this->_pDisplayPassMaterial->assign().bindTexture(this->_rayTracingTarget);

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
  };

  std::vector<SubpassBuilder> subpassBuilders;

  // DISPLAY PASS
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments.push_back(0);

    subpassBuilder.pipelineBuilder.setCullMode(VK_CULL_MODE_FRONT_BIT)
        .setDepthTesting(false)

        // Vertex shader
        .addVertexShader(GEngineDirectory + "/Shaders/Misc/FullScreenQuad.vert")
        // Fragment shader
        .addFragmentShader(
            GEngineDirectory + "/Shaders/Misc/FullScreenTexture.frag")

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_pGlobalResources->getLayout())
        .addDescriptorSet(this->_pDisplayPassMaterialAllocator->getLayout());
  }

  this->_pDisplayPass = std::make_unique<RenderPass>(
      app,
      app.getSwapChainExtent(),
      std::move(attachments),
      std::move(subpassBuilders));

  this->_displayPassSwapChainFrameBuffers =
      SwapChainFrameBufferCollection(app, *this->_pDisplayPass, {});
}

void RayTracingDemo::_createDeferredPass(Application& app) {
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

  this->_swapChainFrameBuffers = SwapChainFrameBufferCollection(
      app,
      *this->_pDeferredPass,
      {app.getDepthImageView()});
}

namespace {
struct DrawableEnvMap {
  void draw(const DrawContext& context) const {
    context.bindDescriptorSets();
    context.draw(3);
  }
};
} // namespace

void RayTracingDemo::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {

  VkDescriptorSet globalDescriptorSet =
      this->_pGlobalResources->getCurrentDescriptorSet(frame);

  /*
    this->_pointLights.updateResource(frame);
    this->_gBufferResources.transitionToAttachment(commandBuffer);

    // Draw point light shadow maps
    this->_pointLights.drawShadowMaps(app, commandBuffer, frame, this->_models);

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

      {
        const DrawContext& context = pass.getDrawContext();
        context.bindDescriptorSets(*this->_pDeferredMaterial);
        context.draw(3);
      }

      pass.nextSubpass();
      pass.setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));
      pass.draw(this->_pointLights);
    }

    */

  this->_rayTracingTarget.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_GENERAL,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

  vkCmdBindPipeline(
      commandBuffer,
      VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
      *this->_pRayTracingPipeline);
  // VkDescriptorSet rtDescSets = { globalDescriptorSet, this?}
  {
    VkDescriptorSet sets[2] = {
        globalDescriptorSet,
        this->_pRayTracingMaterial->getCurrentDescriptorSet(frame)};
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        this->_pRayTracingPipeline->getLayout(),
        0,
        2,
        sets,
        0,
        nullptr);

    Application::vkCmdTraceRaysKHR(
        commandBuffer,
        &this->_shaderBindingTable.getRayGenRegion(),
        &this->_shaderBindingTable.getMissRegion(),
        &this->_shaderBindingTable.getHitRegion(),
        &this->_shaderBindingTable.getCallRegion(),
        1080,
        960,
        1);
  }

  this->_rayTracingTarget.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  // Display pass
  {
    ActiveRenderPass pass = this->_pDisplayPass->begin(
        app,
        commandBuffer,
        frame,
        this->_displayPassSwapChainFrameBuffers.getCurrentFrameBuffer(frame));
    // Bind global descriptor sets
    pass.setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));

    {
      const DrawContext& context = pass.getDrawContext();
      context.bindDescriptorSets(*this->_pDisplayPassMaterial);
      context.draw(3);
    }
  }
}
} // namespace RayTracingDemo
} // namespace AltheaDemo