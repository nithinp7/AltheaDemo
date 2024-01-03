#include "SphericalHarmonics.h"

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
namespace SphericalHarmonics {
namespace {
struct PushConstants {
  IBLHandles ibl;
  uint32_t shWeights;
  uint32_t globalUniforms;
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
        {
          ComputePipeline& pipeline = that->_shPass;
          if (pipeline.recompileStaleShaders()) {
            if (pipeline.hasShaderRecompileErrors()) {
              std::cout << pipeline.getShaderRecompileErrors() << "\n";
            } else {
              pipeline.recreatePipeline(app);
            }
          }
        }

        for (Subpass& subpass : that->_renderPass.getSubpasses()) {
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

void SphericalHarmonics::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void SphericalHarmonics::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  SingleTimeCommandBuffer commandBuffer(app);
  this->_createGlobalResources(app, commandBuffer);
  this->_createComputePass(app);
  this->_createRenderPass(app);
}

void SphericalHarmonics::destroyRenderState(Application& app) {
  this->_ibl = {};
  this->_globalUniforms = {};
  this->_shCoeffs = {};

  this->_shPass = {};
  this->_renderPass = {};
  this->_swapChainFrameBuffers = {};

  this->_globalHeap = {};
}

void SphericalHarmonics::tick(Application& app, const FrameContext& frame) {
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

  this->_globalUniforms.getCurrentUniformBuffer(frame).updateUniforms(
      globalUniforms);
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

  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments = {0};
    subpassBuilder.depthAttachment = 1;

    ShaderDefines defs;
    defs.emplace("BINDLESS_SET", "0");

    subpassBuilder
        .pipelineBuilder
        .setCullMode(VK_CULL_MODE_FRONT_BIT)
        .setDepthTesting(false)
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

  this->_swapChainFrameBuffers = SwapChainFrameBufferCollection(
      app,
      this->_renderPass,
      {app.getDepthImageView()});
}

void SphericalHarmonics::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {

  VkDescriptorSet heapDescriptorSet = this->_globalHeap.getDescriptorSet();

  // Render pass
  {
    PushConstants push{};
    push.ibl = this->_ibl.getHandles();
    push.globalUniforms =
        this->_globalUniforms.getCurrentBindlessHandle(frame).index;
    push.shWeights = this->_shCoeffs.getHandle().index;

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
}
} // namespace SphericalHarmonics
} // namespace AltheaDemo