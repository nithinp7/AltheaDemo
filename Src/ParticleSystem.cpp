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

#define PARTICLE_COUNT 1000000      // 500000 // 200000
#define PARTICLES_PER_BUFFER 100000 // 50000

#define SPATIAL_HASH_SIZE (PARTICLE_COUNT)
#define SPATIAL_HASH_ENTRIES_PER_BUFFER (PARTICLES_PER_BUFFER)

#define JACOBI_ITERS 7
#define PARTICLE_RADIUS 0.1f

#define LOCAL_SIZE_X 1024

#define INSTANCED_MODE

#define GEN_SHADER_DEBUG_INFO

#define INPUT_MASK_MOUSE_LEFT 1
#define INPUT_MASK_MOUSE_RIGHT 2

namespace {
struct SolverPushConstants {
  uint32_t phase;
};
} // namespace
namespace AltheaDemo {
namespace ParticleSystem {

ParticleSystem::ParticleSystem() {}

void ParticleSystem::initGame(Application& app) {
  const VkExtent2D& windowDims = app.getSwapChainExtent();
  this->_pCameraController = std::make_unique<CameraController>(
      app.getInputManager(),
      90.0f,
      (float)windowDims.width / (float)windowDims.height);
  this->_pCameraController->setMaxSpeed(50.0f);

  // TODO: need to unbind these at shutdown
  InputManager& input = app.getInputManager();
  
  input.addMouseBinding({GLFW_MOUSE_BUTTON_2, GLFW_PRESS, 0}, [that = this]() {
    that->_inputMask |= INPUT_MASK_MOUSE_LEFT;
  });
  input.addMouseBinding({GLFW_MOUSE_BUTTON_2, GLFW_RELEASE, 0}, [that = this]() {
    that->_inputMask &= ~INPUT_MASK_MOUSE_LEFT;
  });
  input.addMouseBinding({GLFW_MOUSE_BUTTON_1, GLFW_PRESS, 0}, [that = this]() {
    that->_inputMask |= INPUT_MASK_MOUSE_RIGHT;
  });
  input.addMouseBinding({GLFW_MOUSE_BUTTON_1, GLFW_RELEASE, 0}, [that = this]() {
    that->_inputMask &= ~INPUT_MASK_MOUSE_RIGHT;
  });

  // Download buffers to CPU
  input.addKeyBinding(
      {GLFW_KEY_D, GLFW_PRESS, GLFW_MOD_CONTROL},
      [&app, that = this]() {
        vkDeviceWaitIdle(app.getDevice());

        // TODO: need to fix up how this works since a buffer heap is used now

        // std::vector<Particle> particles;
        // that->_particleBuffer.download(particles);

        // std::vector<uint32_t> spatialHash;
        // that->_cellToBucket.download(spatialHash);

        // TODO: Verify that the ringbuffer usage is correct here before
        // uncommenting SpatialHashUnitTests::runTests(
        //     that->_simUniforms.getUniformBuffers()[app.getCurrentFrameRingBufferIndex()].getUniforms(),
        //     particles,
        //     spatialHash);
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

  input.addKeyBinding({GLFW_KEY_P, GLFW_RELEASE, 0}, [that = this, &app]() {
    vkDeviceWaitIdle(app.getDevice());
    that->_resetParticles(app, SingleTimeCommandBuffer(app));
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

        if (that->_jacobiStep.recompileStaleShaders()) {
          if (that->_jacobiStep.hasShaderRecompileErrors()) {
            std::cout << that->_jacobiStep.getShaderRecompileErrors() << "\n";
          } else {
            that->_jacobiStep.recreatePipeline(app);
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
      });
  input.addMousePositionCallback(
      [&adjustingExposure = this->_adjustingExposure,
       &exposure = this->_exposure](double x, double y, bool cursorHidden) {
        if (adjustingExposure) {
          exposure = static_cast<float>(y);
        }
      });

#ifdef GEN_SHADER_DEBUG_INFO
  Shader::setShouldGenerateDebugInfo(true);
#endif
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
  this->_jacobiStep = {};
  this->_simUniforms = {};
  this->_particleBuffer = {};
  this->_spatialHash = {};
  this->_positionsA = {};
  this->_positionsB = {};

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

  this->_pCameraController->tick(frame.deltaTime);
  const Camera& camera = this->_pCameraController->getCamera();

  // Use fixed timestep for physics
  float deltaTime = 1.0f / 15.0f;//glm::max(1.0f / 60.0f, frame.deltaTime);

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
  simUniforms.gridToWorld =
      glm::scale(glm::mat4(1.0f), glm::vec3(2.0f * PARTICLE_RADIUS));
  // simUniforms.gridToWorld = glm::scale(glm::mat4(1.0f), glm::vec3(0.05f));
  // simUniforms.gridToWorld[3] = glm::vec4(-100.0f, -100.0f, -100.0f, 1.0f);
  simUniforms.worldToGrid = glm::inverse(simUniforms.gridToWorld);

  simUniforms.inverseView = globalUniforms.inverseView;
  if (this->_inputMask & INPUT_MASK_MOUSE_LEFT)
  {
    // TODO:
  }

  simUniforms.inputMask = this->_inputMask;

  if (this->_inputMask & INPUT_MASK_MOUSE_RIGHT) {
    simUniforms.addedParticles = 100;
  } else {
    simUniforms.addedParticles = 0;
  }

  this->_activeParticleCount += simUniforms.addedParticles;
  if (this->_activeParticleCount > PARTICLE_COUNT) {
    simUniforms.addedParticles = PARTICLE_COUNT - this->_activeParticleCount;
    this->_activeParticleCount = PARTICLE_COUNT;
  }

  simUniforms.particleCount = this->_activeParticleCount;
  simUniforms.particlesPerBuffer = PARTICLES_PER_BUFFER;
  simUniforms.spatialHashSize = SPATIAL_HASH_SIZE;
  simUniforms.spatialHashEntriesPerBuffer = SPATIAL_HASH_ENTRIES_PER_BUFFER;

  simUniforms.jacobiIters = JACOBI_ITERS;
  simUniforms.deltaTime = deltaTime;
  simUniforms.particleRadius = PARTICLE_RADIUS;
  simUniforms.time = frame.currentTime;

  this->_simUniforms.updateUniforms(simUniforms, frame);
}

void ParticleSystem::_createModels(
    Application& /*app*/,
    SingleTimeCommandBuffer& /*commandBuffer*/) {}

void ParticleSystem::_resetParticles(
    const Application& app,
    VkCommandBuffer commandBuffer) {
  for (uint32_t particleIdx = 0; particleIdx < PARTICLE_COUNT; ++particleIdx) {
    uint32_t bufferIdx = particleIdx / PARTICLES_PER_BUFFER;
    uint32_t localIdx = particleIdx % PARTICLES_PER_BUFFER;

    glm::vec3 position(rand() % 10, rand() % 10, rand() % 10);
    // glm::vec3 position(rand() % 25, rand() % 20, rand() % 150);
    // position += glm::vec3(10.0f);
    this->_particleBuffer.getBuffer(bufferIdx).setElement(
        Particle{// position
                 position,
                 // next particle in particle bucket linked-list
                 0,
                 // debug value
                 0},
        localIdx);

    glm::vec4 position4(position, 1.0f);
    this->_positionsA.getBuffer(bufferIdx).setElement(position4, localIdx);
    this->_positionsB.getBuffer(bufferIdx).setElement(position4, localIdx);
  }

  this->_particleBuffer.upload(app, commandBuffer);
  this->_positionsA.upload(app, commandBuffer);
  this->_positionsB.upload(app, commandBuffer);
}

void ParticleSystem::_createGlobalResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  this->_iblResources = ImageBasedLighting::createResources(
      app,
      commandBuffer,
      "NeoclassicalInterior");
  this->_gBufferResources = GBufferResources(app);

  // TODO: Create LODs for particles
  ShapeUtilities::createSphere(
      app,
      commandBuffer,
      this->_sphereVertices,
      this->_sphereIndices,
      6);

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
        .addTextureBinding()
        // Sphere indices
        .addStorageBufferBinding(VK_SHADER_STAGE_VERTEX_BIT)
        // Sphere verts
        .addStorageBufferBinding(VK_SHADER_STAGE_VERTEX_BIT);

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
    assignment.bindStorageBuffer(
        this->_sphereIndices.getAllocation(),
        this->_sphereIndices.getSize(),
        false);
    assignment.bindStorageBuffer(
        this->_sphereVertices.getAllocation(),
        this->_sphereVertices.getSize(),
        false);
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
  uint32_t particleBufferCount =
      (PARTICLE_COUNT - 1) / PARTICLES_PER_BUFFER + 1;
  std::vector<StructuredBuffer<Particle>> particleBufferHeap;
  particleBufferHeap.reserve(particleBufferCount);
  std::vector<StructuredBuffer<glm::vec4>> positionsBufferAHeap;
  positionsBufferAHeap.reserve(particleBufferCount);
  std::vector<StructuredBuffer<glm::vec4>> positionsBufferBHeap;
  positionsBufferBHeap.reserve(particleBufferCount);

  // For now the last buffer could be overallocated if the particles-per-buffer
  // value doesn't perfectly divide the particle count
  for (uint32_t bufferIdx = 0; bufferIdx < particleBufferCount; ++bufferIdx) {
    particleBufferHeap.emplace_back(app, PARTICLES_PER_BUFFER);
    positionsBufferAHeap.emplace_back(app, PARTICLES_PER_BUFFER);
    positionsBufferBHeap.emplace_back(app, PARTICLES_PER_BUFFER);
  }

  this->_particleBuffer =
      StructuredBufferHeap<Particle>(std::move(particleBufferHeap));
  this->_positionsA =
      StructuredBufferHeap<glm::vec4>(std::move(positionsBufferAHeap));
  this->_positionsB =
      StructuredBufferHeap<glm::vec4>(std::move(positionsBufferBHeap));

  this->_resetParticles(app, commandBuffer);

  // Need transfer bit for vkCmdFillBuffer
  uint32_t spatialHashBufferCount =
      (SPATIAL_HASH_SIZE - 1) / SPATIAL_HASH_ENTRIES_PER_BUFFER + 1;
  std::vector<StructuredBuffer<uint32_t>> spatialHashHeap;
  spatialHashHeap.reserve(spatialHashBufferCount);
  for (uint32_t bufferIdx = 0; bufferIdx < spatialHashBufferCount;
       ++bufferIdx) {
    spatialHashHeap.emplace_back(app, SPATIAL_HASH_ENTRIES_PER_BUFFER);
  }

  this->_spatialHash =
      StructuredBufferHeap<uint32_t>(std::move(spatialHashHeap));

  this->_simUniforms = TransientUniforms<SimUniforms>(app);

  DescriptorSetLayoutBuilder matBuilder{};
  // Simulation uniforms
  matBuilder.addUniformBufferBinding(VK_SHADER_STAGE_ALL);
  // Particle buffer
  matBuilder.addBufferHeapBinding(
      this->_particleBuffer.getSize(),
      VK_SHADER_STAGE_ALL);
  // Spatial hash
  matBuilder.addBufferHeapBinding(
      this->_spatialHash.getSize(),
      VK_SHADER_STAGE_ALL);
  // Positions A
  matBuilder.addBufferHeapBinding(
      this->_positionsA.getSize(),
      VK_SHADER_STAGE_ALL);
  // Positions B
  matBuilder.addBufferHeapBinding(
      this->_positionsB.getSize(),
      VK_SHADER_STAGE_ALL);

  this->_pSimResources = std::make_unique<PerFrameResources>(app, matBuilder);
  this->_pSimResources->assign()
      .bindTransientUniforms(this->_simUniforms)
      .bindBufferHeap(this->_particleBuffer)
      .bindBufferHeap(this->_spatialHash)
      .bindBufferHeap(this->_positionsA)
      .bindBufferHeap(this->_positionsB);

  ShaderDefines shaderDefs{};
  shaderDefs.emplace("LOCAL_SIZE_X", std::to_string(LOCAL_SIZE_X));
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
            "/Shaders/ParticleSystem/ProjectedJacobiStep.comp.glsl",
        shaderDefs);
    builder.layoutBuilder.addDescriptorSet(this->_pSimResources->getLayout())
        .addPushConstants<SolverPushConstants>(VK_SHADER_STAGE_COMPUTE_BIT);

    this->_jacobiStep = ComputePipeline(app, std::move(builder));
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
    ShaderDefines defs{};
#ifdef INSTANCED_MODE
    defs.emplace("INSTANCED_MODE", "");
#endif

    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    // The GBuffer contains the following color attachments
    // 1. Position
    // 2. Normal
    // 3. Albedo
    // 4. Metallic-Roughness-Occlusion
    subpassBuilder.colorAttachments = {0, 1, 2, 3};
    subpassBuilder.depthAttachment = 4;

    subpassBuilder.pipelineBuilder
        .setPrimitiveType(PrimitiveType::TRIANGLES)
#ifdef INSTANCED_MODE
        .addVertexInputBinding<glm::vec3>(VK_VERTEX_INPUT_RATE_VERTEX)
        .addVertexAttribute(VertexAttributeType::VEC3, 0)
#endif
        .addVertexShader(
            GProjectDirectory + "/Shaders/ParticleSystem/Particles.vert",
            defs)
        .addFragmentShader(
            GProjectDirectory + "/Shaders/ParticleSystem/Particles.frag")
        .layoutBuilder.addDescriptorSet(this->_pGlobalResources->getLayout())
        .addDescriptorSet(this->_pSimResources->getLayout())
        .addPushConstants<uint32_t>(
            VK_SHADER_STAGE_VERTEX_BIT); // sphere index count
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
  uint32_t spatialHashBufferCount = this->_spatialHash.getSize();
  uint32_t particleBufferCount = this->_particleBuffer.getSize();

  // Reset the spatial hash and prepare the buffers for read/write
  {
    static std::vector<VkBufferMemoryBarrier> spatialHashResetBarriers;
    spatialHashResetBarriers.resize(spatialHashBufferCount);

    for (uint32_t bufferIdx = 0; bufferIdx < spatialHashBufferCount;
         ++bufferIdx) {
      const auto& buffer = this->_spatialHash.getBuffer(bufferIdx);

      vkCmdFillBuffer(
          commandBuffer,
          buffer.getAllocation().getBuffer(),
          0,
          buffer.getSize(),
          0xFFFFFFFF);

      VkBufferMemoryBarrier& barrier = spatialHashResetBarriers[bufferIdx];
      barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      barrier.buffer = buffer.getAllocation().getBuffer();
      barrier.offset = 0;
      barrier.size = buffer.getSize();
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask =
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    }

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        spatialHashResetBarriers.size(),
        spatialHashResetBarriers.data(),
        0,
        nullptr);
  }

  VkDescriptorSet set = this->_pSimResources->getCurrentDescriptorSet(frame);
  uint32_t groupCountX = (this->_activeParticleCount - 1) / LOCAL_SIZE_X + 1;

  // Dispatch particle registration into the spatial hash and prepare the
  // spatial hash and particle buckets (linked-lists) for read-only access
  {
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

    static std::vector<VkBufferMemoryBarrier> postRegistrationBarriers;
    postRegistrationBarriers.resize(
        particleBufferCount + spatialHashBufferCount);

    for (uint32_t bufferIdx = 0; bufferIdx < particleBufferCount; ++bufferIdx) {
      const auto& buffer = this->_particleBuffer.getBuffer(bufferIdx);

      VkBufferMemoryBarrier& barrier = postRegistrationBarriers[bufferIdx];
      barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      barrier.buffer = buffer.getAllocation().getBuffer();
      barrier.offset = 0;
      barrier.size = buffer.getSize();
      barrier.srcAccessMask =
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }

    for (uint32_t bufferIdx = 0; bufferIdx < spatialHashBufferCount;
         ++bufferIdx) {
      const auto& buffer = this->_spatialHash.getBuffer(bufferIdx);

      VkBufferMemoryBarrier& barrier =
          postRegistrationBarriers[bufferIdx + particleBufferCount];
      barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      barrier.buffer = buffer.getAllocation().getBuffer();
      barrier.offset = 0;
      barrier.size = buffer.getSize();
      barrier.srcAccessMask =
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        // The particle positions are currently used to render the particles as
        // well currently consider splitting out particles and the hash into
        // separate barriers
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        0,
        0,
        nullptr,
        postRegistrationBarriers.size(),
        postRegistrationBarriers.data(),
        0,
        nullptr);
  }

  // Dispatch jacobi iterations for collision resolution
  {
    static std::vector<VkBufferMemoryBarrier> pingPongBarriers;
    pingPongBarriers.resize(2 * particleBufferCount);

    for (uint32_t bufferIdx = 0; bufferIdx < particleBufferCount; ++bufferIdx) {
      const auto& positionsA = this->_positionsA.getBuffer(bufferIdx);

      VkBufferMemoryBarrier& barrierA = pingPongBarriers[2 * bufferIdx];
      barrierA = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      barrierA.buffer = positionsA.getAllocation().getBuffer();
      barrierA.offset = 0;
      barrierA.size = positionsA.getSize();
      barrierA.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrierA.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

      const auto& positionsB = this->_positionsB.getBuffer(bufferIdx);
      VkBufferMemoryBarrier& barrierB = pingPongBarriers[2 * bufferIdx + 1];
      barrierB = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      barrierB.buffer = positionsB.getAllocation().getBuffer();
      barrierB.offset = 0;
      barrierB.size = positionsB.getSize();
      barrierB.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barrierB.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    }

    this->_jacobiStep.bindPipeline(commandBuffer);
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        this->_jacobiStep.getLayout(),
        0,
        1,
        &set,
        0,
        nullptr);
    for (uint32_t iter = 0; iter < JACOBI_ITERS; ++iter) {
      vkCmdPipelineBarrier(
          commandBuffer,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0,
          0,
          nullptr,
          pingPongBarriers.size(),
          pingPongBarriers.data(),
          0,
          nullptr);

      SolverPushConstants constants{};
      constants.phase = iter % 2;
      vkCmdPushConstants(
          commandBuffer,
          this->_jacobiStep.getLayout(),
          VK_SHADER_STAGE_COMPUTE_BIT,
          0,
          sizeof(SolverPushConstants),
          &constants);
      vkCmdDispatch(commandBuffer, groupCountX, 1, 1);

      // Swap the barrier access direction for the next iteration
      for (auto& barrier : pingPongBarriers)
        std::swap(barrier.srcAccessMask, barrier.dstAccessMask);
    }
  }

  // this->_pointLights.updateResource(frame);
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
    pass.getDrawContext().updatePushConstants<uint32_t>(
        this->_sphereIndices.getIndexCount(),
        0);
    pass.setGlobalDescriptorSets(gsl::span(sets, 2));
    pass.getDrawContext().bindDescriptorSets();
#ifdef INSTANCED_MODE
    pass.getDrawContext().bindIndexBuffer(this->_sphereIndices);
    pass.getDrawContext().bindVertexBuffer(this->_sphereVertices);
    pass.getDrawContext().drawIndexed(
        this->_sphereIndices.getIndexCount(),
        this->_activeParticleCount);
#else
    pass.getDrawContext().draw(
        this->_sphereIndices.getIndexCount() * this->_activeParticleCount);
#endif
    // this->_sphereIndices.getIndexCount(),
    // PARTICLE_COUNT);

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
    // pass.setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));
    // pass.draw(this->_pointLights);
  }
}
} // namespace ParticleSystem
} // namespace AltheaDemo