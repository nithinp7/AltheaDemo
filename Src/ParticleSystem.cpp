#include "ParticleSystem.h"

#include "SpatialHashUnitTests.h"

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

#define PARTICLE_COUNT 5000000     // 500000 // 200000
#define PARTICLES_PER_BUFFER 50000 // 100000 // 50000

#define SPATIAL_HASH_SIZE (3 * PARTICLE_COUNT)
#define SPATIAL_HASH_ENTRIES_PER_BUFFER (PARTICLE_COUNT / 4)

// TODO: Probably waaay too many
#define PARTICLE_BUCKET_COUNT 5000000      //(PARTICLE_COUNT / 4)
#define PARTICLE_BUCKETS_PER_BUFFFER 10000 //(PARTICLE_BUCKET_COUNT)

#define TIME_SUBSTEPS 2
#define JACOBI_ITERS 2
#define PARTICLE_RADIUS 0.1f

#define LOCAL_SIZE_X 32

#define GEN_SHADER_DEBUG_INFO

namespace AltheaDemo {
namespace ParticleSystem {
struct DeferredPassPushConstants {
  uint32_t globalResources;
  uint32_t globalUniforms;
  uint32_t reflectionBuffer;
  uint32_t writeIndex;
};

ParticleSystem::ParticleSystem() {}

void ParticleSystem::initGame(Application& app) {
  const VkExtent2D& windowDims = app.getSwapChainExtent();
  m_pCameraController = std::make_unique<CameraController>(
      app.getInputManager(),
      90.0f,
      (float)windowDims.width / (float)windowDims.height);
  m_pCameraController->setMaxSpeed(50.0f);

  // TODO: need to unbind these at shutdown
  InputManager& input = app.getInputManager();

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

  input.addKeyBinding({GLFW_KEY_P, GLFW_RELEASE, 0}, [that = this, &app]() {
    vkDeviceWaitIdle(app.getDevice());
    that->_resetParticles(app, SingleTimeCommandBuffer(app));
  });

  // Recreate any stale pipelines (shader hot-reload)
  input.addKeyBinding(
      {GLFW_KEY_R, GLFW_PRESS, GLFW_MOD_CONTROL},
      [&app, that = this]() {
        for (auto& pipeline : that->m_computePasses)
          pipeline.tryRecompile(app);

        that->m_gBufferPass.tryRecompile(app);
        that->m_deferredPass.tryRecompile(app);
      });
  input.addMousePositionCallback(
      [&adjustingExposure = m_adjustingExposure,
       &exposure = m_exposure](double x, double y, bool cursorHidden) {
        if (adjustingExposure) {
          exposure = static_cast<float>(y);
        }
      });

#ifdef GEN_SHADER_DEBUG_INFO
  Shader::setShouldGenerateDebugInfo(true);
#endif
}

void ParticleSystem::shutdownGame(Application& app) {
  m_pCameraController.reset();
}

void ParticleSystem::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  m_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  Gui::createRenderState(app);

  SingleTimeCommandBuffer commandBuffer(app);
  _createGlobalResources(app, commandBuffer);
  _createSimResources(app, commandBuffer);
  _createModels(app, commandBuffer);
  _createGBufferPass(app);
  _createDeferredPass(app);
}

void ParticleSystem::destroyRenderState(Application& app) {
  Gui::destroyRenderState(app);

  m_models.clear();

  m_gBufferPass = {};
  m_gBufferFrameBufferA = {};
  m_gBufferFrameBufferB = {};

  m_computePasses.clear();
  m_simUniforms = {};
  m_particleBuffer = {};
  m_spatialHash = {};
  m_freeBucketCounter = {};
  m_buckets = {};

  m_sphere = {};

  m_deferredPass = {};
  m_swapChainFrameBuffers = {};

  m_globalResources = {};
  m_globalUniforms = {};

  m_ssr = {};
  m_heap = {};
}

static LiveValues s_liveValues;

static void updateUi() {
  Gui::startRecordingImgui();
  const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(main_viewport->WorkPos.x + 650, main_viewport->WorkPos.y + 20),
      ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(440, 200), ImGuiCond_FirstUseEver);

  if (ImGui::Begin("Live Edit")) {
    ImGui::Text("Slider1:");
    ImGui::SliderFloat("##slider1", &s_liveValues.slider1, 0.0, 1.0);
    ImGui::Text("Slider2:");
    ImGui::SliderFloat("##slider2", &s_liveValues.slider2, 0.0, 1.0);
    ImGui::Text("Checkbox1:");
    ImGui::Checkbox("##checkbox1", &s_liveValues.checkbox1);
    ImGui::Text("Checkbox2:");
    ImGui::Checkbox("##checkbox2", &s_liveValues.checkbox2);
  }

  ImGui::End();

  Gui::finishRecordingImgui();
}

void ParticleSystem::tick(Application& app, const FrameContext& frame) {
  updateUi();

  // Use fixed delta time

  const Camera& camera = m_pCameraController->getCamera();

  // Use fixed timestep for physics
  float deltaTime = 1.0f / 15.0f / float(TIME_SUBSTEPS);

  const glm::mat4& projection = camera.getProjection();

  uint32_t inputMask = app.getInputManager().getCurrentInputMask();

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
  globalUniforms.inputMask = inputMask;

  m_globalUniforms.getCurrentUniformBuffer(frame).updateUniforms(
      globalUniforms);

  SimUniforms simUniforms{};

  // TODO: Just use spacing scale param??
  // can assume grid is world axis aligned and uniformly scaled on each dim
  // don't care how many cells there are, due to spatial hash
  simUniforms.gridToWorld =
      glm::scale(glm::mat4(1.0f), glm::vec3(2.f * PARTICLE_RADIUS));
  // simUniforms.gridToWorld = glm::scale(glm::mat4(1.0f), glm::vec3(0.05f));
  // simUniforms.gridToWorld[3] = glm::vec4(-100.0f, -100.0f, -100.0f, 1.0f);
  simUniforms.worldToGrid = glm::inverse(simUniforms.gridToWorld);

  if (m_flagReset) {
    m_flagReset = false;
    simUniforms.addedParticles = m_activeParticleCount;
  } else if (inputMask & INPUT_BIT_RIGHT_MOUSE) {
    simUniforms.addedParticles = 1000;

    m_activeParticleCount += simUniforms.addedParticles;
    if (m_activeParticleCount > PARTICLE_COUNT) {
      simUniforms.addedParticles = PARTICLE_COUNT - m_activeParticleCount;
      m_activeParticleCount = PARTICLE_COUNT;
    }
  } else {
    simUniforms.addedParticles = 0;
  }

  simUniforms.particleCount = m_activeParticleCount;
  simUniforms.particlesPerBuffer = PARTICLES_PER_BUFFER;
  simUniforms.spatialHashSize = SPATIAL_HASH_SIZE;
  simUniforms.spatialHashEntriesPerBuffer = SPATIAL_HASH_ENTRIES_PER_BUFFER;
  simUniforms.particleBucketCount = PARTICLE_BUCKET_COUNT;
  simUniforms.particleBucketsPerBuffer = PARTICLE_BUCKETS_PER_BUFFFER;
  simUniforms.freeListsCount = m_freeBucketCounter.getCount();

  simUniforms.jacobiIters = JACOBI_ITERS;
  simUniforms.deltaTime = deltaTime;
  simUniforms.particleRadius = PARTICLE_RADIUS;
  simUniforms.time = frame.currentTime;

  simUniforms.particlesHeap = m_particleBuffer.getBuffer(0).getHandle().index;
  simUniforms.spatialHashHeap = m_spatialHash.getBuffer(0).getHandle().index;
  simUniforms.bucketHeap = m_buckets.getBuffer(0).getHandle().index;
  simUniforms.nextFreeBucket = m_freeBucketCounter.getHandle().index;

  simUniforms.liveValues = s_liveValues;

  m_simUniforms.updateUniforms(simUniforms, frame);

  m_push.globalResourcesHandle = m_globalResources.getHandle().index;
  m_push.globalUniformsHandle =
      m_globalUniforms.getCurrentBindlessHandle(frame).index;
  m_push.simUniformsHandle = m_simUniforms.getCurrentHandle(frame).index;
}

void ParticleSystem::_createModels(
    Application& /*app*/,
    SingleTimeCommandBuffer& /*commandBuffer*/) {}

void ParticleSystem::_resetParticles(
    Application& app,
    VkCommandBuffer commandBuffer) {
  for (uint32_t particleIdx = 0; particleIdx < PARTICLE_COUNT; ++particleIdx) {
    uint32_t bufferIdx = particleIdx / PARTICLES_PER_BUFFER;
    uint32_t localIdx = particleIdx % PARTICLES_PER_BUFFER;

    // glm::vec3 position(rand() % 10, rand() % 10, rand() % 10);
    glm::vec3 position(rand() % 300, rand() % 3000, rand() % 300);
    position *= 0.1f;
    position += glm::vec3(35.0);

    // position += glm::vec3(10.0f);
    m_particleBuffer.getBuffer(bufferIdx).setElement(
        Particle{// position
                 position,
                 0,
                 position,
                 // debug value
                 0xfc3311},
        localIdx);
  }

  m_particleBuffer.upload(app, commandBuffer);

  m_flagReset = true;
}

void ParticleSystem::_createGlobalResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  m_heap = GlobalHeap(app);

  GlobalResourcesBuilder resourcesBuilder{};
  m_globalResources =
      GlobalResources(app, commandBuffer, m_heap, resourcesBuilder);
  m_globalUniforms = GlobalUniformsResource(app, m_heap);

  // TODO: Create LODs for particles
  ShapeUtilities::createSphere(
      app,
      commandBuffer,
      m_sphere.vertices,
      m_sphere.indices,
      6,
      1.3f * PARTICLE_RADIUS);

  // Set up SSR resources
  m_ssr = ScreenSpaceReflection(
      app,
      commandBuffer,
      m_heap.getDescriptorSetLayout());
  m_ssr.getReflectionBuffer().registerToHeap(m_heap);
}

void ParticleSystem::_createSimResources(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  uint32_t particleBufferCount =
      (PARTICLE_COUNT - 1) / PARTICLES_PER_BUFFER + 1;
  std::vector<StructuredBuffer<Particle>> particleBufferHeap;
  particleBufferHeap.reserve(particleBufferCount);

  // For now the last buffer could be overallocated if the particles-per-buffer
  // value doesn't perfectly divide the particle count
  for (uint32_t bufferIdx = 0; bufferIdx < particleBufferCount; ++bufferIdx) {
    particleBufferHeap.emplace_back(app, PARTICLES_PER_BUFFER);
    particleBufferHeap.back().registerToHeap(m_heap);
  }

  m_particleBuffer =
      StructuredBufferHeap<Particle>(std::move(particleBufferHeap));

  _resetParticles(app, commandBuffer);

  // Need transfer bit for vkCmdFillBuffer
  uint32_t spatialHashBufferCount =
      (SPATIAL_HASH_SIZE - 1) / SPATIAL_HASH_ENTRIES_PER_BUFFER + 1;
  std::vector<StructuredBuffer<uint32_t>> spatialHashHeap;
  spatialHashHeap.reserve(spatialHashBufferCount);
  for (uint32_t bufferIdx = 0; bufferIdx < spatialHashBufferCount;
       ++bufferIdx) {
    spatialHashHeap.emplace_back(app, SPATIAL_HASH_ENTRIES_PER_BUFFER);
    spatialHashHeap.back().registerToHeap(m_heap);
  }

  m_spatialHash = StructuredBufferHeap<uint32_t>(std::move(spatialHashHeap));

  m_freeBucketCounter = StructuredBuffer<uint32_t>(app, LOCAL_SIZE_X);
  m_freeBucketCounter.registerToHeap(m_heap);

  uint32_t bucketBufferCount =
      (PARTICLE_BUCKET_COUNT - 1) / PARTICLE_BUCKETS_PER_BUFFFER + 1;
  std::vector<StructuredBuffer<ParticleBucket>> bucketHeap;
  bucketHeap.reserve(bucketBufferCount);
  for (uint32_t bufferIdx = 0; bufferIdx < bucketBufferCount; ++bufferIdx) {
    bucketHeap.emplace_back(app, PARTICLE_BUCKETS_PER_BUFFFER);
    bucketHeap.back().registerToHeap(m_heap);
  }

  m_buckets = StructuredBufferHeap<ParticleBucket>(std::move(bucketHeap));

  m_simUniforms = TransientUniforms<SimUniforms>(app);
  m_simUniforms.registerToHeap(m_heap);

  ShaderDefines shaderDefs{};
  shaderDefs.emplace("LOCAL_SIZE_X", std::to_string(LOCAL_SIZE_X));
  {
    ComputePipelineBuilder builder;
    builder.setComputeShader(
        GProjectDirectory + "/Shaders/ParticleSystem/ParticleSystem.comp.glsl",
        shaderDefs);
    builder.layoutBuilder.addDescriptorSet(m_heap.getDescriptorSetLayout())
        .addPushConstants<PushConstants>();

    m_computePasses.emplace_back(app, std::move(builder));
  }

  {
    ComputePipelineBuilder builder;
    builder.setComputeShader(
        GProjectDirectory + "/Shaders/ParticleSystem/BucketAlloc.glsl",
        shaderDefs);
    builder.layoutBuilder.addDescriptorSet(m_heap.getDescriptorSetLayout())
        .addPushConstants<PushConstants>();

    m_computePasses.emplace_back(app, std::move(builder));
  }

  {
    ComputePipelineBuilder builder;
    builder.setComputeShader(
        GProjectDirectory + "/Shaders/ParticleSystem/CopyToBucket.glsl",
        shaderDefs);
    builder.layoutBuilder.addDescriptorSet(m_heap.getDescriptorSetLayout())
        .addPushConstants<PushConstants>();

    m_computePasses.emplace_back(app, std::move(builder));
  }

  {
    ComputePipelineBuilder builder;
    builder.setComputeShader(
        GProjectDirectory +
            "/Shaders/ParticleSystem/ProjectedJacobiStep.comp.glsl",
        shaderDefs);
    builder.layoutBuilder.addDescriptorSet(m_heap.getDescriptorSetLayout())
        .addPushConstants<PushConstants>();

    m_computePasses.emplace_back(app, std::move(builder));
  }
}

void ParticleSystem::_createGBufferPass(Application& app) {
  std::vector<SubpassBuilder> subpassBuilders;

  // Render particles
  {
    ShaderDefines defs{};
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    GBufferResources::setupAttachments(subpassBuilder);

    subpassBuilder.pipelineBuilder.setPrimitiveType(PrimitiveType::TRIANGLES)
        .addVertexInputBinding<glm::vec3>(VK_VERTEX_INPUT_RATE_VERTEX)
        .addVertexAttribute(VertexAttributeType::VEC3, 0)
        .addVertexShader(
            GProjectDirectory + "/Shaders/ParticleSystem/Particles.vert",
            defs)
        .addFragmentShader(
            GProjectDirectory + "/Shaders/ParticleSystem/Particles.frag")
        .layoutBuilder.addDescriptorSet(m_heap.getDescriptorSetLayout())
        .addPushConstants<PushConstants>();
  }

  // Render floor
#if 0
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    GBufferResources::setupAttachments(subpassBuilder);

    subpassBuilder.pipelineBuilder.setPrimitiveType(PrimitiveType::TRIANGLES)
        .setCullMode(VK_CULL_MODE_FRONT_BIT)
        .addVertexShader(GEngineDirectory + "/Shaders/Misc/FullScreenQuad.vert")
        .addFragmentShader(GEngineDirectory + "/Shaders/Misc/Floor.frag")
        .layoutBuilder.addDescriptorSet(m_heap.getDescriptorSetLayout())
        .addPushConstants<PushConstants>();
  }
#endif

  std::vector<Attachment> attachments =
      m_globalResources.getGBuffer().getAttachmentDescriptions();
  const VkExtent2D& extent = app.getSwapChainExtent();
  m_gBufferPass = RenderPass(
      app,
      extent,
      std::move(attachments),
      std::move(subpassBuilders));

  m_gBufferFrameBufferA = FrameBuffer(
      app,
      m_gBufferPass,
      extent,
      m_globalResources.getGBuffer().getAttachmentViewsA());
  m_gBufferFrameBufferB = FrameBuffer(
      app,
      m_gBufferPass,
      extent,
      m_globalResources.getGBuffer().getAttachmentViewsB());
}

void ParticleSystem::_createDeferredPass(Application& app) {
  VkClearValue colorClear;
  colorClear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkClearValue depthClear;
  depthClear.depthStencil = {1.0f, 0};

  std::vector<Attachment> attachments = {Attachment{
      ATTACHMENT_FLAG_COLOR,
      app.getSwapChainImageFormat(),
      colorClear,
      true,
      false,
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
        .layoutBuilder.addDescriptorSet(m_heap.getDescriptorSetLayout())
        .addPushConstants<DeferredPassPushConstants>();
  }

  m_deferredPass = RenderPass(
      app,
      app.getSwapChainExtent(),
      std::move(attachments),
      std::move(subpassBuilders));

  m_swapChainFrameBuffers =
      SwapChainFrameBufferCollection(app, m_deferredPass, {});
}

namespace {
struct DrawableEnvMap {
  void draw(const DrawContext& context) const {
    context.bindDescriptorSets();
    context.draw(3);
  }
};
} // namespace

void ParticleSystem::_renderGBufferPass(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {
  VkDescriptorSet globalDescriptorSet = m_heap.getDescriptorSet();

  {
    ActiveRenderPass pass = m_gBufferPass.begin(
        app,
        commandBuffer,
        frame,
        m_writeIndex == 0 ? m_gBufferFrameBufferA : m_gBufferFrameBufferB);
    // Bind global descriptor sets
    pass.setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));
    pass.getDrawContext().bindDescriptorSets();
    pass.getDrawContext().updatePushConstants(m_push, 0);

    pass.getDrawContext().bindIndexBuffer(m_sphere.indices);
    pass.getDrawContext().bindVertexBuffer(m_sphere.vertices);
    pass.getDrawContext().drawIndexed(
        m_sphere.indices.getIndexCount(),
        m_activeParticleCount);

    // Draw floor
#if 0
    pass.nextSubpass();
    pass.setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1));
    pass.getDrawContext().bindDescriptorSets();
    pass.getDrawContext().updatePushConstants(m_push, 0);
    pass.getDrawContext().draw(3);
#endif
  }
}

void ParticleSystem::_dispatchComputePass(
    VkCommandBuffer commandBuffer,
    uint32_t passIdx,
    uint32_t groupCount) {
  VkDescriptorSet set = m_heap.getDescriptorSet();
  m_computePasses[passIdx].bindPipeline(commandBuffer);
  vkCmdBindDescriptorSets(
      commandBuffer,
      VK_PIPELINE_BIND_POINT_COMPUTE,
      m_computePasses[passIdx].getLayout(),
      0,
      1,
      &set,
      0,
      nullptr);

  vkCmdPushConstants(
      commandBuffer,
      m_computePasses[passIdx].getLayout(),
      VK_SHADER_STAGE_ALL,
      0,
      sizeof(PushConstants),
      &m_push);

  vkCmdDispatch(commandBuffer, groupCount, 1, 1);
}

void ParticleSystem::_readAfterWriteBarrier(VkCommandBuffer commandBuffer) {

  uint32_t particleBufferCount = m_particleBuffer.getSize();
  uint32_t spatialHashBufferCount = m_spatialHash.getSize();

  static std::vector<VkBufferMemoryBarrier> postPresizeBarriers;
  postPresizeBarriers.resize(particleBufferCount + spatialHashBufferCount);

  for (uint32_t bufferIdx = 0; bufferIdx < particleBufferCount; ++bufferIdx) {
    const auto& buffer = m_particleBuffer.getBuffer(bufferIdx);

    VkBufferMemoryBarrier& barrier = postPresizeBarriers[bufferIdx];
    barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.buffer = buffer.getAllocation().getBuffer();
    barrier.offset = 0;
    barrier.size = buffer.getSize();
    barrier.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  }

  for (uint32_t bufferIdx = 0; bufferIdx < spatialHashBufferCount;
       ++bufferIdx) {
    const auto& buffer = m_spatialHash.getBuffer(bufferIdx);

    VkBufferMemoryBarrier& barrier =
        postPresizeBarriers[bufferIdx + particleBufferCount];
    barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.buffer = buffer.getAllocation().getBuffer();
    barrier.offset = 0;
    barrier.size = buffer.getSize();
    barrier.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  }

  vkCmdPipelineBarrier(
      commandBuffer,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0,
      0,
      nullptr,
      postPresizeBarriers.size(),
      postPresizeBarriers.data(),
      0,
      nullptr);
}

void ParticleSystem::_writeAfterReadBarrier(VkCommandBuffer commandBuffer) {
  uint32_t particleBufferCount = m_particleBuffer.getSize();
  uint32_t spatialHashBufferCount = m_spatialHash.getSize();

  static std::vector<VkBufferMemoryBarrier> postBucketAllocBarriers;
  postBucketAllocBarriers.resize(spatialHashBufferCount + 1);

  for (uint32_t bufferIdx = 0; bufferIdx < spatialHashBufferCount;
       ++bufferIdx) {
    auto& buffer = m_spatialHash.getBuffer(bufferIdx);

    VkBufferMemoryBarrier& barrier = postBucketAllocBarriers[bufferIdx];
    barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.buffer = buffer.getAllocation().getBuffer();
    barrier.offset = 0;
    barrier.size = buffer.getSize();
    barrier.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  }

  // TODO: Do we even need this barrier??
  VkBufferMemoryBarrier& barrier =
      postBucketAllocBarriers[spatialHashBufferCount];
  barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barrier.buffer = m_freeBucketCounter.getAllocation().getBuffer();
  barrier.offset = 0;
  barrier.size = m_freeBucketCounter.getSize();
  barrier.srcAccessMask =
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  vkCmdPipelineBarrier(
      commandBuffer,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0,
      0,
      nullptr,
      postBucketAllocBarriers.size(),
      postBucketAllocBarriers.data(),
      0,
      nullptr);
}

void ParticleSystem::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {

  VkDescriptorSet set = m_heap.getDescriptorSet();

  uint32_t spatialHashBufferCount = m_spatialHash.getSize();
  uint32_t particleBufferCount = m_particleBuffer.getSize();
  uint32_t bucketBufferCount = m_buckets.getSize();

  // TODO: Clean-up rest of the barriers
  // probably just need one, write-after-write type barrier between each compute
  // pass... should be able to re-use same barrier function

  for (uint32_t substep = 0; substep < TIME_SUBSTEPS; ++substep) {
    // Reset the spatial hash and prepare the buffers for read/write
    {
      static std::vector<VkBufferMemoryBarrier> spatialHashResetBarriers;
      spatialHashResetBarriers.resize(spatialHashBufferCount);

      for (uint32_t bufferIdx = 0; bufferIdx < spatialHashBufferCount;
           ++bufferIdx) {
        const auto& buffer = m_spatialHash.getBuffer(bufferIdx);

        VkBufferMemoryBarrier& barrier = spatialHashResetBarriers[bufferIdx];
        barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.buffer = buffer.getAllocation().getBuffer();
        barrier.offset = 0;
        barrier.size = buffer.getSize();
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            1,
            &barrier,
            0,
            nullptr);

        vkCmdFillBuffer(
            commandBuffer,
            buffer.getAllocation().getBuffer(),
            0,
            buffer.getSize(),
            0xFFFFFFFF);

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
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

    // Particle simulation and cell bucket pre-sizing pass
    // - Update particles with new positions
    // - Update particles with new grid cell hash
    // - Increment spatial hash bucket count for each particle
    {
      uint32_t groupCountX = (m_activeParticleCount - 1) / LOCAL_SIZE_X + 1;
      _dispatchComputePass(commandBuffer, SIM_PASS, groupCountX);

      _readAfterWriteBarrier(commandBuffer);
    }

    // Bucket alloc pass
    // - Allocate a bucket from free list
    // - Write bucket start idx to spatial hash grid cell
    {
      uint32_t groupCountX = (SPATIAL_HASH_SIZE - 1) / LOCAL_SIZE_X + 1;

      _dispatchComputePass(commandBuffer, BUCKET_ALLOC_PASS, groupCountX);
      _writeAfterReadBarrier(commandBuffer);
    }

    // Copy to particles bucket pass
    // - Copy particle to bucket
    // - Update bucket offset in spatial hash
    // - Update particle with new global index
    {
      uint32_t groupCountX = (m_activeParticleCount - 1) / LOCAL_SIZE_X + 1;
      _dispatchComputePass(commandBuffer, BUCKET_INSERT_PASS, groupCountX);

      static std::vector<VkBufferMemoryBarrier> postBucketInsertBarriers;
      postBucketInsertBarriers.resize(
          spatialHashBufferCount + bucketBufferCount + particleBufferCount);

      for (uint32_t bufferIdx = 0; bufferIdx < spatialHashBufferCount;
           ++bufferIdx) {
        auto& buffer = m_spatialHash.getBuffer(bufferIdx);

        VkBufferMemoryBarrier& barrier = postBucketInsertBarriers[bufferIdx];
        barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.buffer = buffer.getAllocation().getBuffer();
        barrier.offset = 0;
        barrier.size = buffer.getSize();
        barrier.srcAccessMask =
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      }

      for (uint32_t bufferIdx = 0; bufferIdx < bucketBufferCount; ++bufferIdx) {
        auto& buffer = m_buckets.getBuffer(bufferIdx);

        VkBufferMemoryBarrier& barrier =
            postBucketInsertBarriers[spatialHashBufferCount + bufferIdx];
        barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.buffer = buffer.getAllocation().getBuffer();
        barrier.offset = 0;
        barrier.size = buffer.getSize();
        barrier.srcAccessMask =
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      }

      for (uint32_t bufferIdx = 0; bufferIdx < particleBufferCount;
           ++bufferIdx) {
        auto& buffer = m_particleBuffer.getBuffer(bufferIdx);

        VkBufferMemoryBarrier& barrier = postBucketInsertBarriers
            [spatialHashBufferCount + bucketBufferCount + bufferIdx];
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
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0,
          0,
          nullptr,
          postBucketInsertBarriers.size(),
          postBucketInsertBarriers.data(),
          0,
          nullptr);
    }

    // Dispatch jacobi iterations for collision resolution
    {
      uint32_t groupCountX = (m_activeParticleCount - 1) / LOCAL_SIZE_X + 1;

      static std::vector<VkBufferMemoryBarrier> collisionStepBarriers;
      collisionStepBarriers.resize(bucketBufferCount);

      for (uint32_t bufferIdx = 0; bufferIdx < bucketBufferCount; ++bufferIdx) {
        VkBufferMemoryBarrier& collisionStepBarrier =
            collisionStepBarriers[bufferIdx];
        collisionStepBarrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        collisionStepBarrier.buffer =
            m_buckets.getBuffer(bufferIdx).getAllocation().getBuffer();
        collisionStepBarrier.offset = 0;
        collisionStepBarrier.size = m_buckets.getBuffer(bufferIdx).getSize();
        collisionStepBarrier.srcAccessMask =
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        collisionStepBarrier.dstAccessMask =
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      }

      m_computePasses[JACOBI_STEP_PASS].bindPipeline(commandBuffer);
      vkCmdBindDescriptorSets(
          commandBuffer,
          VK_PIPELINE_BIND_POINT_COMPUTE,
          m_computePasses[JACOBI_STEP_PASS].getLayout(),
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
            collisionStepBarriers.size(),
            collisionStepBarriers.data(),
            0,
            nullptr);

        m_push.iteration = iter;
        vkCmdPushConstants(
            commandBuffer,
            m_computePasses[JACOBI_STEP_PASS].getLayout(),
            VK_SHADER_STAGE_ALL,
            0,
            sizeof(PushConstants),
            &m_push);
        vkCmdDispatch(commandBuffer, groupCountX, 1, 1);
      }
    }
  }

  m_globalResources.getGBuffer().transitionToAttachment(commandBuffer);

  _renderGBufferPass(app, commandBuffer, frame);

  m_globalResources.getGBuffer().transitionToTextures(commandBuffer);

  // Reflection buffer and convolution
  {
    m_ssr.captureReflection(app, commandBuffer, set, frame, {}, {});
    m_ssr.convolveReflectionBuffer(app, commandBuffer, frame);
  }

  // Deferred pass
  {
    ActiveRenderPass pass = m_deferredPass.begin(
        app,
        commandBuffer,
        frame,
        m_swapChainFrameBuffers.getCurrentFrameBuffer(frame));
    // Bind global descriptor sets
    pass.setGlobalDescriptorSets(gsl::span(&set, 1));

    DeferredPassPushConstants push{};
    push.globalResources = m_globalResources.getHandle().index;
    push.globalUniforms =
        m_globalUniforms.getCurrentBindlessHandle(frame).index;
    push.reflectionBuffer = m_ssr.getReflectionBuffer().getHandle().index;
    push.writeIndex = m_writeIndex;
    {
      const DrawContext& context = pass.getDrawContext();
      context.bindDescriptorSets();
      context.updatePushConstants(push, 0);
      context.draw(3);
    }

    m_writeIndex ^= 1;
  }

  Gui::draw(app, frame, commandBuffer);
}
} // namespace ParticleSystem
} // namespace AltheaDemo