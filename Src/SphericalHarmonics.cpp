#include "SphericalHarmonics.h"

#include <Althea/Application.h>
#include <Althea/Camera.h>
#include <Althea/Cubemap.h>
#include <Althea/DescriptorSet.h>
#include <Althea/GraphicsPipeline.h>
#include <Althea/Gui.h>
#include <Althea/InputManager.h>
#include <Althea/InputMask.h>
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

#define DISPLAY_MODE_DEFAULT 0
#define DISPLAY_MODE_SH 1
#define DISPLAY_MODE_GRAPH 2

namespace AltheaDemo {
namespace SphericalHarmonics {
namespace {
struct PushConstants {
  IBLHandles ibl;
  uint32_t globalUniforms;
  uint32_t shUniforms;
};

struct GraphPushConstants {
  uint32_t vertexCount;
  uint32_t shUniforms;
  uint32_t legendreUniforms;
};

struct SHPushConstants {
  uint32_t seed;
  uint32_t coeffsHandle;
  uint32_t envMapHandle;
};
} // namespace

SphericalHarmonics::SphericalHarmonics() {}

void SphericalHarmonics::initGame(Application& app) {
  const VkExtent2D& windowDims = app.getSwapChainExtent();
  this->_pCameraController = std::make_unique<CameraController>(
      app.getInputManager(),
      90.0f,
      (float)windowDims.width / (float)windowDims.height);
  this->_pCameraController->setMaxSpeed(15.0f);

  // TODO: need to unbind these at shutdown
  InputManager& input = app.getInputManager();

  // Recreate any stale pipelines (shader hot-reload)
  input.addKeyBinding(
      {GLFW_KEY_R, GLFW_PRESS, GLFW_MOD_CONTROL},
      [&app, that = this]() {
        // that->_shPass.tryRecompile(app);
        that->_fitLegendre.tryRecompile(app);
        that->_graphPass.tryRecompile(app);
        that->_renderPass.tryRecompile(app);
      });

  input.setMouseCursorHidden(false);
}

void SphericalHarmonics::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void SphericalHarmonics::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  Gui::createRenderState(app);

  SingleTimeCommandBuffer commandBuffer(app);
  this->_createGlobalResources(app, commandBuffer);
  this->_createGraph(app);
  this->_createComputePass(app);
  this->_createRenderPass(app);
}

void SphericalHarmonics::destroyRenderState(Application& app) {
  Gui::destroyRenderState(app);

  this->_graph = {};
  this->_graphPass = {};
  this->_graphFrameBuffer = {};

  this->_ibl = {};
  this->_globalUniforms = {};
  this->_shUniforms = {};
  this->_legendreUniforms = {};
  this->_shCoeffs = {};
  this->_legendreCoeffs = {};

  this->_shPass = {};
  this->_fitLegendre = {};
  this->_renderPass = {};
  this->_swapChainFrameBuffers = {};

  this->_globalHeap = {};
}

#define COEFF_SLIDER(IDX)                                                      \
  ImGui::SetNextItemWidth(80);                                                 \
  ImGui::SliderFloat(                                                          \
      "##P" #IDX,                                                              \
      &this->_shUniformValues.coeffs[IDX],                                     \
      coeffMin,                                                                \
      coeffMax,                                                                \
      "P" #IDX ": %.3f")

void SphericalHarmonics::tick(Application& app, const FrameContext& frame) {
  {
    Gui::startRecordingImgui();

    const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(main_viewport->WorkPos.x + 650, main_viewport->WorkPos.y + 20),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);

    char* displayModes[] = {"Default", "Spherical Harmonics", "Graph"};

    const float coeffMin = -1.0f;
    const float coeffMax = 1.0f;
    if (ImGui::Begin("Spherical Harmonics")) {
      ImGui::Text("Display Mode:");
      ImGui::Combo(
          "##displayMode",
          &this->_shUniformValues.displayMode,
          displayModes,
          3);
      if (ImGui::CollapsingHeader("Graph", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Legendre Coeffs:");

        if (ImGui::Button("Reset Samples")) {
          this->_legendreUniformValues.sampleCount = 0;
        }
        
        // if (ImGui::Button("Reset Coeffs")) {
        //   for (uint32_t i = 0; i < 16; ++i)
        //     this->_shUniformValues.coeffs[i] = 0.0f;
        // }

        // COEFF_SLIDER(0);
        // ImGui::SameLine();
        // COEFF_SLIDER(1);
        // ImGui::SameLine();
        // COEFF_SLIDER(2);
        // ImGui::SameLine();
        // COEFF_SLIDER(3);

        // COEFF_SLIDER(4);
        // ImGui::SameLine();
        // COEFF_SLIDER(5);
        // ImGui::SameLine();
        // COEFF_SLIDER(6);
        // ImGui::SameLine();
        // COEFF_SLIDER(7);

        // COEFF_SLIDER(8);
        // ImGui::SameLine();
        // COEFF_SLIDER(9);
        // ImGui::SameLine();
        // COEFF_SLIDER(10);
        // ImGui::SameLine();
        // COEFF_SLIDER(11);

        // COEFF_SLIDER(12);
        // ImGui::SameLine();
        // COEFF_SLIDER(13);
        // ImGui::SameLine();
        // COEFF_SLIDER(14);
        // ImGui::SameLine();
        // COEFF_SLIDER(15);
      }

      if (ImGui::CollapsingHeader("Lighting")) {
        // ImGui::Text("Color:");
        // ImGui::SliderFloat3(
        //     "##color",
        //     reinterpret_cast<float(&)[3]>(this->_shUniformValues.color),
        //     0.0f,
        //     1.0f);
        ImGui::Text("Exposure:");
        ImGui::SliderFloat("##exposure", &this->_exposure, 0.0f, 1.0f);
      }
    }

    ImGui::End();

    Gui::finishRecordingImgui();
  }

  this->_pCameraController->tick(frame.deltaTime);
  const Camera& camera = this->_pCameraController->getCamera();

  const glm::mat4& projection = camera.getProjection();

  uint32_t inputMask = app.getInputManager().getCurrentInputMask();
  GlobalUniforms globalUniforms;
  globalUniforms.projection = camera.getProjection();
  globalUniforms.inverseProjection = glm::inverse(globalUniforms.projection);
  globalUniforms.view = camera.computeView();
  globalUniforms.inverseView = glm::inverse(globalUniforms.view);
  globalUniforms.time = static_cast<float>(frame.currentTime);
  globalUniforms.exposure = this->_exposure;
  globalUniforms.inputMask = inputMask;

  InputManager::MousePos mPos = app.getInputManager().getCurrentMousePos();
  VkExtent2D extent = app.getSwapChainExtent();
  glm::vec2 mouseUV(
      static_cast<float>(mPos.x / extent.width),
      static_cast<float>(mPos.y / extent.height));

  globalUniforms.mouseUV = mouseUV;

  {
    // Add legendre samples
    // TODO: Remove silly hack
    static bool bLastPressed = false;
    if (inputMask & INPUT_BIT_LEFT_MOUSE) {
      if (!bLastPressed && this->_legendreUniformValues.sampleCount < 10) {
        this->_legendreUniformValues
            .samples[this->_legendreUniformValues.sampleCount] = mouseUV;
        this->_legendreUniformValues.sampleCount++;
      }

      bLastPressed = true;
    } else {
      bLastPressed = false;
    }
  }

  this->_globalUniforms.getCurrentUniformBuffer(frame).updateUniforms(
      globalUniforms);

  this->_shUniforms.getCurrentUniformBuffer(frame).updateUniforms(
      this->_shUniformValues);

  this->_legendreUniforms.getCurrentUniformBuffer(frame).updateUniforms(
      this->_legendreUniformValues);
}

void SphericalHarmonics::_createGlobalResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  this->_globalHeap = GlobalHeap(app);
  this->_globalUniforms = GlobalUniformsResource(app, this->_globalHeap);

  this->_ibl = ImageBasedLighting::createResources(
      app,
      commandBuffer,
      "NeoclassicalInterior");
  this->_ibl.registerToHeap(this->_globalHeap);

  this->_shCoeffs = StructuredBuffer<CoeffSet>(app, 1);
  this->_shCoeffs.registerToHeap(this->_globalHeap);

  this->_legendreCoeffs = StructuredBuffer<CoeffSet>(app, 1);
  this->_legendreCoeffs.registerToHeap(this->_globalHeap);

  this->_shUniforms = TransientUniforms<SHUniforms>(app);
  this->_shUniforms.registerToHeap(this->_globalHeap);

  this->_legendreUniforms = TransientUniforms<LegendreUniforms>(app);
  this->_legendreUniforms.registerToHeap(this->_globalHeap);

  this->_legendreUniformValues.coeffBuffer =
      this->_legendreCoeffs.getHandle().index;
}

void SphericalHarmonics::_createGraph(Application& app) {

  VkExtent2D extent = {512, 512};

  ImageOptions image{};
  image.width = extent.width;
  image.height = extent.height;
  image.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

  this->_graph.image = Image(app, image);
  this->_graph.view = ImageView(app, this->_graph.image, {});
  this->_graph.sampler = Sampler(app, {});

  this->_graphHandle = this->_globalHeap.registerTexture();
  this->_globalHeap.updateTexture(
      this->_graphHandle,
      this->_graph.view,
      this->_graph.sampler);
  this->_shUniformValues.graphHandle = this->_graphHandle.index;

  VkClearValue colorClear;
  colorClear.color = {{1.0f, 1.0f, 1.0f, 1.0f}};

  std::vector<Attachment> attachments = {Attachment{
      ATTACHMENT_FLAG_COLOR,
      image.format,
      colorClear,
      false,
      false,
      true}};

  std::vector<SubpassBuilder> subpassBuilders;

  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments = {0};

    ShaderDefines defs;
    defs.emplace("BINDLESS_SET", "0");

    subpassBuilder.pipelineBuilder.setDepthTesting(false)
        .setPrimitiveType(PrimitiveType::LINES)
        .setLineWidth(3.0f)
        // Vertex shader
        .addVertexShader(
            GProjectDirectory + "/Shaders/SphericalHarmonics/GraphLine.vert",
            defs)
        // Fragment shader
        .addFragmentShader(
            GProjectDirectory + "/Shaders/SphericalHarmonics/GraphLine.frag",
            defs)

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_globalHeap.getDescriptorSetLayout())
        .addPushConstants<GraphPushConstants>(VK_SHADER_STAGE_ALL);
  }

  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments = {0};

    ShaderDefines defs;
    defs.emplace("BINDLESS_SET", "0");

    subpassBuilder.pipelineBuilder.setDepthTesting(false)
        .setPrimitiveType(PrimitiveType::POINTS)
        .setLineWidth(3.0f)

        // .setP(3.0f) // set point size??
        // Vertex shader
        .addVertexShader(
            GProjectDirectory + "/Shaders/SphericalHarmonics/GraphPoint.vert",
            defs)
        // Fragment shader
        .addFragmentShader(
            GProjectDirectory + "/Shaders/SphericalHarmonics/GraphPoint.frag",
            defs)

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_globalHeap.getDescriptorSetLayout())
        .addPushConstants<uint32_t>(VK_SHADER_STAGE_ALL); // uniform handle
  }

  this->_graphPass = RenderPass(
      app,
      extent,
      std::move(attachments),
      std::move(subpassBuilders));
  this->_graphFrameBuffer =
      FrameBuffer(app, this->_graphPass, extent, {this->_graph.view});
}

void SphericalHarmonics::_createComputePass(Application& app) {
  {
    ComputePipelineBuilder builder{};
    builder.setComputeShader(
        GProjectDirectory +
        "/Shaders/SphericalHarmonics/FitLegendre.comp.glsl");
    builder.layoutBuilder.addDescriptorSet(
        this->_globalHeap.getDescriptorSetLayout());
    builder.layoutBuilder.addPushConstants<uint32_t>(VK_SHADER_STAGE_ALL);

    this->_fitLegendre = ComputePipeline(app, std::move(builder));
  }

  {
    ComputePipelineBuilder builder{};
    builder.setComputeShader(
        GProjectDirectory +
        "/Shaders/SphericalHarmonics/UpdateSH.comp.glsl");
    builder.layoutBuilder.addDescriptorSet(
        this->_globalHeap.getDescriptorSetLayout());
    builder.layoutBuilder.addPushConstants<uint32_t>(VK_SHADER_STAGE_ALL);

    this->_fitLegendre = ComputePipeline(app, std::move(builder));
  }
}

void SphericalHarmonics::_createRenderPass(Application& app) {
  VkClearValue colorClear;
  colorClear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkClearValue depthClear;
  depthClear.depthStencil = {1.0f, 0};

  std::vector<Attachment> attachments = {
      Attachment{
          ATTACHMENT_FLAG_COLOR,
          app.getSwapChainImageFormat(),
          colorClear,
          false,
          false,
          true}
      //,

      // Depth buffer
      // Attachment{
      //     ATTACHMENT_FLAG_DEPTH,
      //     app.getDepthImageFormat(),
      //     depthClear,
      //     false,
      //     true,
      //     true}
  };

  std::vector<SubpassBuilder> subpassBuilders;

  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments = {0};
    // subpassBuilder.depthAttachment = 1;

    ShaderDefines defs;
    defs.emplace("BINDLESS_SET", "0");

    subpassBuilder.pipelineBuilder
        .setCullMode(VK_CULL_MODE_FRONT_BIT)
        // .setDepthTesting(false) // ??
        // Vertex shader
        .addVertexShader(
            GProjectDirectory + "/Shaders/SphericalHarmonics/SH.vert",
            defs)
        // Fragment shader
        .addFragmentShader(
            GProjectDirectory + "/Shaders/SphericalHarmonics/SH.frag",
            defs)

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_globalHeap.getDescriptorSetLayout())
        .addPushConstants<PushConstants>(VK_SHADER_STAGE_ALL);
  }

  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_renderPass = RenderPass(
      app,
      extent,
      std::move(attachments),
      std::move(subpassBuilders));

  this->_swapChainFrameBuffers =
      SwapChainFrameBufferCollection(app, this->_renderPass, {});
  // {app.getDepthImageView()});
}

void SphericalHarmonics::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {

  VkDescriptorSet heapDescriptorSet = this->_globalHeap.getDescriptorSet();

  // Compute passes
  {
    vkCmdBindPipeline(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        this->_fitLegendre.getPipeline());
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        this->_fitLegendre.getLayout(),
        0,
        1,
        &heapDescriptorSet,
        0,
        nullptr);
    uint32_t push = this->_legendreUniforms.getCurrentHandle(frame).index;
    vkCmdPushConstants(
        commandBuffer,
        this->_fitLegendre.getLayout(),
        VK_SHADER_STAGE_ALL,
        0,
        sizeof(uint32_t),
        &push);
    vkCmdDispatch(commandBuffer, 1, 1, 1); // local size 16x1x1

    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.buffer = this->_legendreCoeffs.getAllocation().getBuffer();
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.offset = 0;
    barrier.size = this->_legendreCoeffs.getSize();

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        0,
        0,
        nullptr,
        1,
        &barrier,
        0,
        nullptr);
  }

  // Graph pass
  this->_graph.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  {
    GraphPushConstants push{};
    push.vertexCount = 100;
    push.shUniforms = this->_shUniforms.getCurrentHandle(frame).index;
    push.legendreUniforms =
        this->_legendreUniforms.getCurrentHandle(frame).index;

    ActiveRenderPass pass = this->_graphPass.begin(
        app,
        commandBuffer,
        frame,
        this->_graphFrameBuffer);
    pass.setGlobalDescriptorSets(gsl::span(&heapDescriptorSet, 1));
    pass.getDrawContext().bindDescriptorSets();
    pass.getDrawContext().updatePushConstants(push, 0);
    pass.getDrawContext().draw(push.vertexCount, 16);

    pass.nextSubpass();

    if (this->_legendreUniformValues.sampleCount > 0) {
      pass.getDrawContext().bindDescriptorSets();
      pass.getDrawContext().updatePushConstants(push.legendreUniforms, 0);
      pass.getDrawContext().draw(this->_legendreUniformValues.sampleCount);
    }
  }
  this->_graph.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  // Render pass
  {
    PushConstants push{};
    push.ibl = this->_ibl.getHandles();
    push.globalUniforms =
        this->_globalUniforms.getCurrentBindlessHandle(frame).index;
    push.shUniforms = this->_shUniforms.getCurrentHandle(frame).index;

    ActiveRenderPass pass = this->_renderPass.begin(
        app,
        commandBuffer,
        frame,
        this->_swapChainFrameBuffers.getCurrentFrameBuffer(frame));
    // Bind global descriptor sets
    pass.setGlobalDescriptorSets(gsl::span(&heapDescriptorSet, 1));
    pass.getDrawContext().bindDescriptorSets();
    pass.getDrawContext().updatePushConstants(push, 0);
    pass.getDrawContext().draw(3);
  }

  Gui::draw(app, frame, commandBuffer);
}
} // namespace SphericalHarmonics
} // namespace AltheaDemo