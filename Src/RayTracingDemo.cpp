#include "RayTracingDemo.h"

#include <Althea/Application.h>
#include <Althea/BufferUtilities.h>
#include <Althea/Camera.h>
#include <Althea/Cubemap.h>
#include <Althea/DefaultTextures.h>
#include <Althea/DescriptorSet.h>
#include <Althea/GraphicsPipeline.h>
#include <Althea/IndexBuffer.h>
#include <Althea/InputManager.h>
#include <Althea/InputMask.h>
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
  m_pCameraController = std::make_unique<CameraController>(
      app.getInputManager(),
      90.0f,
      (float)windowDims.width / (float)windowDims.height);
  m_pCameraController->setMaxSpeed(15.0f);

  // Recreate any stale pipelines (shader hot-reload)
  GInputManager->addKeyBinding(
      {GLFW_KEY_R, GLFW_PRESS, GLFW_MOD_CONTROL},
      [&app, this]() {
        m_rayTracingPipeline.tryRecompile(app);
        m_displayPass.tryRecompile(app);
      });
}

void RayTracingDemo::shutdownGame(Application& app) {
  m_pCameraController.reset();
}

void RayTracingDemo::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  m_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  SingleTimeCommandBuffer commandBuffer(app);
  _createGlobalResources(app, commandBuffer);
  _createRayTracingPass(app, commandBuffer);
}

void RayTracingDemo::destroyRenderState(Application& app) {
  m_models.clear();

  m_rayTracingPipeline = {};
  m_displayPass = {};
  m_displayPassSwapChainFrameBuffers = {};

  m_globalResources = {};
  m_globalUniforms = {};
  m_heap = {};
}

static bool s_bIsCameraFrozen = true;
static uint32_t s_lastInputMask = 0;

void RayTracingDemo::tick(Application& app, const FrameContext& frame) {
  uint32_t inputMask = GInputManager->getCurrentInputMask();
  uint32_t changedInputs = inputMask ^ s_lastInputMask;
  if (changedInputs & INPUT_BIT_SPACE) {
    s_bIsCameraFrozen = !(inputMask & INPUT_BIT_SPACE);
  }
  s_lastInputMask = inputMask;

  if (s_bIsCameraFrozen) {
    m_pCameraController->setMouseDisabled();
  } else {
    m_pCameraController->setMouseEnabled();
    m_pCameraController->tick(frame.deltaTime);
  }

  const Camera& camera = m_pCameraController->getCamera();

  const glm::mat4& projection = camera.getProjection();

  GlobalUniforms globalUniforms;
  globalUniforms.projection = camera.getProjection();
  globalUniforms.inverseProjection = glm::inverse(globalUniforms.projection);
  globalUniforms.view = camera.computeView();
  globalUniforms.inverseView = glm::inverse(globalUniforms.view);
  globalUniforms.lightCount = 0;
  globalUniforms.time = static_cast<float>(frame.currentTime);
  globalUniforms.exposure = 0.3f;
  // TODO: need to retain this in the engine
  static uint32_t frameCount = 0;
  globalUniforms.frameCount = frameCount++;
  m_globalUniforms.getCurrentUniformBuffer(frame).updateUniforms(
      globalUniforms);
}

void RayTracingDemo::_createModels(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {

  m_models.emplace_back(
      app,
      commandBuffer,
      m_heap,
      GEngineDirectory + "/Content/Models/DamagedHelmet.glb",
      glm::scale(
          glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -10.0f)),
          glm::vec3(4.0f)));

  // m_models.emplace_back(
  //     app,
  //     commandBuffer,
  //     GEngineDirectory + "/Content/Models/FlightHelmet/FlightHelmet.gltf");
  // m_models.back().setModelTransform(glm::scale(
  //     glm::translate(glm::mat4(1.0f), glm::vec3(50.0f, -1.0f, 0.0f)),
  //     glm::vec3(8.0f)));

  // m_models.emplace_back(
  //     app,
  //     commandBuffer,
  //     GEngineDirectory + "/Content/Models/MetalRoughSpheres.glb");
  // m_models.back().setModelTransform(glm::scale(
  //     glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)),
  //     glm::vec3(4.0f)));

  /*m_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/Sponza/glTF/Sponza.gltf");
  m_models.back().setModelTransform(glm::translate(
      glm::scale(glm::mat4(1.0f), glm::vec3(10.0f)),
      glm::vec3(10.0f, -1.0f, 0.0f)));*/
}

void RayTracingDemo::_createGlobalResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  m_heap = GlobalHeap(app);
  AltheaEngine::registerDefaultTexturesToHeap(m_heap);

  _createModels(app, commandBuffer);

  m_globalUniforms = GlobalUniformsResource(app, m_heap);

  RayTracingResourcesBuilder rtResourceBuilder{};
  rtResourceBuilder.models = &m_models;

  GlobalResourcesBuilder resourceBuilder{};
  sprintf(resourceBuilder.environmentMapName, "NeoclassicalInterior");
  resourceBuilder.rayTracing = &rtResourceBuilder;

  m_globalResources =
      GlobalResources(app, commandBuffer, m_heap, resourceBuilder);
}

void RayTracingDemo::_createRayTracingPass(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {

  ShaderDefines defs;

  RayTracingPipelineBuilder builder{};
  builder.setRayGenShader(
      GEngineDirectory + "/Shaders/SimpleRT/SimpleRT.rgen.glsl",
      defs);
  builder.addMissShader(
      GEngineDirectory + "/Shaders/SimpleRT/SimpleRT.miss.glsl",
      defs);
  builder.addClosestHitShader(
      GEngineDirectory + "/Shaders/SimpleRT/SimpleRT.chit.glsl",
      defs);

  builder.layoutBuilder.addDescriptorSet(m_heap.getDescriptorSetLayout())
      .addPushConstants<RtPush>();

  m_rayTracingPipeline = RayTracingPipeline(app, std::move(builder));

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
            GEngineDirectory + "/Shaders/SimpleRT/DisplayPass.vert")
        // Fragment shader
        .addFragmentShader(
            GEngineDirectory + "/Shaders/SimpleRT/DisplayPass.frag")

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(m_heap.getDescriptorSetLayout())
        .addPushConstants<RtPush>();
  }

  m_displayPass = RenderPass(
      app,
      app.getSwapChainExtent(),
      std::move(attachments),
      std::move(subpassBuilders));

  m_displayPassSwapChainFrameBuffers =
      SwapChainFrameBufferCollection(app, m_displayPass, {});
}

void RayTracingDemo::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {

  VkDescriptorSet heapSet = m_heap.getDescriptorSet();

  m_globalResources.getRayTracingResources().transitionToTarget(commandBuffer);

  RtPush push{};
  push.globalResourcesHandle = m_globalResources.getHandle().index;
  push.globalUniformsHandle =
      m_globalUniforms.getCurrentBindlessHandle(frame).index;
  static uint32_t s_accumulatedFrames = 0;
  if (!s_bIsCameraFrozen)
    s_accumulatedFrames = 0;
  push.accumulatedFramesCount = s_accumulatedFrames++;

  m_rayTracingPipeline.bindPipeline(commandBuffer);
  m_rayTracingPipeline.setPushConstants(commandBuffer, push);
  m_rayTracingPipeline.bindDescriptorSet(commandBuffer, heapSet);
  m_rayTracingPipeline.traceRays(app.getSwapChainExtent(), commandBuffer);

  m_globalResources.getRayTracingResources().transitionToTexture(commandBuffer);

  // Display pass
  {
    ActiveRenderPass pass = m_displayPass.begin(
        app,
        commandBuffer,
        frame,
        m_displayPassSwapChainFrameBuffers.getCurrentFrameBuffer(frame));
    // Bind global descriptor sets
    pass.setGlobalDescriptorSets(gsl::span(&heapSet, 1));

    {
      const DrawContext& context = pass.getDrawContext();
      context.bindDescriptorSets();
      context.updatePushConstants(push, 0);
      context.draw(3);
    }
  }
}
} // namespace RayTracingDemo
} // namespace AltheaDemo