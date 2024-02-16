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

namespace {
struct RTPush {
  uint32_t globalResourcesHandle;
  uint32_t globalUniformsHandle;
  uint32_t tlasHandle;

  uint32_t prevImgHandle;
  uint32_t imgHandle;
  uint32_t prevDepthBufferHandle;
  uint32_t depthBufferHandle;

  uint32_t framesSinceCameraMoved;
};

struct ProbePush {
  uint32_t tmp;
};

struct DisplayPush {
  uint32_t globalUniformsHandle;
  uint32_t imgHandle;
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

        that->m_rtPass.tryRecompile(app);
        // that->m_probePass.tryRecompile(app);
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
  createRayTracingPass(app, commandBuffer);
}

void PathTracing::destroyRenderState(Application& app) {
  m_models.clear();
  Primitive::resetPrimitiveIndexCount();

  m_accelerationStructure = {};
  m_rtTarget[0] = {};
  m_rtTarget[1] = {};
  m_depthBuffer[0] = {};
  m_depthBuffer[1] = {};
  m_debugTarget = {};
  m_giUniforms = {};
  m_probes = {};
  m_spatialHash = {};
  m_freeList = {};

  m_rtPass = {};
  m_probePass = {};
  m_displayPass = {};
  m_displayPassSwapChainFrameBuffers = {};

  m_globalResources = {};
  m_globalUniforms = {};
  m_pointLights = {};
  m_primitiveConstantsBuffer = {};

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

  GlobalIlluminationUniforms giUniforms{};
  giUniforms.probeCount = PROBE_COUNT;
  giUniforms.probesPerBuffer = PROBES_PER_BUFFER;
  giUniforms.spatialHashSize = SPATIAL_HASH_SIZE;
  giUniforms.spatialHashSlotsPerBuffer = SPATIAL_HASH_SLOTS_PER_BUFFER;

  giUniforms.gridCellSize = 0.1f;

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
}

void PathTracing::createRayTracingPass(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  m_giUniforms = TransientUniforms<GlobalIlluminationUniforms>(app);
  m_giUniforms.registerToHeap(m_heap);

  // std::vector<StructuredBuffer<Probe>> bucketBuffers;
  // uint32_t probeBufferCount = (PROBE_COUNT - 1) / PROBES_PER_BUFFER + 1;
  // bucketBuffers.reserve(probeBufferCount);
  // for (uint32_t i = 0; i < probeBufferCount; ++i) {
  //   bucketBuffers.emplace_back(app, PROBES_PER_BUFFER);
  // }
  // m_probes = StructuredBufferHeap<Probe>(std::move(bucketBuffers));
  // m_probes.registerToHeap(m_heap);

  // std::vector<StructuredBuffer<uint32_t>> hashBuffers;
  // uint32_t hashBufferCount =
  //     (SPATIAL_HASH_SIZE - 1) / SPATIAL_HASH_SLOTS_PER_BUFFER + 1;
  // hashBuffers.reserve(hashBufferCount);
  // for (uint32_t i = 0; i < hashBufferCount; ++i) {
  //   hashBuffers.emplace_back(app, SPATIAL_HASH_SLOTS_PER_BUFFER);
  // }
  // m_spatialHash = StructuredBufferHeap<uint32_t>(std::move(hashBuffers));

  // m_freeList = StructuredBuffer<FreeList>(app, 1);
  // m_freeList.setElement({0}, 0);
  // m_freeList.upload(app, commandBuffer);

  // m_probes = StructuredBufferHeap<ProbeBucket>()

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

  m_rtTarget[0].image = Image(app, imageOptions);
  m_rtTarget[0].view = ImageView(app, m_rtTarget[0].image, viewOptions);
  m_rtTarget[0].sampler = Sampler(app, samplerOptions);
  m_rtTargetHandle[0] = m_heap.registerTexture();
  m_heap.updateStorageImage(
      m_rtTargetHandle[0],
      m_rtTarget[0].view,
      m_rtTarget[0].sampler);

  m_rtTarget[1].image = Image(app, imageOptions);
  m_rtTarget[1].view = ImageView(app, m_rtTarget[1].image, viewOptions);
  m_rtTarget[1].sampler = Sampler(app, samplerOptions);
  m_rtTargetHandle[1] = m_heap.registerTexture();
  m_heap.updateStorageImage(
      m_rtTargetHandle[1],
      m_rtTarget[1].view,
      m_rtTarget[1].sampler);

  imageOptions.format = viewOptions.format = VK_FORMAT_R32_SFLOAT;
  m_depthBuffer[0].image = Image(app, imageOptions);
  m_depthBuffer[0].view = ImageView(app, m_depthBuffer[0].image, viewOptions);
  m_depthBuffer[0].sampler = Sampler(app, {});
  m_depthBufferHandle[0] = m_heap.registerTexture();
  m_heap.updateStorageImage(
      m_depthBufferHandle[0],
      m_depthBuffer[0].view,
      m_depthBuffer[0].sampler);

  m_depthBuffer[1].image = Image(app, imageOptions);
  m_depthBuffer[1].view = ImageView(app, m_depthBuffer[1].image, viewOptions);
  m_depthBuffer[1].sampler = Sampler(app, {});
  m_depthBufferHandle[1] = m_heap.registerTexture();
  m_heap.updateStorageImage(
      m_depthBufferHandle[1],
      m_depthBuffer[1].view,
      m_depthBuffer[1].sampler);

  imageOptions.format = viewOptions.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  imageOptions.width = imageOptions.height = 128;
  m_debugTarget.image = Image(app, imageOptions);
  m_debugTarget.view = ImageView(app, m_debugTarget.image, viewOptions);
  m_debugTarget.sampler = Sampler(app, {});

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

  builder.layoutBuilder.addDescriptorSet(m_heap.getDescriptorSetLayout())
      .addPushConstants<RTPush>(VK_SHADER_STAGE_ALL);

  m_rtPass = RayTracingPipeline(app, std::move(builder));

  {
    ComputePipelineBuilder computeBuilder{};
    computeBuilder.layoutBuilder
        .addDescriptorSet(m_heap.getDescriptorSetLayout())
        .addPushConstants<uint32_t>(VK_SHADER_STAGE_ALL);

    computeBuilder.setComputeShader(
        GEngineDirectory + "/Shaders/GlobalIllumination/UpdateProbe.comp.glsl",
        defs);

    // m_probePass = ComputePipeline(app, std::move(computeBuilder));
  }

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
        .addPushConstants<DisplayPush>(VK_SHADER_STAGE_ALL);
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

  // Draw point light shadow maps
  // m_pointLights.drawShadowMaps(
  //     app,
  //     commandBuffer,
  //     frame,
  //     m_models,
  //     globalDescriptorSet);

  uint32_t readIndex = m_targetIndex ^ 1;

  m_rtTarget[readIndex].image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
  m_rtTarget[m_targetIndex].image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_GENERAL,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
  m_debugTarget.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_GENERAL,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

  m_depthBuffer[readIndex].image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
  m_depthBuffer[m_targetIndex].image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_GENERAL,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

  // VkDescriptorSet rtDescSets = { globalDescriptorSet, this?}
  {
    RTPush push{};
    push.globalResourcesHandle = m_globalResources.getHandle().index;
    push.globalUniformsHandle =
        m_globalUniforms.getCurrentBindlessHandle(frame).index;

    push.tlasHandle = m_accelerationStructure.getTlasHandle().index;
    push.prevImgHandle = m_rtTargetHandle[readIndex].index;
    push.imgHandle = m_rtTargetHandle[m_targetIndex].index;
    push.prevDepthBufferHandle = m_depthBufferHandle[readIndex].index;
    push.depthBufferHandle = m_depthBufferHandle[m_targetIndex].index;

    push.framesSinceCameraMoved = m_framesSinceCameraMoved;

    vkCmdBindPipeline(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        m_rtPass);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        m_rtPass.getLayout(),
        0,
        1,
        &heapSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        m_rtPass.getLayout(),
        VK_SHADER_STAGE_ALL,
        0,
        sizeof(RTPush),
        &push);
    m_rtPass.traceRays(app.getSwapChainExtent(), commandBuffer);
  }

#if 0 
  {
    vkCmdBindPipeline(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        m_probePass.getPipeline());
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        m_probePass.getLayout(),
        0,
        1,
        &heapSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        m_probePass.getLayout(),
        VK_SHADER_STAGE_ALL,
        0,
        sizeof(uint32_t),
        &m_framesSinceCameraMoved);
    uint32_t groupCount = PROBE_COUNT / 32;
    vkCmdDispatch(commandBuffer, groupCount, 1, 1);
  }
#endif

  m_rtTarget[m_targetIndex].image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  m_debugTarget.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  // Display pass
  {
    DisplayPush push{};
    push.globalUniformsHandle =
        m_globalUniforms.getCurrentBindlessHandle(frame).index;
    push.imgHandle = m_rtTargetHandle[m_targetIndex].index;

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
} // namespace PathTracing
} // namespace AltheaDemo