#include "HlslTest.h"

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

namespace AltheaDemo {
namespace HlslTest {

HlslTest::HlslTest() {}

void HlslTest::initGame(Application& app) {
  const VkExtent2D& windowDims = app.getSwapChainExtent();

  // TODO: need to unbind these at shutdown
  InputManager& input = app.getInputManager();

  // Recreate any stale pipelines (shader hot-reload)
  input.addKeyBinding(
      {GLFW_KEY_R, GLFW_PRESS, GLFW_MOD_CONTROL},
      [&app, that = this]() {
        // that->_shPass.tryRecompile(app);
        that->_renderPass.tryRecompile(app);
      });

  input.setMouseCursorHidden(false);
}

void HlslTest::shutdownGame(Application& app) {
}

void HlslTest::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();

  SingleTimeCommandBuffer commandBuffer(app);
  this->_createGlobalResources(app, commandBuffer);
  this->_createRenderPass(app);
}

void HlslTest::destroyRenderState(Application& app) {
  this->_ibl = {};
  this->_globalUniforms = {};

  this->_renderPass = {};
  this->_swapChainFrameBuffers = {};

  this->_globalHeap = {};
}

void HlslTest::tick(Application& app, const FrameContext& frame) {
  Camera camera{};

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

  this->_globalUniforms.getCurrentUniformBuffer(frame).updateUniforms(
      globalUniforms);
}

void HlslTest::_createGlobalResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  this->_globalHeap = GlobalHeap(app);
  this->_globalUniforms = GlobalUniformsResource(app, this->_globalHeap);
}

void HlslTest::_createRenderPass(Application& app) {
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

    subpassBuilder.pipelineBuilder
      .setCullMode(VK_CULL_MODE_FRONT_BIT)
      // .setDepthTesting(false) // ??
      // Vertex shader
      .addVertexShader(
        GProjectDirectory + "/Shaders/HlslTest/Test.vert.hlsl",
        defs,
        SHADER_LANGUAGE_HLSL)
      // Fragment shader
      .addFragmentShader(
        GProjectDirectory + "/Shaders/HlslTest/Test.frag.hlsl",
        defs,
        SHADER_LANGUAGE_HLSL);/*

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_globalHeap.getDescriptorSetLayout())
        .addPushConstants<PushConstants>(VK_SHADER_STAGE_ALL);*/
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

void HlslTest::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {

  VkDescriptorSet heapDescriptorSet = this->_globalHeap.getDescriptorSet();

  // Render pass
  {
    ActiveRenderPass pass = this->_renderPass.begin(
        app,
        commandBuffer,
        frame,
        this->_swapChainFrameBuffers.getCurrentFrameBuffer(frame));
    // Bind global descriptor sets
    //pass.setGlobalDescriptorSets(gsl::span(&heapDescriptorSet, 1));
    //pass.getDrawContext().bindDescriptorSets();
    pass.getDrawContext().draw(3);
  }

}
} // namespace HlslTest
} // namespace AltheaDemo