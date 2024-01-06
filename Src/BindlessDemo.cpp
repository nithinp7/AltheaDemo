#include "BindlessDemo.h"

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
namespace BindlessDemo {
namespace {
struct ForwardPassPushConstants {
  glm::mat4 model;
  uint32_t primitiveIdx;

  uint32_t globalResources;
  uint32_t globalUniforms;
};

struct DeferredPassPushConstants {
  uint32_t globalResources;
  uint32_t globalUniforms;
  uint32_t reflectionBuffer;
};
} // namespace

BindlessDemo::BindlessDemo() {}

void BindlessDemo::initGame(Application& app) {
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

        for (Subpass& subpass : that->_pForwardPass->getSubpasses()) {
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

void BindlessDemo::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void BindlessDemo::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  Gui::createRenderState(app);

  SingleTimeCommandBuffer commandBuffer(app);
  this->_createGlobalResources(app, commandBuffer);
  this->_createForwardPass(app);
  this->_createDeferredPass(app);
}

void BindlessDemo::destroyRenderState(Application& app) {
  Primitive::resetPrimitiveIndexCount();

  Gui::destroyRenderState(app);

  this->_models.clear();

  this->_pForwardPass.reset();
  this->_forwardFrameBuffer = {};
  this->_primitiveConstantsBuffer = {};

  this->_pDeferredPass.reset();
  this->_swapChainFrameBuffers = {};

  this->_pointLights = {};

  this->_SSR = {};
  this->_globalUniforms = {};

  this->_globalResources = {};
  this->_globalHeap = {};
}

void BindlessDemo::tick(Application& app, const FrameContext& frame) {
  {
    Gui::startRecordingImgui();
    const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(main_viewport->WorkPos.x + 650, main_viewport->WorkPos.y + 20),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(220, 100), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Debug Options")) {
      if (ImGui::CollapsingHeader("Lighting")) {
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
  globalUniforms.lightCount = static_cast<int>(this->_pointLights.getCount());
  globalUniforms.lightBufferHandle =
      this->_pointLights.getCurrentLightBufferHandle(frame).index;
  globalUniforms.time = static_cast<float>(frame.currentTime);
  globalUniforms.exposure = this->_exposure;

  this->_globalUniforms.getCurrentUniformBuffer(frame).updateUniforms(
      globalUniforms);

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

void BindlessDemo::_createModels(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {

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
      glm::vec3(10.0f, -1.0f, 0.0f)));
}

void BindlessDemo::_createGlobalResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  this->_globalHeap = GlobalHeap(app);
  this->_globalUniforms = GlobalUniformsResource(app, this->_globalHeap);

  // Create GLTF resource heaps
  {
    this->_createModels(app, commandBuffer);

    for (Model& model : this->_models) {
      model.registerToHeap(this->_globalHeap);
    }

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

    // The primitive constant buffers contain all the bindless
    // indices for the primitive texture resources
    this->_primitiveConstantsBuffer.upload(app, commandBuffer);
    this->_primitiveConstantsBuffer.registerToHeap(this->_globalHeap);

    // this->_textureHeap = TextureHeap(this->_models);
  }

  // Global resources
  {
    this->_pointLights = PointLightCollection(
        app,
        commandBuffer,
        this->_globalHeap,
        9,
        true,
        this->_primitiveConstantsBuffer.getHandle());
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
  }

  this->_globalResources = GlobalResources(
      app,
      commandBuffer,
      this->_globalHeap,
      this->_pointLights.getShadowMapHandle(),
      this->_primitiveConstantsBuffer.getHandle());

  // Set up SSR resources
  this->_SSR = ScreenSpaceReflection(
      app,
      commandBuffer,
      this->_globalHeap.getDescriptorSetLayout());
  this->_SSR.getReflectionBuffer().registerToHeap(this->_globalHeap);
}

void BindlessDemo::_createForwardPass(Application& app) {
  std::vector<SubpassBuilder> subpassBuilders;

  //  FORWARD GLTF PASS
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    // The GBuffer contains the following color attachments
    // 1. Position
    // 2. Normal
    // 3. Albedo
    // 4. Metallic-Roughness-Occlusion
    subpassBuilder.colorAttachments = {0, 1, 2, 3};
    subpassBuilder.depthAttachment = 4;

    Primitive::buildPipeline(subpassBuilder.pipelineBuilder);

    ShaderDefines defs;
    defs.emplace("BINDLESS_SET", "0");

    subpassBuilder
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
        .addDescriptorSet(this->_globalHeap.getDescriptorSetLayout())
        .addPushConstants<ForwardPassPushConstants>(VK_SHADER_STAGE_ALL);
  }

  const GBufferResources& gBuffer = this->_globalResources.getGBuffer();
  std::vector<Attachment> attachments = gBuffer.getAttachmentDescriptions();
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pForwardPass = std::make_unique<RenderPass>(
      app,
      extent,
      std::move(attachments),
      std::move(subpassBuilders));

  this->_forwardFrameBuffer = FrameBuffer(
      app,
      *this->_pForwardPass,
      extent,
      gBuffer.getAttachmentViews());
}

void BindlessDemo::_createDeferredPass(Application& app) {
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
                 // deferred pass
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

    ShaderDefines defs;
    defs.emplace("BINDLESS_SET", "0");

    subpassBuilder.pipelineBuilder.setCullMode(VK_CULL_MODE_FRONT_BIT)
        .setDepthTesting(false)

        // Vertex shader
        .addVertexShader(GProjectDirectory + "/Shaders/DeferredPass.vert", defs)
        // Fragment shader
        .addFragmentShader(
            GProjectDirectory + "/Shaders/DeferredPass.frag",
            defs)

        // Pipeline resource layouts
        .layoutBuilder
        .addDescriptorSet(this->_globalHeap.getDescriptorSetLayout())
        .addPushConstants<DeferredPassPushConstants>(VK_SHADER_STAGE_ALL);
  }

  // SHOW POINT LIGHTS (kinda hacky)
  // TODO: Really light objects should be rendered in the forward
  // pass as well and an emissive channel should be added to the
  // G-Buffer
  this->_pointLights.setupPointLightMeshSubpass(
      subpassBuilders.emplace_back(),
      0,
      1,
      this->_globalHeap.getDescriptorSetLayout());

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

void BindlessDemo::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {

  this->_pointLights.updateResource(frame);
  this->_globalResources.getGBuffer().transitionToAttachment(commandBuffer);

  VkDescriptorSet heapDescriptorSet = this->_globalHeap.getDescriptorSet();

  // Draw point light shadow maps
  this->_pointLights.drawShadowMaps(
      app,
      commandBuffer,
      frame,
      this->_models,
      heapDescriptorSet,
      this->_globalResources.getHandle());

  // Forward pass
  {
    ForwardPassPushConstants push{};
    push.globalResources =
        this->_globalResources.getConstants().getHandle().index;
    push.globalUniforms =
        this->_globalUniforms.getCurrentBindlessHandle(frame).index;

    ActiveRenderPass pass = this->_pForwardPass->begin(
        app,
        commandBuffer,
        frame,
        this->_forwardFrameBuffer);
    // Bind global descriptor sets
    pass.setGlobalDescriptorSets(gsl::span(&heapDescriptorSet, 1));
    pass.getDrawContext().bindDescriptorSets();

    // Draw models
    for (const Model& model : this->_models) {
      for (const Primitive& primitive : model.getPrimitives()) {
        push.model = primitive.computeWorldTransform();
        push.primitiveIdx =
            static_cast<uint32_t>(primitive.getPrimitiveIndex());

        pass.getDrawContext().setFrontFaceDynamic(primitive.getFrontFace());
        pass.getDrawContext().updatePushConstants(push, 0);
        pass.getDrawContext().drawIndexed(
            primitive.getVertexBuffer(),
            primitive.getIndexBuffer());
      }
    }
  }

  this->_globalResources.getGBuffer().transitionToTextures(commandBuffer);

  // Reflection buffer and convolution
  {
    this->_SSR.captureReflection(
        app,
        commandBuffer,
        heapDescriptorSet,
        frame,
        this->_globalUniforms.getCurrentBindlessHandle(frame),
        this->_globalResources.getHandle());
    this->_SSR.convolveReflectionBuffer(app, commandBuffer, frame);
  }

  // Deferred pass
  {
    DeferredPassPushConstants push{};
    push.globalResources = this->_globalResources.getHandle().index;
    push.globalUniforms =
        this->_globalUniforms.getCurrentBindlessHandle(frame).index;
    push.reflectionBuffer = this->_SSR.getReflectionBuffer().getHandle().index;

    ActiveRenderPass pass = this->_pDeferredPass->begin(
        app,
        commandBuffer,
        frame,
        this->_swapChainFrameBuffers.getCurrentFrameBuffer(frame));
    // Bind global descriptor sets
    pass.setGlobalDescriptorSets(gsl::span(&heapDescriptorSet, 1));
    pass.getDrawContext().updatePushConstants(push, 0);

    {
      const DrawContext& context = pass.getDrawContext();
      context.bindDescriptorSets();
      context.draw(3);
    }

    pass.nextSubpass();
    pass.setGlobalDescriptorSets(gsl::span(&heapDescriptorSet, 1));
    this->_pointLights.draw(
        pass.getDrawContext(),
        this->_globalUniforms.getCurrentBindlessHandle(frame));
  }

  Gui::draw(app, frame, commandBuffer);
}
} // namespace BindlessDemo
} // namespace AltheaDemo