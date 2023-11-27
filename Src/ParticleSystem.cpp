#include "ParticleSystem.h"

#include "SpatialHashUnitTests.h"

#include <Althea/Application.h>
#include <Althea/Camera.h>
#include <Althea/Cubemap.h>
#include <Althea/DescriptorSet.h>
#include <Althea/GraphicsPipeline.h>
#include <Althea/InputManager.h>
#include <Althea/ModelViewProjection.h>
#include <Althea/Primitive.h>
#include <Althea/ShapeUtilities.h>
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
namespace ParticleSystem {

ParticleSystem::ParticleSystem() {}

void ParticleSystem::initGame(Application& app) {
  const VkExtent2D& windowDims = app.getSwapChainExtent();
  this->_pCameraController = std::make_unique<CameraController>(
      app.getInputManager(),
      90.0f,
      (float)windowDims.width / (float)windowDims.height);
  this->_pCameraController->setMaxSpeed(15.0f);

  // TODO: need to unbind these at shutdown
  InputManager& input = app.getInputManager();

  // Download buffers to CPU
  input.addKeyBinding(
      {GLFW_KEY_D, GLFW_PRESS, GLFW_MOD_CONTROL},
      [&app, that = this]() {
        vkDeviceWaitIdle(app.getDevice());

        std::vector<Particle> particles;
        that->_particleBuffer.download(particles);

        std::vector<uint32_t> spatialHash;
        that->_particleToCellBuffer.download(spatialHash);

        SpatialHashUnitTests::runTests(
            that->_simUniforms.getUniformBuffers()[0].getUniforms(),
            particles,
            spatialHash);
      });

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
        if (that->_simPass.recompileStaleShaders()) {
          if (that->_simPass.hasShaderRecompileErrors()) {
            std::cout << that->_simPass.getShaderRecompileErrors() << "\n";
          } else {
            that->_simPass.recreatePipeline(app);
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

        vkDeviceWaitIdle(app.getDevice());
        that->_resetParticles();
      });

  input.addMousePositionCallback(
      [&adjustingExposure = this->_adjustingExposure,
       &exposure = this->_exposure](double x, double y, bool cursorHidden) {
        if (adjustingExposure) {
          exposure = static_cast<float>(y);
        }
      });
}

void ParticleSystem::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void ParticleSystem::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  SingleTimeCommandBuffer commandBuffer(app);
  this->_createGlobalResources(app, commandBuffer);
  this->_createSimResources(app, commandBuffer);
  this->_createModels(app, commandBuffer);
  this->_createForwardPass(app);
  this->_createDeferredPass(app);
}

void ParticleSystem::destroyRenderState(Application& app) {
  this->_models.clear();

  this->_pForwardPass.reset();
  this->_gBufferResources = {};
  this->_forwardFrameBuffer = {};

  this->_pSimResources.reset();
  this->_simPass = {};
  this->_collisionsPass = {};
  this->_simUniforms = {};
  this->_particleBuffer = {};
  this->_particleToCellBuffer = {};
  this->_sphereVertices = {};
  this->_sphereIndices = {};

  this->_pDeferredPass.reset();
  this->_swapChainFrameBuffers = {};
  this->_pDeferredMaterial.reset();
  this->_pDeferredMaterialAllocator.reset();

  this->_pGlobalResources.reset();
  this->_pGlobalUniforms.reset();
  this->_pointLights = {};
  this->_pGltfMaterialAllocator.reset();
  this->_iblResources = {};

  this->_pSSR.reset();
}

void ParticleSystem::tick(Application& app, const FrameContext& frame) {
  // Use fixed delta time
  float deltaTime = 1.0f / 60.0f;

  this->_pCameraController->tick(deltaTime);
  const Camera& camera = this->_pCameraController->getCamera();

  const glm::mat4& projection = camera.getProjection();

  GlobalUniforms globalUniforms;
  globalUniforms.projection = camera.getProjection();
  globalUniforms.inverseProjection = glm::inverse(globalUniforms.projection);
  globalUniforms.view = camera.computeView();
  globalUniforms.inverseView = glm::inverse(globalUniforms.view);
  globalUniforms.lightCount = static_cast<int>(this->_pointLights.getCount());
  globalUniforms.time = static_cast<float>(frame.currentTime);
  globalUniforms.exposure = this->_exposure;

  this->_pGlobalUniforms->updateUniforms(globalUniforms, frame);

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

  SimUniforms simUniforms{};

  // TODO: Just use spacing scale param??
  // can assume grid is world axis aligned and uniformly scaled on each dim
  // don't care how many cells there are, due to spatial hash
  simUniforms.gridToWorld = glm::scale(glm::mat4(1.0f), glm::vec3(0.05f));
  // simUniforms.gridToWorld[3] = glm::vec4(-100.0f, -100.0f, -100.0f, 1.0f);
  simUniforms.worldToGrid = glm::inverse(simUniforms.gridToWorld);

  simUniforms.particleCount = this->_particleBuffer.getCount();
  simUniforms.spatialHashSize = this->_particleToCellBuffer.getCount();
  simUniforms.spatialHashProbeSteps = 40;

  simUniforms.deltaTime = deltaTime;

  this->_simUniforms.updateUniforms(simUniforms, frame);
}

void ParticleSystem::_createModels(
    Application& /*app*/,
    SingleTimeCommandBuffer& /*commandBuffer*/) {}

void ParticleSystem::_resetParticles() {
  for (uint32_t i = 0; i < this->_particleBuffer.getCount(); ++i) {
    glm::vec3 position =
        0.01f * glm::vec3(rand() % 1000, rand() % 1000, rand() % 1000);
    // position += glm::vec3(10.0f);
    this->_particleBuffer.setElement(
        Particle{// position
                 position,
                 // radius
                 0.049f,
                 // velocity
                 glm::vec3(0.0f),
                 // padding
                 0,
                 // next position
                 position,
                 // debug value
                 0},
        i);
  }

  this->_particleBuffer.upload();
}

void ParticleSystem::_createGlobalResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  this->_iblResources = ImageBasedLighting::createResources(
      app,
      commandBuffer,
      "NeoclassicalInterior");
  this->_gBufferResources = GBufferResources(app);

  // Per-primitive material resources
  {
    DescriptorSetLayoutBuilder primitiveMaterialLayout;
    Primitive::buildMaterial(primitiveMaterialLayout);

    this->_pGltfMaterialAllocator =
        std::make_unique<DescriptorSetAllocator>(app, primitiveMaterialLayout);
  }

  // Global resources
  {
    DescriptorSetLayoutBuilder globalResourceLayout;

    // Add textures for IBL
    ImageBasedLighting::buildLayout(globalResourceLayout);
    globalResourceLayout
        // Global uniforms.
        .addUniformBufferBinding()
        // Point light buffer.
        .addStorageBufferBinding(
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
        // Shadow map texture.
        .addTextureBinding();

    this->_pGlobalResources =
        std::make_unique<PerFrameResources>(app, globalResourceLayout);
    this->_pGlobalUniforms =
        std::make_unique<TransientUniforms<GlobalUniforms>>(app);

    this->_pointLights = PointLightCollection(
        app,
        commandBuffer,
        9,
        true,
        this->_pGltfMaterialAllocator->getLayout());
    for (uint32_t i = 0; i < 3; ++i) {
      for (uint32_t j = 0; j < 3; ++j) {
        PointLight light;
        float t = static_cast<float>(i * 3 + j);

        light.position = 40.0f * glm::vec3(
                                     static_cast<float>(i),
                                     -1.0f,
                                     static_cast<float>(j) * 0.5f);
        light.emission =
            1000.0f * // / static_cast<float>(i + 1) *
            glm::vec3(cos(t) + 1.0f, sin(t + 1.0f) + 1.0f, sin(t) + 1.0f);

        this->_pointLights.setLight(i * 3 + j, light);
      }
    }

    ResourcesAssignment assignment = this->_pGlobalResources->assign();

    // Bind IBL resources
    this->_iblResources.bind(assignment);

    // Bind global uniforms
    assignment.bindTransientUniforms(*this->_pGlobalUniforms);
    assignment.bindStorageBuffer(
        this->_pointLights.getAllocation(),
        this->_pointLights.getByteSize(),
        true);
    assignment.bindTexture(
        this->_pointLights.getShadowMapArrayView(),
        this->_pointLights.getShadowMapSampler());
  }

  // Set up SSR resources
  this->_pSSR = std::make_unique<ScreenSpaceReflection>(
      app,
      commandBuffer,
      this->_pGlobalResources->getLayout(),
      this->_gBufferResources);

  // Deferred pass resources (GBuffer)
  {
    DescriptorSetLayoutBuilder deferredMaterialLayout{};
    this->_gBufferResources.buildMaterial(deferredMaterialLayout);
    // Roughness-filtered reflection buffer
    deferredMaterialLayout.addTextureBinding();

    this->_pDeferredMaterialAllocator =
        std::make_unique<DescriptorSetAllocator>(
            app,
            deferredMaterialLayout,
            1);
    this->_pDeferredMaterial =
        std::make_unique<Material>(app, *this->_pDeferredMaterialAllocator);

    // Bind G-Buffer resources as textures in the deferred pass
    ResourcesAssignment& assignment = this->_pDeferredMaterial->assign();
    this->_gBufferResources.bindTextures(assignment);
    this->_pSSR->bindTexture(assignment);
  }
}

void ParticleSystem::_createSimResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  // TODO: Create particle count constant
  uint32_t particleCount = 10000; // 64000;
  this->_particleBuffer = StructuredBuffer<Particle>(app, particleCount);
  this->_resetParticles();

  // Need transfer bit for vkCmdFillBuffer
  this->_particleToCellBuffer = StructuredBuffer<uint32_t>(
      app,
      4 * particleCount,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT);

  ShapeUtilities::createSphere(
      app,
      commandBuffer,
      this->_sphereVertices,
      this->_sphereIndices);

  this->_simUniforms = TransientUniforms<SimUniforms>(app);

  DescriptorSetLayoutBuilder matBuilder{};
  // Simulation uniforms
  matBuilder.addUniformBufferBinding(VK_SHADER_STAGE_ALL);
  // Particle buffer
  matBuilder.addStorageBufferBinding(VK_SHADER_STAGE_ALL);
  // Particle to cell map
  matBuilder.addStorageBufferBinding(VK_SHADER_STAGE_ALL);

  this->_pSimResources = std::make_unique<PerFrameResources>(app, matBuilder);
  this->_pSimResources->assign()
      .bindTransientUniforms(this->_simUniforms)
      .bindStorageBuffer(
          this->_particleBuffer.getAllocation(),
          this->_particleBuffer.getSize(),
          false)
      .bindStorageBuffer(
          this->_particleToCellBuffer.getAllocation(),
          this->_particleToCellBuffer.getSize(),
          false);

  ShaderDefines shaderDefs{};
  {
    ComputePipelineBuilder builder;
    builder.setComputeShader(
        GProjectDirectory + "/Shaders/ParticleSystem/ParticleSystem.comp.glsl",
        shaderDefs);
    builder.layoutBuilder.addDescriptorSet(this->_pSimResources->getLayout());

    this->_simPass = ComputePipeline(app, std::move(builder));
  }

  {
    ComputePipelineBuilder builder;
    builder.setComputeShader(
        GProjectDirectory +
            "/Shaders/ParticleSystem/HandleCollisions.comp.glsl",
        shaderDefs);
    builder.layoutBuilder.addDescriptorSet(this->_pSimResources->getLayout());

    this->_collisionsPass = ComputePipeline(app, std::move(builder));
  }
}

void ParticleSystem::_createForwardPass(Application& app) {
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

    subpassBuilder
        .pipelineBuilder
        // Vertex shader
        .addVertexShader(GEngineDirectory + "/Shaders/GltfForward.vert")
        // Fragment shader
        .addFragmentShader(GEngineDirectory + "/Shaders/GltfForward.frag")

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_pGlobalResources->getLayout())
        // Material (per-object) resources (diffuse, normal map,
        // metallic-roughness, etc)
        .addDescriptorSet(this->_pGltfMaterialAllocator->getLayout());
  }

  // Render particles
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    // The GBuffer contains the following color attachments
    // 1. Position
    // 2. Normal
    // 3. Albedo
    // 4. Metallic-Roughness-Occlusion
    subpassBuilder.colorAttachments = {0, 1, 2, 3};
    subpassBuilder.depthAttachment = 4;

    subpassBuilder.pipelineBuilder.setPrimitiveType(PrimitiveType::TRIANGLES)
        .addVertexInputBinding<glm::vec3>(VK_VERTEX_INPUT_RATE_VERTEX)
        .addVertexAttribute(VertexAttributeType::VEC3, 0)

        .addVertexShader(
            GProjectDirectory + "/Shaders/ParticleSystem/Particles.vert")
        .addFragmentShader(
            GProjectDirectory + "/Shaders/ParticleSystem/Particles.frag")
        .layoutBuilder.addDescriptorSet(this->_pGlobalResources->getLayout())
        .addDescriptorSet(this->_pSimResources->getLayout());
  }

  // Render floor
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    // The GBuffer contains the following color attachments
    // 1. Position
    // 2. Normal
    // 3. Albedo
    // 4. Metallic-Roughness-Occlusion
    subpassBuilder.colorAttachments = {0, 1, 2, 3};
    subpassBuilder.depthAttachment = 4;

    subpassBuilder.pipelineBuilder.setPrimitiveType(PrimitiveType::TRIANGLES)
        .setCullMode(VK_CULL_MODE_FRONT_BIT)
        .addVertexShader(GEngineDirectory + "/Shaders/Misc/FullScreenQuad.vert")
        .addFragmentShader(GEngineDirectory + "/Shaders/Misc/Floor.frag")
        .layoutBuilder.addDescriptorSet(this->_pGlobalResources->getLayout());
  }

  std::vector<Attachment> attachments =
      this->_gBufferResources.getAttachmentDescriptions();
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
      this->_gBufferResources.getAttachmentViews());
}

void ParticleSystem::_createDeferredPass(Application& app) {
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

  // DEFERRED PBR PASS
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments.push_back(0);

    subpassBuilder.pipelineBuilder.setCullMode(VK_CULL_MODE_FRONT_BIT)
        .setDepthTesting(false)

        // Vertex shader
        .addVertexShader(GProjectDirectory + "/Shaders/DeferredPass.vert")
        // Fragment shader
        .addFragmentShader(GProjectDirectory + "/Shaders/DeferredPass.frag")

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_pGlobalResources->getLayout())
        // GBuffer material (position, normal, albedo,
        // metallic-roughness-occlusion)
        .addDescriptorSet(this->_pDeferredMaterialAllocator->getLayout());
  }

  // SHOW POINT LIGHTS (kinda hacky)
  // TODO: Really light objects should be rendered in the forward
  // pass as well and an emissive channel should be added to the
  // G-Buffer
  this->_pointLights.setupPointLightMeshSubpass(
      subpassBuilders.emplace_back(),
      0,
      1,
      this->_pGlobalResources->getLayout());

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

void ParticleSystem::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {

  vkCmdFillBuffer(
      commandBuffer,
      this->_particleToCellBuffer.getAllocation().getBuffer(),
      0,
      this->_particleToCellBuffer.getSize(),
      0xFFFFFFFF);

  {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.buffer = this->_particleToCellBuffer.getAllocation().getBuffer();
    barrier.offset = 0;
    barrier.size = this->_particleToCellBuffer.getSize();
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        1,
        &barrier,
        0,
        nullptr);
  }

  {
    VkDescriptorSet set = this->_pSimResources->getCurrentDescriptorSet(frame);

    uint32_t groupCountX = (this->_particleBuffer.getCount() - 1) / 32 + 1;

    this->_simPass.bindPipeline(commandBuffer);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        this->_simPass.getLayout(),
        0,
        1,
        &set,
        0,
        nullptr);
    vkCmdDispatch(commandBuffer, groupCountX, 1, 1);

    VkBufferMemoryBarrier barriers[2] = {};
    barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[0].buffer = this->_particleBuffer.getAllocation().getBuffer();
    barriers[0].offset = 0;
    barriers[0].size = this->_particleBuffer.getSize();
    barriers[0].srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barriers[0].dstAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[1].buffer =
        this->_particleToCellBuffer.getAllocation().getBuffer();
    barriers[1].offset = 0;
    barriers[1].size = this->_particleToCellBuffer.getSize();
    barriers[1].srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        2,
        barriers,
        0,
        nullptr);

    this->_collisionsPass.bindPipeline(commandBuffer);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        this->_collisionsPass.getLayout(),
        0,
        1,
        &set,
        0,
        nullptr);
    vkCmdDispatch(commandBuffer, groupCountX, 1, 1);
  }

  {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.buffer = this->_particleBuffer.getAllocation().getBuffer();
    barrier.offset = 0;
    barrier.size = this->_particleBuffer.getSize();
    barrier.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

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

  this->_pointLights.updateResource(frame);
  this->_gBufferResources.transitionToAttachment(commandBuffer);

  VkDescriptorSet globalDescriptorSet =
      this->_pGlobalResources->getCurrentDescriptorSet(frame);

  // Draw point light shadow maps
  this->_pointLights.drawShadowMaps(app, commandBuffer, frame, this->_models);

  // Forward pass
  {
    ActiveRenderPass pass = this->_pForwardPass->begin(
        app,
        commandBuffer,
        frame,
        this->_forwardFrameBuffer);
    // Bind global descriptor sets
    pass.setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));
    // Draw models
    for (const Model& model : this->_models) {
      pass.draw(model);
    }

    VkDescriptorSet sets[2] = {
        globalDescriptorSet,
        this->_pSimResources->getCurrentDescriptorSet(frame)};

    // Draw instanced particles
    pass.nextSubpass();
    pass.setGlobalDescriptorSets(gsl::span(sets, 2));
    pass.getDrawContext().bindDescriptorSets();
    pass.getDrawContext().bindIndexBuffer(this->_sphereIndices);
    pass.getDrawContext().bindVertexBuffer(this->_sphereVertices);
    pass.getDrawContext().drawIndexed(
        this->_sphereIndices.getIndexCount(),
        this->_particleBuffer.getCount());

    // Draw floor
    pass.nextSubpass();
    pass.setGlobalDescriptorSets(gsl::span(sets, 1));
    pass.getDrawContext().bindDescriptorSets();
    pass.getDrawContext().draw(3);
  }

  this->_gBufferResources.transitionToTextures(commandBuffer);

  // Reflection buffer and convolution
  {
    this->_pSSR
        ->captureReflection(app, commandBuffer, globalDescriptorSet, frame);
    this->_pSSR->convolveReflectionBuffer(app, commandBuffer, frame);
  }

  // Deferred pass
  {
    ActiveRenderPass pass = this->_pDeferredPass->begin(
        app,
        commandBuffer,
        frame,
        this->_swapChainFrameBuffers.getCurrentFrameBuffer(frame));
    // Bind global descriptor sets
    pass.setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));

    {
      const DrawContext& context = pass.getDrawContext();
      context.bindDescriptorSets(*this->_pDeferredMaterial);
      context.draw(3);
    }

    pass.nextSubpass();
    pass.setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));
    pass.draw(this->_pointLights);
  }
}
} // namespace ParticleSystem
} // namespace AltheaDemo