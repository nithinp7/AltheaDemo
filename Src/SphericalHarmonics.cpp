#include "SphericalHarmonics.h"

#include <Althea/Application.h>
#include <Althea/Camera.h>
#include <Althea/Cubemap.h>
#include <Althea/DescriptorSet.h>
#include <Althea/GraphicsPipeline.h>
#include <Althea/Gui.h>
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
namespace SphericalHarmonics {
namespace {
struct PushConstants {
  IBLHandles ibl;
  uint32_t globalUniforms;
  uint32_t shUniforms;
};

struct GraphPushConstants {
  uint32_t vertexCount;
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
        that->_graphPass.tryRecompile(app);
        that->_renderPass.tryRecompile(app);
      });
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
  this->_shCoeffs = {};

  this->_shPass = {};
  this->_renderPass = {};
  this->_swapChainFrameBuffers = {};

  this->_globalHeap = {};
}

void SphericalHarmonics::tick(Application& app, const FrameContext& frame) {
  {
    Gui::startRecordingImgui();

    const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(main_viewport->WorkPos.x + 650, main_viewport->WorkPos.y + 20),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Debug Options")) {
      if (ImGui::CollapsingHeader("Lighting")) {
        ImGui::Text("Color:");
        ImGui::SliderFloat3(
            "##color",
            reinterpret_cast<float(&)[3]>(this->_shUniformValues.color),
            0.0f,
            1.0f);
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

  GlobalUniforms globalUniforms;
  globalUniforms.projection = camera.getProjection();
  globalUniforms.inverseProjection = glm::inverse(globalUniforms.projection);
  globalUniforms.view = camera.computeView();
  globalUniforms.inverseView = glm::inverse(globalUniforms.view);
  globalUniforms.time = static_cast<float>(frame.currentTime);
  globalUniforms.exposure = this->_exposure;
  globalUniforms.inputMask = app.getInputManager().getCurrentInputMask();

  InputManager::MousePos mPos = app.getInputManager().getCurrentMousePos();
  VkExtent2D extent = app.getSwapChainExtent();
  globalUniforms.mouseUV = glm::vec2(
      static_cast<float>(mPos.x / extent.width),
      static_cast<float>(mPos.y / extent.height));

  this->_globalUniforms.getCurrentUniformBuffer(frame).updateUniforms(
      globalUniforms);

  this->_shUniforms.getCurrentUniformBuffer(frame).updateUniforms(
      this->_shUniformValues);
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

  this->_shCoeffs = StructuredBuffer<SHCoeffs>(app, 1);
  this->_shCoeffs.registerToHeap(this->_globalHeap);

  this->_shUniforms = TransientUniforms<SHUniforms>(app);
  this->_shUniforms.registerToHeap(this->_globalHeap);
}

void SphericalHarmonics::_createGraph(Application& app) {

  VkExtent2D extent = {256, 256};

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

  this->_graphPass = RenderPass(
      app,
      extent,
      std::move(attachments),
      std::move(subpassBuilders));
  this->_graphFrameBuffer =
      FrameBuffer(app, this->_graphPass, extent, {this->_graph.view});
}

void SphericalHarmonics::_createComputePass(Application& app) {}

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

  // Graph pass
  this->_graph.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  {
    GraphPushConstants push{};
    push.vertexCount = 100;

    ActiveRenderPass pass = this->_graphPass.begin(
        app,
        commandBuffer,
        frame,
        this->_graphFrameBuffer);
    pass.setGlobalDescriptorSets(gsl::span(&heapDescriptorSet, 1));
    pass.getDrawContext().bindDescriptorSets();
    pass.getDrawContext().updatePushConstants(push, 0);
    pass.getDrawContext().draw(push.vertexCount);
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