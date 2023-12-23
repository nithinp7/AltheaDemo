#include "PathTracing.h"

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

#define PROBE_COUNT 100000
#define PROBES_PER_BUFFER (PROBE_COUNT)

#define SPATIAL_HASH_SIZE (PROBES_PER_BUFFER)
#define SPATIAL_HASH_SLOTS_PER_BUFFER (SPATIAL_HASH_SIZE)

namespace AltheaDemo {
namespace PathTracing {

PathTracing::PathTracing() {}

void PathTracing::initGame(Application& app) {
  const VkExtent2D& windowDims = app.getSwapChainExtent();
  this->_pCameraController = std::make_unique<CameraController>(
      app.getInputManager(),
      90.0f,
      (float)windowDims.width / (float)windowDims.height);
  this->_pCameraController->setMaxSpeed(15.0f);
  this->_pCameraController->getCamera().setPosition(
      glm::vec3(2.0f, 2.0f, 2.0f));

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
        that->_framesSinceCameraMoved = 0;

        if (that->_pRayTracingPipeline->recompileStaleShaders()) {
          if (that->_pRayTracingPipeline->hasShaderRecompileErrors()) {
            std::cout << that->_pRayTracingPipeline->getShaderRecompileErrors()
                      << "\n";
          } else {
            that->_pRayTracingPipeline->recreatePipeline(app);
          }
        }

        if (that->_probePass.recompileStaleShaders()) {
          if (that->_probePass.hasShaderRecompileErrors()) {
            std::cout << that->_probePass.getShaderRecompileErrors() << "\n";
          } else {
            that->_probePass.recreatePipeline(app);
          }
        }

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

        for (Subpass& subpass : that->_pDisplayPass->getSubpasses()) {
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
  input.addKeyBinding(
      {GLFW_KEY_F, GLFW_PRESS, 0},
      [&freezeCamera = this->_freezeCamera]() { freezeCamera = false; });
  input.addKeyBinding(
      {GLFW_KEY_F, GLFW_RELEASE, 0},
      [&freezeCamera = this->_freezeCamera]() { freezeCamera = true; });
  input.addMousePositionCallback(
      [&adjustingExposure = this->_adjustingExposure,
       &exposure = this->_exposure](double x, double y, bool cursorHidden) {
        if (adjustingExposure) {
          exposure = static_cast<float>(y);
        }
      });
}

void PathTracing::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void PathTracing::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  SingleTimeCommandBuffer commandBuffer(app);
  this->_createGlobalResources(app, commandBuffer);
  this->_createRayTracingPass(app, commandBuffer);
}

void PathTracing::destroyRenderState(Application& app) {
  this->_models.clear();
  Primitive::resetPrimitiveIndexCount();

  this->_accelerationStructure = {};
  this->_rayTracingTarget[0] = {};
  this->_rayTracingTarget[1] = {};
  this->_depthBuffer[0] = {};
  this->_depthBuffer[1] = {};
  this->_debugTarget = {};
  this->_giUniforms = {};
  this->_probes = {};
  this->_spatialHash = {};
  this->_freeList = {};
  this->_pRayTracingMaterial[0].reset();
  this->_pRayTracingMaterial[1].reset();
  this->_pRayTracingMaterialAllocator.reset();
  this->_pRayTracingPipeline.reset();
  this->_probePass = {};
  this->_pDisplayPassMaterial[0].reset();
  this->_pDisplayPassMaterial[1].reset();
  this->_pDisplayPassMaterialAllocator.reset();
  this->_pDisplayPass.reset();
  this->_displayPassSwapChainFrameBuffers = {};

  this->_globalResources = {};
  this->_globalUniforms = {};
  this->_pointLights = {};
  this->_textureHeap = {};
  this->_vertexBufferHeap = {};
  this->_indexBufferHeap = {};
  this->_primitiveConstantsBuffer = {};
  this->_iblResources = {};
}

void PathTracing::tick(Application& app, const FrameContext& frame) {
  const Camera& camera = this->_pCameraController->getCamera();

  GlobalUniforms globalUniforms;
  globalUniforms.prevView = camera.computeView();
  globalUniforms.prevInverseView = glm::inverse(globalUniforms.prevView);

  // if (this->_freezeCamera) {
  //   ++this->_framesSinceCameraMoved;
  // } else {
  // this->_framesSinceCameraMoved = 0;
  this->_framesSinceCameraMoved++; // TODO: Clean this up..
  this->_pCameraController->tick(frame.deltaTime);
  // }

  globalUniforms.projection = camera.getProjection();
  globalUniforms.inverseProjection = glm::inverse(globalUniforms.projection);
  globalUniforms.view = camera.computeView();
  globalUniforms.inverseView = glm::inverse(globalUniforms.view);
  globalUniforms.lightCount = static_cast<int>(this->_pointLights.getCount());
  globalUniforms.time = static_cast<float>(frame.currentTime);
  globalUniforms.exposure = this->_exposure;

  this->_globalUniforms.updateUniforms(globalUniforms, frame);

  // TODO: Allow lights to move again :)
  // for (uint32_t i = 0; i < this->_pointLights.getCount(); ++i) {
  //   PointLight light = this->_pointLights.getLight(i);

  //   light.position = 40.0f * glm::vec3(
  //                                static_cast<float>(i / 3),
  //                                -0.1f,
  //                                (static_cast<float>(i % 3) - 1.5f) * 0.5f);

  //   light.position.x += 5.5f * cos(1.5f * frame.currentTime + i);
  //   light.position.z += 5.5f * sin(1.5 * frame.currentTime + i);

  //   this->_pointLights.setLight(i, light);
  // }

  this->_pointLights.updateResource(frame);

  GlobalIlluminationUniforms giUniforms{};
  giUniforms.probeCount = PROBE_COUNT;
  giUniforms.probesPerBuffer = PROBES_PER_BUFFER;
  giUniforms.spatialHashSize = SPATIAL_HASH_SIZE;
  giUniforms.spatialHashSlotsPerBuffer = SPATIAL_HASH_SLOTS_PER_BUFFER;

  giUniforms.gridCellSize = 0.1f;

  this->_giUniforms.updateUniforms(giUniforms, frame);
}

void PathTracing::_createModels(
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

  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/Sponza/glTF/Sponza.gltf");
  this->_models.back().setModelTransform(glm::translate(
      glm::scale(glm::mat4(1.0f), glm::vec3(10.0f)),
      glm::vec3(0.0f, -8.0f, 0.0f)));
}

void PathTracing::_createGlobalResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  this->_iblResources = ImageBasedLighting::createResources(
      app,
      commandBuffer,
      "NeoclassicalInterior");

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

    this->_primitiveConstantsBuffer.upload(app, commandBuffer);

    this->_textureHeap = TextureHeap(this->_models);
    this->_vertexBufferHeap = BufferHeap::CreateVertexBufferHeap(this->_models);
    this->_indexBufferHeap = BufferHeap::CreateIndexBufferHeap(this->_models);
  }

  // Global resources
  {
    DescriptorSetLayoutBuilder globalResourceLayout;

    // Add textures for IBL
    ImageBasedLighting::buildLayout(globalResourceLayout); // bindings 0-3
    globalResourceLayout
        // Global uniforms.
        .addUniformBufferBinding(VK_SHADER_STAGE_ALL) // 4
        // Point light buffer.
        .addStorageBufferBinding(VK_SHADER_STAGE_ALL) // 5
        // Shadow map texture.
        .addTextureBinding(VK_SHADER_STAGE_ALL) // 6
        // Texture heap.
        .addTextureHeapBinding(
            this->_textureHeap.getSize(),
            VK_SHADER_STAGE_ALL) // 7
        // Primitive constants heap.
        .addStorageBufferBinding(VK_SHADER_STAGE_ALL) // 8
        // Vertex buffer heap.
        .addBufferHeapBinding(
            this->_vertexBufferHeap.getSize(),
            VK_SHADER_STAGE_ALL) // 9
        // Index buffer heap.
        .addBufferHeapBinding(
            this->_indexBufferHeap.getSize(),
            VK_SHADER_STAGE_ALL); // 10

    this->_globalResources = PerFrameResources(app, globalResourceLayout);
    this->_globalUniforms = TransientUniforms<GlobalUniforms>(app);

    this->_pointLights = PointLightCollection(
        app,
        commandBuffer,
        9,
        true,
        this->_globalResources.getLayout(),
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

    ResourcesAssignment assignment = this->_globalResources.assign();

    // Bind IBL resources
    this->_iblResources.bind(assignment);

    // Bind global uniforms
    assignment.bindTransientUniforms(this->_globalUniforms);
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
}

void PathTracing::_createRayTracingPass(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {

  this->_accelerationStructure =
      AccelerationStructure(app, commandBuffer, this->_models);

  this->_giUniforms = TransientUniforms<GlobalIlluminationUniforms>(app);

  std::vector<StructuredBuffer<Probe>> bucketBuffers;
  uint32_t probeBufferCount = (PROBE_COUNT - 1) / PROBES_PER_BUFFER + 1;
  bucketBuffers.reserve(probeBufferCount);
  for (uint32_t i = 0; i < probeBufferCount; ++i) {
    bucketBuffers.emplace_back(app, PROBES_PER_BUFFER);
  }
  this->_probes = StructuredBufferHeap<Probe>(std::move(bucketBuffers));

  std::vector<StructuredBuffer<uint32_t>> hashBuffers;
  uint32_t hashBufferCount =
      (SPATIAL_HASH_SIZE - 1) / SPATIAL_HASH_SLOTS_PER_BUFFER + 1;
  hashBuffers.reserve(hashBufferCount);
  for (uint32_t i = 0; i < hashBufferCount; ++i) {
    hashBuffers.emplace_back(app, SPATIAL_HASH_SLOTS_PER_BUFFER);
  }
  this->_spatialHash = StructuredBufferHeap<uint32_t>(std::move(hashBuffers));

  this->_freeList = StructuredBuffer<FreeList>(app, 1);
  this->_freeList.setElement({0}, 0);
  this->_freeList.upload(app, commandBuffer);

  // this->_probes = StructuredBufferHeap<ProbeBucket>()

  //DescriptorSetLayoutBuilder rayTracingMaterialLayout{};
  // TODO: Use ray queries in deferred pass instead
  ImageOptions imageOptions{};
  imageOptions.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  imageOptions.width = app.getSwapChainExtent().width;
  imageOptions.height = app.getSwapChainExtent().height;
  imageOptions.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

  ImageViewOptions viewOptions{};
  viewOptions.format = imageOptions.format;

  SamplerOptions samplerOptions{};
  samplerOptions.minFilter = VK_FILTER_NEAREST;
  samplerOptions.magFilter = VK_FILTER_NEAREST;

  this->_rayTracingTarget[0].image = Image(app, imageOptions);
  this->_rayTracingTarget[0].view =
      ImageView(app, this->_rayTracingTarget[0].image, viewOptions);
  this->_rayTracingTarget[0].sampler = Sampler(app, samplerOptions);

  this->_rayTracingTarget[1].image = Image(app, imageOptions);
  this->_rayTracingTarget[1].view =
      ImageView(app, this->_rayTracingTarget[1].image, viewOptions);
  this->_rayTracingTarget[1].sampler = Sampler(app, samplerOptions);

  imageOptions.format = viewOptions.format = VK_FORMAT_R32_SFLOAT;
  this->_depthBuffer[0].image = Image(app, imageOptions);
  this->_depthBuffer[1].image = Image(app, imageOptions);
  this->_depthBuffer[0].view =
      ImageView(app, this->_depthBuffer[0].image, viewOptions);
  this->_depthBuffer[1].view =
      ImageView(app, this->_depthBuffer[1].image, viewOptions);
  this->_depthBuffer[0].sampler = Sampler(app, {});
  this->_depthBuffer[1].sampler = Sampler(app, {});

  imageOptions.format = viewOptions.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  imageOptions.width = imageOptions.height = 128;
  this->_debugTarget.image = Image(app, imageOptions);
  this->_debugTarget.view =
      ImageView(app, this->_debugTarget.image, viewOptions);
  this->_debugTarget.sampler = Sampler(app, {});

  // Material layout
  DescriptorSetLayoutBuilder matBuilder{};
  matBuilder.addAccelerationStructureBinding(VK_SHADER_STAGE_ALL);
  // prev accum buffer
  matBuilder.addTextureBinding(VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  // current accumulation buffer
  matBuilder.addStorageImageBinding(VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  // prev depth buffer
  matBuilder.addTextureBinding(VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  // depth buffer
  matBuilder.addStorageImageBinding(VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  // GI uniforms
  matBuilder.addUniformBufferBinding(VK_SHADER_STAGE_ALL);
  // Probe bucket buffers
  matBuilder.addBufferHeapBinding(probeBufferCount, VK_SHADER_STAGE_ALL);
  // Spatial hash buffers
  matBuilder.addBufferHeapBinding(hashBufferCount, VK_SHADER_STAGE_ALL);
  // Free list
  matBuilder.addStorageBufferBinding(VK_SHADER_STAGE_ALL);
  // Debug buffer
  matBuilder.addStorageImageBinding(VK_SHADER_STAGE_ALL);

  this->_pRayTracingMaterialAllocator =
      std::make_unique<DescriptorSetAllocator>(app, matBuilder, 2);
  this->_pRayTracingMaterial[0] =
      std::make_unique<Material>(app, *this->_pRayTracingMaterialAllocator);
  this->_pRayTracingMaterial[1] =
      std::make_unique<Material>(app, *this->_pRayTracingMaterialAllocator);

  this->_pRayTracingMaterial[0]
      ->assign()
      .bindAccelerationStructure(this->_accelerationStructure.getTLAS())
      .bindTexture(
          this->_rayTracingTarget[1].view,
          this->_rayTracingTarget[1].sampler)
      .bindStorageImage(
          this->_rayTracingTarget[0].view,
          this->_rayTracingTarget[0].sampler)
      .bindTexture(this->_depthBuffer[1].view, this->_depthBuffer[1].sampler)
      .bindStorageImage(
          this->_depthBuffer[0].view,
          this->_depthBuffer[0].sampler)
      .bindTransientUniforms(this->_giUniforms)
      .bindBufferHeap(this->_probes)
      .bindBufferHeap(this->_spatialHash)
      .bindStorageBuffer(
          this->_freeList.getAllocation(),
          this->_freeList.getSize(),
          false)
      .bindStorageImage(this->_debugTarget.view, this->_debugTarget.sampler);

  this->_pRayTracingMaterial[1]
      ->assign()
      .bindAccelerationStructure(this->_accelerationStructure.getTLAS())
      .bindTexture(
          this->_rayTracingTarget[0].view,
          this->_rayTracingTarget[0].sampler)
      .bindStorageImage(
          this->_rayTracingTarget[1].view,
          this->_rayTracingTarget[1].sampler)
      .bindTexture(this->_depthBuffer[0].view, this->_depthBuffer[0].sampler)
      .bindStorageImage(
          this->_depthBuffer[1].view,
          this->_depthBuffer[1].sampler)
      .bindTransientUniforms(this->_giUniforms)
      .bindBufferHeap(this->_probes)
      .bindBufferHeap(this->_spatialHash)
      .bindStorageBuffer(
          this->_freeList.getAllocation(),
          this->_freeList.getSize(),
          false)
      .bindStorageImage(this->_debugTarget.view, this->_debugTarget.sampler);

  ShaderDefines defs;

  RayTracingPipelineBuilder builder{};
  builder.setRayGenShader(
      GEngineDirectory + "/Shaders/PathTracing/PathTrace.glsl",
      defs);
  builder.addMissShader(
      GEngineDirectory + "/Shaders/PathTracing/PathTrace.miss.glsl",
      defs);
  builder.addClosestHitShader(
      GEngineDirectory + "/Shaders/PathTracing/PathTrace.chit.glsl",
      defs);
  builder.addClosestHitShader(
      GEngineDirectory + "/Shaders/PathTracing/DepthRay.chit.glsl",
      defs);

  builder.layoutBuilder.addDescriptorSet(this->_globalResources.getLayout())
      .addDescriptorSet(this->_pRayTracingMaterialAllocator->getLayout())
      .addPushConstants<uint32_t>(VK_SHADER_STAGE_ALL);

  this->_pRayTracingPipeline =
      std::make_unique<RayTracingPipeline>(app, std::move(builder));

  {
    ComputePipelineBuilder computeBuilder{};
    computeBuilder.layoutBuilder
        .addDescriptorSet(this->_globalResources.getLayout())
        .addDescriptorSet(this->_pRayTracingMaterialAllocator->getLayout())
        .addPushConstants<uint32_t>(VK_SHADER_STAGE_ALL);

    computeBuilder.setComputeShader(
        GEngineDirectory + "/Shaders/GlobalIllumination/UpdateProbe.comp.glsl",
        defs);

    this->_probePass = ComputePipeline(app, std::move(computeBuilder));
  }

  // Display Pass
  DescriptorSetLayoutBuilder displayPassMatLayout{};
  displayPassMatLayout.addTextureBinding();
  displayPassMatLayout.addTextureBinding();

  this->_pDisplayPassMaterialAllocator =
      std::make_unique<DescriptorSetAllocator>(app, displayPassMatLayout, 2);
  this->_pDisplayPassMaterial[0] =
      std::make_unique<Material>(app, *this->_pDisplayPassMaterialAllocator);
  this->_pDisplayPassMaterial[1] =
      std::make_unique<Material>(app, *this->_pDisplayPassMaterialAllocator);

  this->_pDisplayPassMaterial[0]
      ->assign()
      .bindTexture(this->_rayTracingTarget[0])
      .bindTexture(this->_debugTarget);
  this->_pDisplayPassMaterial[1]
      ->assign()
      .bindTexture(this->_rayTracingTarget[1])
      .bindTexture(this->_debugTarget);

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
            GEngineDirectory + "/Shaders/PathTracing/DisplayPass.frag")

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_globalResources.getLayout())
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

void PathTracing::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {
  VkDescriptorSet globalDescriptorSet =
      this->_globalResources.getCurrentDescriptorSet(frame);

  this->_pointLights.updateResource(frame);

  // Draw point light shadow maps
  // this->_pointLights.drawShadowMaps(
  //     app,
  //     commandBuffer,
  //     frame,
  //     this->_models,
  //     globalDescriptorSet);

  uint32_t readIndex = this->_targetIndex ^ 1;

  this->_rayTracingTarget[readIndex].image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
  this->_rayTracingTarget[this->_targetIndex].image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_GENERAL,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
  this->_debugTarget.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_GENERAL,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  this->_depthBuffer[readIndex].image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
  this->_depthBuffer[this->_targetIndex].image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_GENERAL,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

  // VkDescriptorSet rtDescSets = { globalDescriptorSet, this?}
  {
    VkDescriptorSet sets[2] = {
        globalDescriptorSet,
        this->_pRayTracingMaterial[this->_targetIndex]->getCurrentDescriptorSet(
            frame)};
    vkCmdBindPipeline(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        *this->_pRayTracingPipeline);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        this->_pRayTracingPipeline->getLayout(),
        0,
        2,
        sets,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        this->_pRayTracingPipeline->getLayout(),
        VK_SHADER_STAGE_ALL,
        0,
        sizeof(uint32_t),
        &this->_framesSinceCameraMoved);
    this->_pRayTracingPipeline->traceRays(
        app.getSwapChainExtent(),
        commandBuffer);

    vkCmdBindPipeline(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        this->_probePass.getPipeline());
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        this->_probePass.getLayout(),
        0,
        2,
        sets,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        this->_probePass.getLayout(),
        VK_SHADER_STAGE_ALL,
        0,
        sizeof(uint32_t),
        &this->_framesSinceCameraMoved);
    uint32_t groupCount = PROBE_COUNT / 32;
    vkCmdDispatch(commandBuffer, groupCount, 1, 1);
  }

  this->_rayTracingTarget[this->_targetIndex].image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  this->_debugTarget.image.transitionLayout(
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
      context.bindDescriptorSets(
          *this->_pDisplayPassMaterial[this->_targetIndex]);
      context.draw(3);
    }
  }

  this->_targetIndex ^= 1;
}
} // namespace PathTracing
} // namespace AltheaDemo