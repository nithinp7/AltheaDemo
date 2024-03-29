#include "DiffuseProbes.h"

#include <Althea/Application.h>
#include <Althea/BufferUtilities.h>
#include <Althea/Camera.h>
#include <Althea/Cubemap.h>
#include <Althea/DescriptorSet.h>
#include <Althea/GraphicsPipeline.h>
#include <Althea/Gui.h>
#include <Althea/IndexBuffer.h>
#include <Althea/InputManager.h>
#include <Althea/InputMask.h>
#include <Althea/ModelViewProjection.h>
#include <Althea/Primitive.h>
#include <Althea/ShapeUtilities.h>
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

// TODO: Eventually need to page the probes
#define PROBE_COUNT 16383

namespace AltheaDemo {
namespace DiffuseProbes {

namespace {
struct GBufferPush {
  glm::mat4 transform;
  uint32_t primitiveIdx;
  uint32_t globalResourcesHandle;
  uint32_t globalUniformsHandle;
};

// TODO: Move this into Althea/Shared/GlobalIllumination.glsl"
struct RTPush {
  uint32_t globalResourcesHandle;
  uint32_t globalUniformsHandle;
  uint32_t giUniformsHandle;
};
} // namespace

DiffuseProbes::DiffuseProbes() {}

void DiffuseProbes::initGame(Application& app) {
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
        that->m_frameNumber = 0;

        that->m_gBufferPass.tryRecompile(app);
        that->m_directSamplingPass.tryRecompile(app);
        that->m_spatialResamplingPass.tryRecompile(app);
        that->m_displayPass.tryRecompile(app);
        that->m_compositingPass.tryRecompile(app);
        that->m_probePlacementPass.tryRecompile(app);
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

void DiffuseProbes::shutdownGame(Application& app) {
  m_pCameraController.reset();
}

void DiffuseProbes::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  m_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  Gui::createRenderState(app);

  SingleTimeCommandBuffer commandBuffer(app);
  createGlobalResources(app, commandBuffer);
  createGBufferPass(app, commandBuffer);
  createSamplingPasses(app, commandBuffer);
  createProbeResources(app, commandBuffer);
}

void DiffuseProbes::destroyRenderState(Application& app) {
  m_models.clear();
  Primitive::resetPrimitiveIndexCount();

  Gui::destroyRenderState(app);

  m_accelerationStructure = {};
  m_rtTarget = {};
  m_giUniforms = {};

  m_gBufferPass = {};
  m_gBufferFrameBufferA = {};
  m_gBufferFrameBufferB = {};
  m_directSamplingPass = {};
  m_spatialResamplingPass = {};
  m_displayPass = {};
  m_displayPassSwapChainFrameBuffers = {};

  m_probeController = {};
  m_probes = {};

  m_sphere = {};
  m_probePlacementPass = {};
  m_compositingPass = {};
  m_compositingFrameBufferA = {};
  m_compositingFrameBufferB = {};

  m_globalResources = {};
  m_globalUniforms = {};
  m_primitiveConstantsBuffer = {};

  m_reservoirHeap.clear();

  m_heap = {};
}

static GlobalIllumination::LiveEditValues s_liveValues{};
static bool s_bLightSamplingMode = false;
static bool s_bDisableEnvMap = false;

static void updateUi() {
  using namespace AltheaEngine::GlobalIllumination;

  Gui::startRecordingImgui();
  const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(main_viewport->WorkPos.x + 650, main_viewport->WorkPos.y + 20),
      ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(440, 200), ImGuiCond_FirstUseEver);

  if (ImGui::Begin("Live Edit")) {
    ImGui::Text("Temporal Blend:");
    ImGui::SliderFloat(
        "##temporalblend",
        &s_liveValues.temporalBlend,
        0.0f,
        1.0f);
    ImGui::Text("Depth Discrepancy Tolerance:");
    ImGui::SliderFloat("##depthdiscrepancy", &s_liveValues.depthDiscrepancyTolerance, 0.0, 1.0);
    ImGui::Text("Spatial Resampling Radius:");
    ImGui::SliderFloat("##spatialresamplingradius", &s_liveValues.spatialResamplingRadius, 0.0, 1.0);
    ImGui::Text("Light Intensity:");
    ImGui::SliderFloat("##lightintensity", &s_liveValues.lightIntensity, 0.0, 1.0);
    ImGui::Text("Enable Light Sampling:");
    if (ImGui::Checkbox("##lightsampling", &s_bLightSamplingMode)) {
      if (s_bLightSamplingMode)
        s_liveValues.flags |= LEF_LIGHT_SAMPLING_MODE;
      else 
        s_liveValues.flags &= ~LEF_LIGHT_SAMPLING_MODE;
    }
    ImGui::Text("Disable Env Map:");
    if (ImGui::Checkbox("##disableenvmap", &s_bDisableEnvMap)) {
      if (s_bDisableEnvMap)
        s_liveValues.flags |= LEF_DISABLE_ENV_MAP;
      else 
        s_liveValues.flags &= ~LEF_DISABLE_ENV_MAP;
    }
  }

  ImGui::End();

  Gui::finishRecordingImgui();
}

void DiffuseProbes::tick(Application& app, const FrameContext& frame) {
  ++m_frameNumber;

  if (!app.getInputManager().getMouseCursorHidden())
    updateUi();

  const Camera& camera = m_pCameraController->getCamera();

  GlobalUniforms globalUniforms;
  globalUniforms.prevView = camera.computeView();
  globalUniforms.prevInverseView = glm::inverse(globalUniforms.prevView);

  m_pCameraController->tick(frame.deltaTime);

  globalUniforms.projection = camera.getProjection();
  globalUniforms.inverseProjection = glm::inverse(globalUniforms.projection);
  globalUniforms.view = camera.computeView();
  globalUniforms.inverseView = glm::inverse(globalUniforms.view);
  globalUniforms.lightCount = 0;
  globalUniforms.time = static_cast<float>(frame.currentTime);
  globalUniforms.exposure = m_exposure;

  globalUniforms.inputMask = m_inputMask = app.getInputManager().getCurrentInputMask();

  InputManager::MousePos mPos = app.getInputManager().getCurrentMousePos();
  VkExtent2D extent = app.getSwapChainExtent();
  glm::vec2 mouseUV(
      static_cast<float>(mPos.x / extent.width),
      static_cast<float>(mPos.y / extent.height));

  globalUniforms.mouseUV = mouseUV;

  globalUniforms.lightBufferHandle = INVALID_BINDLESS_HANDLE;
  globalUniforms.lightCount = 0;

  m_globalUniforms.getCurrentUniformBuffer(frame).updateUniforms(
      globalUniforms);

  uint32_t readIndex = m_targetIndex ^ 1;

  GlobalIllumination::Uniforms giUniforms{};

  giUniforms.tlas = m_accelerationStructure.getTlasHandle().index;

  giUniforms.colorSampler = m_rtTarget.targetTextureHandle.index;
  giUniforms.colorTarget = m_rtTarget.targetImageHandle.index;

  giUniforms.targetWidth = app.getSwapChainExtent().width;
  giUniforms.targetHeight = app.getSwapChainExtent().height;

  giUniforms.writeIndex = m_targetIndex;

  giUniforms.reservoirHeap = m_reservoirHeap[0].getHandle().index;
  giUniforms.reservoirsPerBuffer = RESERVOIR_COUNT_PER_BUFFER;

  giUniforms.frameNumber = m_frameNumber;

  giUniforms.probesInfo = m_probeController.getHandle().index;
  giUniforms.probes = m_probes.getHandle().index;

  giUniforms.liveValues = s_liveValues;

  m_giUniforms.updateUniforms(giUniforms, frame);
}

void DiffuseProbes::createModels(
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

void DiffuseProbes::createGlobalResources(
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

  m_globalResources = GlobalResources(
      app,
      commandBuffer,
      m_heap,
      {}, // point lights...
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

void DiffuseProbes::createGBufferPass(
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

  this->m_gBufferFrameBufferA = FrameBuffer(
      app,
      this->m_gBufferPass,
      extent,
      gBuffer.getAttachmentViewsA());
  this->m_gBufferFrameBufferB = FrameBuffer(
      app,
      this->m_gBufferPass,
      extent,
      gBuffer.getAttachmentViewsB());
}

void DiffuseProbes::createSamplingPasses(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  m_giUniforms = TransientUniforms<GlobalIllumination::Uniforms>(app);
  m_giUniforms.registerToHeap(m_heap);

  for (int i = 0; i < 2; ++i) {
    ImageOptions imageOptions{};
    imageOptions.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    imageOptions.width = app.getSwapChainExtent().width;
    imageOptions.height = app.getSwapChainExtent().height;
    imageOptions.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    ImageViewOptions viewOptions{};
    viewOptions.format = imageOptions.format;

    SamplerOptions samplerOptions{};
    samplerOptions.minFilter = VK_FILTER_NEAREST;
    samplerOptions.magFilter = VK_FILTER_NEAREST;

    m_rtTarget.target.image = Image(app, imageOptions);
    m_rtTarget.target.view =
        ImageView(app, m_rtTarget.target.image, viewOptions);
    m_rtTarget.target.sampler = Sampler(app, samplerOptions);

    m_rtTarget.targetImageHandle = m_heap.registerImage();
    m_heap.updateStorageImage(
        m_rtTarget.targetImageHandle,
        m_rtTarget.target.view,
        m_rtTarget.target.sampler);

    m_rtTarget.targetTextureHandle = m_heap.registerTexture();
    m_heap.updateTexture(
        m_rtTarget.targetTextureHandle,
        m_rtTarget.target.view,
        m_rtTarget.target.sampler);
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
          false, // forPresent is false since the imGUI pass follows the
                 // display pass
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

void DiffuseProbes::createProbeResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  ShapeUtilities::createSphere(
      app,
      commandBuffer,
      m_sphere.vertices,
      m_sphere.indices,
      10);

  VkDrawIndexedIndirectCommand indirectCmd{};
  indirectCmd.firstIndex = 0;
  indirectCmd.firstInstance = 0;
  indirectCmd.indexCount = m_sphere.indices.getIndexCount();
  indirectCmd.instanceCount = 0;
  indirectCmd.vertexOffset = 0;

  m_probeController = StructuredBuffer<VkDrawIndexedIndirectCommand>(
      app,
      1,
      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
  m_probeController.setElement(indirectCmd, 0);
  m_probeController.upload(app, commandBuffer);
  m_probeController.registerToHeap(m_heap);

  m_probes = StructuredBuffer<GlobalIllumination::Probe>(app, PROBE_COUNT);
  m_probes.zeroBuffer(commandBuffer);
  m_probes.registerToHeap(m_heap);

  {
    ComputePipelineBuilder builder{};
    builder.setComputeShader(
        GEngineDirectory +
        "/Shaders/GlobalIllumination/Probes/GBufferPlaceProbes.comp.glsl");
    builder.layoutBuilder.addDescriptorSet(m_heap.getDescriptorSetLayout())
        .addPushConstants<RTPush>(VK_SHADER_STAGE_ALL);

    m_probePlacementPass = ComputePipeline(app, std::move(builder));
  }
  const ImageOptions& targetOptions = m_rtTarget.target.image.getOptions();

  VkClearValue colorClear;
  colorClear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkClearValue depthClear;
  depthClear.depthStencil = {1.0f, 0};

  std::vector<Attachment> attachments = {
      Attachment{
          ATTACHMENT_FLAG_COLOR,
          targetOptions.format,
          colorClear,
          false,
          true,
          true},
      Attachment{
          ATTACHMENT_FLAG_DEPTH,
          app.getDepthImageFormat(),
          depthClear,
          false,
          true,
          true}};

  // Compositing pass
  std::vector<SubpassBuilder> builders;
  {
    SubpassBuilder& builder = builders.emplace_back();
    builder.colorAttachments = {0};
    builder.depthAttachment = 1;

    builder.pipelineBuilder.setDepthWrite(false)
        .addVertexInputBinding<glm::vec3>(VK_VERTEX_INPUT_RATE_VERTEX)
        .addVertexAttribute(VertexAttributeType::VEC3, 0)
        .addVertexShader(GProjectDirectory + "/Shaders/Probes/ProbeViz.vert")
        .addFragmentShader(GProjectDirectory + "/Shaders/Probes/ProbeViz.frag")
        .layoutBuilder.addDescriptorSet(m_heap.getDescriptorSetLayout())
        .addPushConstants<RTPush>(VK_SHADER_STAGE_ALL);
  }

  VkExtent2D extent{targetOptions.width, targetOptions.height};
  m_compositingPass = RenderPass(
      app,
      extent,
      std::move(attachments),
      std::move(builders),
      true);
  m_compositingFrameBufferA = FrameBuffer(
      app,
      m_compositingPass,
      extent,
      {m_rtTarget.target.view, m_globalResources.getGBuffer().getDepthViewA()});
  m_compositingFrameBufferB = FrameBuffer(
      app,
      m_compositingPass,
      extent,
      {m_rtTarget.target.view, m_globalResources.getGBuffer().getDepthViewB()});
}

void DiffuseProbes::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {
  VkDescriptorSet heapSet = m_heap.getDescriptorSet();

  uint32_t readIndex = m_targetIndex ^ 1;

  m_globalResources.getGBuffer().transitionToAttachment(commandBuffer);

  {
    GBufferPush gBufPush{};
    gBufPush.globalResourcesHandle = m_globalResources.getHandle().index;
    gBufPush.globalUniformsHandle =
        m_globalUniforms.getCurrentBindlessHandle(frame).index;

    ActiveRenderPass draw = m_gBufferPass.begin(
        app,
        commandBuffer,
        frame,
        (m_targetIndex == 0) ? m_gBufferFrameBufferA : m_gBufferFrameBufferB);
    draw.setGlobalDescriptorSets(gsl::span(&heapSet, 1));

    const DrawContext& context = draw.getDrawContext();
    for (const Model& model : m_models) {
      for (const Primitive& primitive : model.getPrimitives()) {
        gBufPush.transform = primitive.computeWorldTransform();
        gBufPush.primitiveIdx = primitive.getPrimitiveIndex();

        context.updatePushConstants(gBufPush, 0);

        context.setFrontFaceDynamic(primitive.getFrontFace());
        context.bindDescriptorSets();
        context.drawIndexed(
            primitive.getVertexBuffer(),
            primitive.getIndexBuffer());
      }
    }
  }

  RTPush push{};
  push.globalResourcesHandle = m_globalResources.getHandle().index;
  push.globalUniformsHandle =
      m_globalUniforms.getCurrentBindlessHandle(frame).index;
  push.giUniformsHandle = m_giUniforms.getCurrentHandle(frame).index;

  m_globalResources.getGBuffer().transitionToTextures(commandBuffer);

  m_rtTarget.target.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_GENERAL,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

  // GBuffer probe placement
  if ((m_inputMask & INPUT_BIT_LEFT_MOUSE) &&
      app.getInputManager().getMouseCursorHidden())
  {
    uint32_t localSize = 8;
    uint32_t groupCount = 1; // 128 / localSize;

    m_probePlacementPass.bindPipeline(commandBuffer);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        m_probePlacementPass.getLayout(),
        0,
        1,
        &heapSet,
        0,
        nullptr);
    vkCmdPushConstants(
        commandBuffer,
        m_probePlacementPass.getLayout(),
        VK_SHADER_STAGE_ALL,
        0,
        sizeof(RTPush),
        &push);
    vkCmdDispatch(commandBuffer, groupCount, groupCount, 1);
  }

  probeBarrier(commandBuffer);

  // Direct Sampling
  {
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

  // Spatial Resampling
  {
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

  m_rtTarget.target.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

  m_globalResources.getGBuffer().transitionToAttachment(commandBuffer);

  // Compositing pass
  {
    ActiveRenderPass pass = m_compositingPass.begin(
        app,
        commandBuffer,
        frame,
        (m_targetIndex == 0) ? m_compositingFrameBufferA
                             : m_compositingFrameBufferB);
    // Bind global descriptor sets
    pass.setGlobalDescriptorSets(gsl::span(&heapSet, 1));

    const DrawContext& context = pass.getDrawContext();
    context.bindDescriptorSets();
    context.updatePushConstants(push, 0);
    context.bindIndexBuffer(m_sphere.indices);
    context.bindVertexBuffer(m_sphere.vertices);

    vkCmdDrawIndexedIndirect(
        commandBuffer,
        m_probeController.getAllocation().getBuffer(),
        0,
        1,
        sizeof(VkDrawIndexedIndirectCommand));
  }

  m_rtTarget.target.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

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
      context.updatePushConstants(push, 0);
      context.bindDescriptorSets();
      context.draw(3);
    }
  }

  if (!app.getInputManager().getMouseCursorHidden())
    Gui::draw(app, frame, commandBuffer);

  m_targetIndex ^= 1;
}

void DiffuseProbes::reservoirBarrier(VkCommandBuffer commandBuffer) {
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

void DiffuseProbes::probeBarrier(VkCommandBuffer commandBuffer) {
  VkBufferMemoryBarrier barriers[2];
  barriers[0] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barriers[0].buffer = m_probes.getAllocation().getBuffer();
  barriers[0].offset = 0;
  barriers[0].size = m_probes.getSize();
  barriers[0].srcAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  barriers[1] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barriers[1].buffer = m_probeController.getAllocation().getBuffer();
  barriers[1].offset = 0;
  barriers[1].size = m_probeController.getSize();
  barriers[1].srcAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  barriers[1].dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;

  vkCmdPipelineBarrier(
      commandBuffer,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      0,
      0,
      nullptr,
      2,
      barriers,
      0,
      nullptr);
}
} // namespace DiffuseProbes
} // namespace AltheaDemo