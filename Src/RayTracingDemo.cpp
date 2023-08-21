#include "RayTracingDemo.h"

#include <Althea/Application.h>
#include <Althea/BufferUtilities.h>
#include <Althea/Camera.h>
#include <Althea/Cubemap.h>
#include <Althea/DescriptorSet.h>
#include <Althea/GraphicsPipeline.h>
#include <Althea/IndexBuffer.h>
#include <Althea/InputManager.h>
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

void RayTracingDemo::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void RayTracingDemo::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  SingleTimeCommandBuffer commandBuffer(app);
  this->_createGlobalResources(app, commandBuffer);
  this->_createModels(app, commandBuffer);
  this->_createForwardPass(app);
  this->_createRayTracingPass(app, commandBuffer);
  this->_createDeferredPass(app);
}

void RayTracingDemo::destroyRenderState(Application& app) {
  this->_models.clear();

  this->_pForwardPass.reset();
  this->_gBufferResources = {};
  this->_forwardFrameBuffer = {};

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

void RayTracingDemo::tick(Application& app, const FrameContext& frame) {
  this->_pCameraController->tick(frame.deltaTime);
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

void RayTracingDemo::_createModels(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {

  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/DamagedHelmet.glb",
      *this->_pGltfMaterialAllocator);
  this->_models.back().setModelTransform(glm::scale(
      glm::translate(glm::mat4(1.0f), glm::vec3(36.0f, 0.0f, 0.0f)),
      glm::vec3(4.0f)));

  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/FlightHelmet/FlightHelmet.gltf",
      *this->_pGltfMaterialAllocator);
  this->_models.back().setModelTransform(glm::scale(
      glm::translate(glm::mat4(1.0f), glm::vec3(50.0f, -1.0f, 0.0f)),
      glm::vec3(8.0f)));

  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/MetalRoughSpheres.glb",
      *this->_pGltfMaterialAllocator);
  this->_models.back().setModelTransform(glm::scale(
      glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)),
      glm::vec3(4.0f)));

  this->_models.emplace_back(
      app,
      commandBuffer,
      GEngineDirectory + "/Content/Models/Sponza/glTF/Sponza.gltf",
      *this->_pGltfMaterialAllocator);
  this->_models.back().setModelTransform(glm::translate(
      glm::scale(glm::mat4(1.0f), glm::vec3(10.0f)),
      glm::vec3(10.0f, -1.0f, 0.0f)));
}

void RayTracingDemo::_createGlobalResources(
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
        std::make_unique<TransientUniforms<GlobalUniforms>>(app, commandBuffer);

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
                                     -0.1f,
                                     (static_cast<float>(j) - 1.5f) * 0.5f);
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

void RayTracingDemo::_createForwardPass(Application& app) {
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

void RayTracingDemo::_createRayTracingPass(
    Application& app,
    SingleTimeCommandBuffer& commandBuffer) {
  uint32_t primCount = 0;
  for (const Model& model : this->_models) {
    primCount += static_cast<uint32_t>(model.getPrimitivesCount());
  }

  std::vector<AABB> aabbs;
  aabbs.reserve(primCount);
  std::vector<VkTransformMatrixKHR> transforms;
  transforms.reserve(primCount);
  for (const Model& model : this->_models) {
    for (const Primitive& prim : model.getPrimitives()) {
      aabbs.push_back(prim.getAABB());

      const glm::mat4& primTransform = prim.getRelativeTransform();

      VkTransformMatrixKHR& transform = transforms.emplace_back();
      transform.matrix[0][0] = primTransform[0][0];
      transform.matrix[1][0] = primTransform[0][1];
      transform.matrix[2][0] = primTransform[0][2];

      transform.matrix[0][1] = primTransform[1][0];
      transform.matrix[1][1] = primTransform[1][1];
      transform.matrix[2][1] = primTransform[1][2];

      transform.matrix[0][2] = primTransform[2][0];
      transform.matrix[1][2] = primTransform[2][1];
      transform.matrix[2][2] = primTransform[2][2];

      transform.matrix[0][3] = primTransform[3][0];
      transform.matrix[1][3] = primTransform[3][1];
      transform.matrix[2][3] = primTransform[3][2];
    }
  }

  // Upload AABBs for all primitives
  VmaAllocationCreateInfo aabbBufferAllocationInfo{};
  aabbBufferAllocationInfo.flags =
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  aabbBufferAllocationInfo.usage = VMA_MEMORY_USAGE_AUTO;

  BufferAllocation aabbBuffer = BufferUtilities::createBuffer(
      app,
      commandBuffer,
      sizeof(AABB) * primCount,
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      aabbBufferAllocationInfo);

  {
    void* data = aabbBuffer.mapMemory();
    memcpy(data, aabbs.data(), primCount * sizeof(AABB));
    aabbBuffer.unmapMemory();
  }

  VkBufferDeviceAddressInfo aabbBufferAddrInfo{
      VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  aabbBufferAddrInfo.buffer = aabbBuffer.getBuffer();
  VkDeviceAddress aabbBufferDevAddr =
      vkGetBufferDeviceAddress(app.getDevice(), &aabbBufferAddrInfo);

  // Upload transforms for all primitives
  VmaAllocationCreateInfo transformBufferAllocInfo{};
  transformBufferAllocInfo.flags =
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  transformBufferAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;

  BufferAllocation transformBuffer = BufferUtilities::createBuffer(
      app,
      commandBuffer,
      sizeof(VkTransformMatrixKHR) * primCount,
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      transformBufferAllocInfo);

  {
    void* data = transformBuffer.mapMemory();
    memcpy(data, transforms.data(), primCount * sizeof(VkTransformMatrixKHR));
    transformBuffer.unmapMemory();
  }

  VkBufferDeviceAddressInfo transformBufferAddrInfo{
      VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  transformBufferAddrInfo.buffer = transformBuffer.getBuffer();
  VkDeviceAddress transformBufferDevAddr =
      vkGetBufferDeviceAddress(app.getDevice(), &transformBufferAddrInfo);

  std::vector<VkAccelerationStructureGeometryKHR> geometries;
  geometries.reserve(primCount);

  std::vector<VkAccelerationStructureBuildRangeInfoKHR> buildRanges;
  buildRanges.reserve(primCount);

  std::vector<uint32_t> triCounts;
  triCounts.reserve(primCount);

  uint32_t primIndex = 0;
  uint32_t maxPrimCount = 0;
  for (const Model& model : this->_models) {
    for (const Primitive& prim : model.getPrimitives()) {
      const VertexBuffer<Vertex>& vertexBuffer = prim.getVertexBuffer();
      VkBufferDeviceAddressInfo vertexBufferAddrInfo{
          VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
      vertexBufferAddrInfo.buffer = vertexBuffer.getAllocation().getBuffer();
      VkDeviceAddress vertexBufferDevAddr =
          vkGetBufferDeviceAddress(app.getDevice(), &vertexBufferAddrInfo);

      const IndexBuffer& indexBuffer = prim.getIndexBuffer();
      VkBufferDeviceAddressInfo indexBufferAddrInfo{
          VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
      indexBufferAddrInfo.buffer = indexBuffer.getAllocation().getBuffer();
      VkDeviceAddress indexBufferDevAddr =
          vkGetBufferDeviceAddress(app.getDevice(), &indexBufferAddrInfo);

      VkAccelerationStructureGeometryKHR& geometry = geometries.emplace_back();
      geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
      geometry.geometry.aabbs.sType =
          VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
      geometry.geometry.aabbs.data.deviceAddress =
          aabbBufferDevAddr + primIndex * sizeof(AABB);
      geometry.geometry.aabbs.stride = sizeof(AABB);

      geometry.geometry.triangles.sType =
          VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
      geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
      geometry.geometry.triangles.vertexData.deviceAddress =
          vertexBufferDevAddr;
      geometry.geometry.triangles.vertexStride = sizeof(Vertex);
      geometry.geometry.triangles.maxVertex =
          static_cast<uint32_t>(vertexBuffer.getVertexCount() - 1);
      geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
      geometry.geometry.triangles.indexData.deviceAddress = indexBufferDevAddr;
      
      geometry.geometry.triangles.transformData.deviceAddress = 
          transformBufferDevAddr + primIndex * sizeof(VkTransformMatrixKHR);

      VkAccelerationStructureBuildRangeInfoKHR& buildRange =
          buildRanges.emplace_back();
      buildRange.firstVertex = 0;
      buildRange.primitiveCount = indexBuffer.getIndexCount() / 3;
      buildRange.primitiveOffset = 0;
      buildRange.transformOffset = 0;

      triCounts.push_back(buildRange.primitiveCount);

      ++primIndex;
    }
  }

  VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  buildInfo.pGeometries = geometries.data();
  buildInfo.geometryCount = primCount;

  VkAccelerationStructureBuildSizesInfoKHR buildSizes{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  app.vkGetAccelerationStructureBuildSizesKHR(
      app.getDevice(),
      VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
      &buildInfo,
      triCounts.data(),
      &buildSizes);

  // Create backing buffer and scratch buffer for acceleration structures
  VmaAllocationCreateInfo accelStrAllocInfo{};
  accelStrAllocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  this->_accelerationStructureBuffer = BufferUtilities::createBuffer(
      app,
      commandBuffer,
      buildSizes.accelerationStructureSize,
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
      accelStrAllocInfo);
  BufferAllocation scratchBuffer = BufferUtilities::createBuffer(
      app,
      commandBuffer,
      buildSizes.buildScratchSize,
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      accelStrAllocInfo);

  VkBufferDeviceAddressInfo scratchBufferAddrInfo{
      VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  scratchBufferAddrInfo.buffer = scratchBuffer.getBuffer();
  VkDeviceAddress scratchBufferDevAddr =
      vkGetBufferDeviceAddress(app.getDevice(), &scratchBufferAddrInfo);

  buildInfo.scratchData.deviceAddress = scratchBufferDevAddr;

  VkAccelerationStructureCreateInfoKHR accelerationStructureInfo{};
  accelerationStructureInfo.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
  accelerationStructureInfo.type =
      VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  accelerationStructureInfo.buffer =
      this->_accelerationStructureBuffer.getBuffer();
  accelerationStructureInfo.offset = 0;
  accelerationStructureInfo.size = buildSizes.accelerationStructureSize;

  if (app.vkCreateAccelerationStructureKHR(
          app.getDevice(),
          &accelerationStructureInfo,
          nullptr,
          &this->_accelerationStructure) != VK_SUCCESS) {
    throw std::runtime_error("Failed to create acceleration structure!");
  }

  buildInfo.dstAccelerationStructure = this->_accelerationStructure;

  VkAccelerationStructureBuildRangeInfoKHR* pBuildRange = buildRanges.data();
  VkAccelerationStructureBuildRangeInfoKHR** ppBuildRange = &pBuildRange;

  app.vkCmdBuildAccelerationStructuresKHR(
      commandBuffer,
      1,
      &buildInfo,
      ppBuildRange);

  // Add task to delete all temp buffers once the commands have completed
  commandBuffer.addPostCompletionTask(
      [pAabbBuffer = new BufferAllocation(std::move(aabbBuffer)),
       pTransformBuffer = new BufferAllocation(std::move(transformBuffer)),
       pScratchBuffer = new BufferAllocation(std::move(scratchBuffer))]() {
        delete pAabbBuffer;
        delete pTransformBuffer;
        delete pScratchBuffer;
      });
}

void RayTracingDemo::_createDeferredPass(Application& app) {
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

void RayTracingDemo::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {

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
} // namespace RayTracingDemo
} // namespace AltheaDemo