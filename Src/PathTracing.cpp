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

#define RESERVOIR_COUNT_PER_BUFFER 16383

namespace AltheaDemo {
namespace PathTracing {

namespace {
struct GBufferPush {
  glm::mat4 transform;
  uint32_t primitiveIdx;
  uint32_t globalResourcesHandle;
  uint32_t globalUniformsHandle;
};

struct RTPush {
  uint32_t globalResourcesHandle;
  uint32_t globalUniformsHandle;
  uint32_t giUniformsHandle;
};
} // namespace

PathTracing::PathTracing() {}

void PathTracing::initGame(Application& app) {
  const VkExtent2D& windowDims = app.getSwapChainExtent();
  m_pCameraController = std::make_unique<CameraController>(
      app.getInputManager(),
      90.0f,
      (float)windowDims.width / (float)windowDims.height);
  m_pCameraController->setMaxSpeed(15.0f);
  m_pCameraController->getCamera().setPosition(glm::vec3(2.0f, 2.0f, 2.0f));

  // TODO: need to unbind these at shutdown
  InputManager& input = app.getInputManager();
  input.addKeyBinding(
      {GLFW_KEY_L, GLFW_PRESS, 0},
      [&adjustingExposure = m_adjustingExposure, &input]() {
        adjustingExposure = true;
        input.setMouseCursorHidden(false);
      });

  input.addKeyBinding(
      {GLFW_KEY_L, GLFW_RELEASE, 0},
      [&adjustingExposure = m_adjustingExposure, &input]() {
        adjustingExposure = false;
        input.setMouseCursorHidden(true);
      });

  // Recreate any stale pipelines (shader hot-reload)
  input.addKeyBinding(
      {GLFW_KEY_R, GLFW_PRESS, GLFW_MOD_CONTROL},
      [&app, that = this]() {
        that->m_framesSinceCameraMoved = 0;

        that->m_gBufferPass.tryRecompile(app);
        that->m_directSamplingPass.tryRecompile(app);
        that->m_spatialResamplingPass.tryRecompile(app);
        that->m_pointLights.getShadowMapPass().tryRecompile(app);
        that->m_displayPass.tryRecompile(app);
      });

  input.addKeyBinding(
      {GLFW_KEY_F, GLFW_PRESS, 0},
      [&freezeCamera = m_freezeCamera]() { freezeCamera = false; });
  input.addKeyBinding(
      {GLFW_KEY_F, GLFW_RELEASE, 0},
      [&freezeCamera = m_freezeCamera]() { freezeCamera = true; });
  input.addMousePositionCallback(
      [&adjustingExposure = m_adjustingExposure,
       &exposure = m_exposure](double x, double y, bool cursorHidden) {
        if (adjustingExposure) {
          exposure = static_cast<float>(y);
        }
      });
}

void PathTracing::shutdownGame(Application& app) {
  m_pCameraController.reset();
}

void PathTracing::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  m_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  SingleTimeCommandBuffer commandBuffer(app);
  createGlobalResources(app, commandBuffer);
  createGBufferPass(app, commandBuffer);
  createSamplingPasses(app, commandBuffer);
}

void PathTracing::destroyRenderState(Application& app) {
  m_models.clear();
  Primitive::resetPrimitiveIndexCount();

  m_accelerationStructure = {};
  m_rtTargets[0] = {};
  m_rtTargets[1] = {};
  m_giUniforms = {};

  m_gBufferPass = {};
  m_gBufferFrameBuffer = {};
  m_directSamplingPass = {};
  m_spatialResamplingPass = {};
  m_displayPass = {};
  m_displayPassSwapChainFrameBuffers = {};

  m_globalResources = {};
  m_globalUniforms = {};
  m_pointLights = {};
  m_primitiveConstantsBuffer = {};

  m_reservoirHeap.clear();

  m_heap = {};
}

void PathTracing::tick(Application& app, const FrameContext& frame) {
  const Camera& camera = m_pCameraController->getCamera();

  GlobalUniforms globalUniforms;
  globalUniforms.prevView = camera.computeView();
  globalUniforms.prevInverseView = glm::inverse(globalUniforms.prevView);

  // if (m_freezeCamera) {
  //   ++m_framesSinceCameraMoved;
  // } else {
  // m_framesSinceCameraMoved = 0;
  m_framesSinceCameraMoved++; // TODO: Clean this up..
  m_pCameraController->tick(frame.deltaTime);
  // }

  uint32_t inputMask = app.getInputManager().getCurrentInputMask();

  globalUniforms.projection = camera.getProjection();
  globalUniforms.inverseProjection = glm::inverse(globalUniforms.projection);
  globalUniforms.view = camera.computeView();
  globalUniforms.inverseView = glm::inverse(globalUniforms.view);
  globalUniforms.lightCount = static_cast<int>(m_pointLights.getCount());
  globalUniforms.time = static_cast<float>(frame.currentTime);
  globalUniforms.exposure = m_exposure;

  globalUniforms.inputMask = inputMask;

  InputManager::MousePos mPos = app.getInputManager().getCurrentMousePos();
  VkExtent2D extent = app.getSwapChainExtent();
  glm::vec2 mouseUV(
      static_cast<float>(mPos.x / extent.width),
      static_cast<float>(mPos.y / extent.height));

  globalUniforms.mouseUV = mouseUV;

  globalUniforms.lightBufferHandle =
      m_pointLights.getCurrentLightBufferHandle(frame).index;
  globalUniforms.lightCount = m_pointLights.getCount();

  m_globalUniforms.getCurrentUniformBuffer(frame).updateUniforms(
      globalUniforms);

  // TODO: Allow lights to move again :)
  // for (uint32_t i = 0; i < m_pointLights.getCount(); ++i) {
  //   PointLight light = m_pointLights.getLight(i);

  //   light.position = 40.0f * glm::vec3(
  //                                static_cast<float>(i / 3),
  //                                -0.1f,
  //                                (static_cast<float>(i % 3) - 1.5f) * 0.5f);

  //   light.position.x += 5.5f * cos(1.5f * frame.currentTime + i);
  //   light.position.z += 5.5f * sin(1.5 * frame.currentTime + i);

  //   m_pointLights.setLight(i, light);
  // }

  m_pointLights.updateResource(frame);

  uint32_t readIndex = m_targetIndex ^ 1;

  GlobalIlluminationUniforms giUniforms{};

  giUniforms.tlas = m_accelerationStructure.getTlasHandle().index;

  giUniforms.colorSamplers[0] = m_rtTargets[0].targetTextureHandle.index;
  giUniforms.colorSamplers[1] = m_rtTargets[1].targetTextureHandle.index;

  giUniforms.colorTargets[0] = m_rtTargets[0].targetImageHandle.index;
  giUniforms.colorTargets[1] = m_rtTargets[1].targetImageHandle.index;

  giUniforms.targetWidth = app.getSwapChainExtent().width;
  giUniforms.targetHeight = app.getSwapChainExtent().height;

  giUniforms.writeIndex = m_targetIndex;

  giUniforms.reservoirHeap = m_reservoirHeap[0].getHandle().index;
  giUniforms.reservoirsPerBuffer = RESERVOIR_COUNT_PER_BUFFER;

  giUniforms.framesSinceCameraMoved = m_framesSinceCameraMoved;

  m_giUniforms.updateUniforms(giUniforms, frame);
}

void PathTracing::createModels(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {

  m_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/DamagedHelmet.glb");
  m_models.back().setModelTransform(glm::scale(
      glm::translate(glm::mat4(1.0f), glm::vec3(36.0f, 0.0f, 0.0f)),
      glm::vec3(4.0f)));

  m_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/FlightHelmet/FlightHelmet.gltf");
  m_models.back().setModelTransform(glm::scale(
      glm::translate(glm::mat4(1.0f), glm::vec3(50.0f, -1.0f, 0.0f)),
      glm::vec3(8.0f)));

  m_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/MetalRoughSpheres.glb");
  m_models.back().setModelTransform(glm::scale(
      glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)),
      glm::vec3(4.0f)));

  m_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/Sponza/glTF/Sponza.gltf");
  m_models.back().setModelTransform(glm::translate(
      glm::scale(glm::mat4(1.0f), glm::vec3(10.0f)),
      glm::vec3(0.0f, -8.0f, 0.0f)));
}

void PathTracing::createGlobalResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {

  m_heap = GlobalHeap(app);

  // Create GLTF resource heaps
  {
    createModels(app, commandBuffer);

    for (Model& model : m_models)
      model.registerToHeap(m_heap);

    uint32_t primCount = 0;
    for (const Model& model : m_models)
      primCount += static_cast<uint32_t>(model.getPrimitivesCount());

    m_primitiveConstantsBuffer =
        StructuredBuffer<PrimitiveConstants>(app, primCount);

    for (const Model& model : m_models) {
      for (const Primitive& primitive : model.getPrimitives()) {
        m_primitiveConstantsBuffer.setElement(
            primitive.getConstants(),
            primitive.getPrimitiveIndex());
      }
    }

    m_primitiveConstantsBuffer.upload(app, commandBuffer);
    m_primitiveConstantsBuffer.registerToHeap(m_heap);
  }

  // Create acceleration structure for models
  m_accelerationStructure = AccelerationStructure(app, commandBuffer, m_models);
  m_accelerationStructure.registerToHeap(m_heap);

  {
    m_pointLights = PointLightCollection(
        app,
        commandBuffer,
        m_heap,
        9,
        true,
        m_primitiveConstantsBuffer.getHandle());
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

        m_pointLights.setLight(i * 3 + j, light);
      }
    }
  }

  m_globalResources = GlobalResources(
      app,
      commandBuffer,
      m_heap,
      m_pointLights.getShadowMapHandle(),
      m_primitiveConstantsBuffer.getHandle());
  m_globalUniforms = GlobalUniformsResource(app, m_heap);

  // TODO: Make this buffer smaller...
  VkExtent2D extent = app.getSwapChainExtent();

  {
    uint32_t reservoirCount = 2 * extent.width * extent.height;
    uint32_t bufferCount =
        (reservoirCount - 1) / RESERVOIR_COUNT_PER_BUFFER + 1;

    m_reservoirHeap.reserve(bufferCount);

    for (uint32_t bufferIdx = 0; bufferIdx < bufferCount; ++bufferIdx) {
      auto& buffer =
          m_reservoirHeap.emplace_back(app, RESERVOIR_COUNT_PER_BUFFER);
      buffer.zeroBuffer(commandBuffer);
      // These buffers are registered sequentially, we use this assumption in
      // the shader
      buffer.registerToHeap(m_heap);
    }
  }
}

void PathTracing::createGBufferPass(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {

  std::vector<SubpassBuilder> builders;
  {
    SubpassBuilder& builder = builders.emplace_back();
    GBufferResources::setupAttachments(builder);

    Primitive::buildPipeline(builder.pipelineBuilder);

    ShaderDefines defs;
    defs.emplace("BINDLESS_SET", "0");

    builder
        .pipelineBuilder
        // Vertex shader
        .addVertexShader(
            GEngineDirectory + "/Shaders/GltfForwardBindless.vert",
            defs)
        // Fragment shader
        .addFragmentShader(
            GEngineDirectory + "/Shaders/GltfForwardBindless.frag",
            defs)

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->m_heap.getDescriptorSetLayout())
        .addPushConstants<GBufferPush>(VK_SHADER_STAGE_ALL);
  }

  const GBufferResources& gBuffer = m_globalResources.getGBuffer();
  std::vector<Attachment> attachments = gBuffer.getAttachmentDescriptions();

  const VkExtent2D& extent = app.getSwapChainExtent();
  this->m_gBufferPass =
      RenderPass(app, extent, std::move(attachments), std::move(builders));

  this->m_gBufferFrameBuffer = FrameBuffer(
      app,
      this->m_gBufferPass,
      extent,
      gBuffer.getAttachmentViews());
}

void PathTracing::createSamplingPasses(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  m_giUniforms = TransientUniforms<GlobalIlluminationUniforms>(app);
  m_giUniforms.registerToHeap(m_heap);

  for (int i = 0; i < 2; ++i) {
    ImageOptions imageOptions{};
    imageOptions.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    imageOptions.width = app.getSwapChainExtent().width;
    imageOptions.height = app.getSwapChainExtent().height;
    imageOptions.usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    ImageViewOptions viewOptions{};
    viewOptions.format = imageOptions.format;

    SamplerOptions samplerOptions{};
    samplerOptions.minFilter = VK_FILTER_NEAREST;
    samplerOptions.magFilter = VK_FILTER_NEAREST;

    RtTarget& rtTarget = m_rtTargets[i];
    rtTarget.target.image = Image(app, imageOptions);
    rtTarget.target.view = ImageView(app, rtTarget.target.image, viewOptions);
    rtTarget.target.sampler = Sampler(app, samplerOptions);

    rtTarget.targetImageHandle = m_heap.registerImage();
    m_heap.updateStorageImage(
        rtTarget.targetImageHandle,
        rtTarget.target.view,
        rtTarget.target.sampler);

    rtTarget.targetTextureHandle = m_heap.registerTexture();
    m_heap.updateTexture(
        rtTarget.targetTextureHandle,
        rtTarget.target.view,
        rtTarget.target.sampler);
  }

  ShaderDefines defs;
  RayTracingPipelineBuilder builder{};
  builder.setRayGenShader(
      GEngineDirectory + "/Shaders/PathTracing/DirectSampling.rgen.glsl",
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

  builder.layoutBuilder.addDescriptorSet(m_heap.getDescriptorSetLayout())
      .addPushConstants<RTPush>(VK_SHADER_STAGE_ALL);

  m_directSamplingPass =
      RayTracingPipeline(app, RayTracingPipelineBuilder(builder));

  builder.setRayGenShader(
      GEngineDirectory + "/Shaders/PathTracing/SpatialResampling.rgen.glsl",
      defs);

  m_spatialResamplingPass =
      RayTracingPipeline(app, RayTracingPipelineBuilder(builder));

  // Display Pass
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
        .addVertexShader(
            GEngineDirectory + "/Shaders/PathTracing/DisplayPass.vert")
        // Fragment shader
        .addFragmentShader(
            GEngineDirectory + "/Shaders/PathTracing/DisplayPass.frag")

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(m_heap.getDescriptorSetLayout())
        .addPushConstants<RTPush>(VK_SHADER_STAGE_ALL);
  }

  m_displayPass = RenderPass(
      app,
      app.getSwapChainExtent(),
      std::move(attachments),
      std::move(subpassBuilders));

  m_displayPassSwapChainFrameBuffers =
      SwapChainFrameBufferCollection(app, m_displayPass, {});
}

void PathTracing::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {
  VkDescriptorSet heapSet = m_heap.getDescriptorSet();

  m_pointLights.updateResource(frame);

  uint32_t readIndex = m_targetIndex ^ 1;

  m_globalResources.getGBuffer().transitionToAttachment(commandBuffer);

  {
    GBufferPush push{};
    push.globalResourcesHandle = m_globalResources.getHandle().index;
    push.globalUniformsHandle =
        m_globalUniforms.getCurrentBindlessHandle(frame).index;

    ActiveRenderPass draw =
        m_gBufferPass.begin(app, commandBuffer, frame, m_gBufferFrameBuffer);
    draw.setGlobalDescriptorSets(gsl::span(&heapSet, 1));

    const DrawContext& context = draw.getDrawContext();
    for (const Model& model : m_models) {
      for (const Primitive& primitive : model.getPrimitives()) {
        push.transform = primitive.computeWorldTransform();
        push.primitiveIdx = primitive.getPrimitiveIndex();

        context.updatePushConstants(push, 0);

        context.setFrontFaceDynamic(primitive.getFrontFace());
        context.bindDescriptorSets();
        context.drawIndexed(
            primitive.getVertexBuffer(),
            primitive.getIndexBuffer());
      }
    }
  }

  m_globalResources.getGBuffer().transitionToTextures(commandBuffer);

  m_rtTargets[readIndex].target.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
  m_rtTargets[m_targetIndex].target.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_GENERAL,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

  // Direct Sampling
  {
    RTPush push{};
    push.globalResourcesHandle = m_globalResources.getHandle().index;
    push.globalUniformsHandle =
        m_globalUniforms.getCurrentBindlessHandle(frame).index;
    push.giUniformsHandle = m_giUniforms.getCurrentHandle(frame).index;

    vkCmdBindPipeline(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        m_directSamplingPass);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        m_directSamplingPass.getLayout(),
        0,
        1,
        &heapSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        m_directSamplingPass.getLayout(),
        VK_SHADER_STAGE_ALL,
        0,
        sizeof(RTPush),
        &push);
    m_directSamplingPass.traceRays(app.getSwapChainExtent(), commandBuffer);
  }

  reservoirBarrier(commandBuffer);

#if 0
  // Spatial Resampling
  {
    RTPush push{};
    push.globalResourcesHandle = m_globalResources.getHandle().index;
    push.globalUniformsHandle =
        m_globalUniforms.getCurrentBindlessHandle(frame).index;
    push.giUniformsHandle = m_giUniforms.getCurrentHandle(frame).index;

    vkCmdBindPipeline(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        m_spatialResamplingPass);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        m_spatialResamplingPass.getLayout(),
        0,
        1,
        &heapSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        m_spatialResamplingPass.getLayout(),
        VK_SHADER_STAGE_ALL,
        0,
        sizeof(RTPush),
        &push);
    m_spatialResamplingPass.traceRays(app.getSwapChainExtent(), commandBuffer);
  }

  reservoirBarrier(commandBuffer);
#endif
  // Display pass
  {
    RTPush push{};
    push.globalResourcesHandle = m_globalResources.getHandle().index;
    push.globalUniformsHandle =
        m_globalUniforms.getCurrentBindlessHandle(frame).index;
    push.giUniformsHandle = m_giUniforms.getCurrentHandle(frame).index;

    ActiveRenderPass pass = m_displayPass.begin(
        app,
        commandBuffer,
        frame,
        m_displayPassSwapChainFrameBuffers.getCurrentFrameBuffer(frame));
    // Bind global descriptor sets
    pass.setGlobalDescriptorSets(gsl::span(&heapSet, 1));

    {
      const DrawContext& context = pass.getDrawContext();
      context.updatePushConstants(push, 0);
      context.bindDescriptorSets();
      context.draw(3);
    }
  }

  m_targetIndex ^= 1;
}

void PathTracing::reservoirBarrier(VkCommandBuffer commandBuffer) {
  for (auto& buffer : m_reservoirHeap) {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.buffer = buffer.getAllocation().getBuffer();
    barrier.offset = 0;
    barrier.size = buffer.getSize();
    barrier.srcAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        nullptr,
        1,
        &barrier,
        0,
        nullptr);
  }
}
} // namespace PathTracing
} // namespace AltheaDemo