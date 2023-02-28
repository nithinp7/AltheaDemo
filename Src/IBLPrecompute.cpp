#include "IBLPrecompute.h"

#include <Althea/Application.h>
#include <Althea/InputManager.h>
#include <Althea/SingleTimeCommandBuffer.h>
#include <Althea/Utilities.h>
#include <CesiumGltf/ImageCesium.h>
#include <CesiumGltfReader/GltfReader.h>
#include <gsl/span>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace AltheaEngine;

namespace {
struct ImageDetailsPushConstants {
  float width;
  float height;
};

struct PrefilterEnvMapPushConstants {
  float width;
  float height;
  float roughness;
};

void saveHdriImage(
    Application& app,
    VkCommandBuffer commandBuffer,
    Image& image,
    const std::string& path) {
  uint32_t width = image.getOptions().width;
  uint32_t height = image.getOptions().height;

  VmaAllocationCreateInfo stagingAllocInfo{};
  stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
  stagingAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
  stagingAllocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  BufferAllocation stagingBuffer = BufferUtilities::createBuffer(
      app,
      commandBuffer,
      width * height * 32 * 4,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      stagingAllocInfo);

  image.copyMipToBuffer(commandBuffer, stagingBuffer.getBuffer(), 0, 0);

  // Write out image and delete the staging buffer once the frame is
  // complete
  app.addDeletiontask(DeletionTask{
      [width,
       height,
       path,
       pStagingBuffer = new BufferAllocation(std::move(stagingBuffer))]() {
        size_t bufferSize = pStagingBuffer->getInfo().size;
        std::vector<std::byte> buffer;
        buffer.resize(bufferSize);

        void* pStaging = pStagingBuffer->mapMemory();
        std::memcpy(buffer.data(), pStaging, bufferSize);
        pStagingBuffer->unmapMemory();

        Utilities::saveHdri(
            path,
            static_cast<int>(width),
            static_cast<int>(height),
            buffer);

        delete pStagingBuffer;
      },
      app.getCurrentFrameRingBufferIndex()});

  image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}
} // namespace

namespace AltheaDemo {
namespace IBLPrecompute {

void IBLPrecompute::initGame(Application& app) {
  const VkExtent2D& windowDims = app.getSwapChainExtent();
  this->_pCameraController = std::make_unique<CameraController>(
      app.getInputManager(),
      90.0f,
      (float)windowDims.width / (float)windowDims.height);

  // TODO: need to unbind these at shutdown
  InputManager& input = app.getInputManager();
  // Recreate any stale pipelines (shader hot-reload)
  input.addKeyBinding(
      {GLFW_KEY_R, GLFW_PRESS, GLFW_MOD_CONTROL},
      [&app, that = this]() {
        for (Subpass& subpass : that->_renderPass.pRenderPass->getSubpasses()) {
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

  // Refresh render state (useful to update compute shader)
  // TODO: implement compute pipeline hot-reloading
  input.addKeyBinding(
      {GLFW_KEY_T, GLFW_PRESS, GLFW_MOD_CONTROL},
      [&app, that = this]() {
        vkDeviceWaitIdle(app.getDevice());
        that->destroyRenderState(app);
        that->createRenderState(app);
      });
}

void IBLPrecompute::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void IBLPrecompute::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  SingleTimeCommandBuffer commandBuffer(app);

  // Environment map
  CesiumGltf::ImageCesium envMapImg = Utilities::loadHdri(
      GProjectDirectory + "/Content/HDRI_Skybox/NeoclassicalInterior.hdr");
  // loadHdri(GProjectDirectory + "/Content/HDRI_Skybox/LuxuryRoom.hdr");

  ImageOptions imageOptions{};
  imageOptions.width = static_cast<uint32_t>(envMapImg.width);
  imageOptions.height = static_cast<uint32_t>(envMapImg.height);
  imageOptions.mipCount = 1;
  // Utilities::computeMipCount(imageOptions.width, imageOptions.height);
  imageOptions.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  imageOptions.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT;
  this->_environmentMap.image =
      Image(app, commandBuffer, envMapImg.pixelData, imageOptions);

  SamplerOptions samplerOptions{};
  samplerOptions.mipCount = imageOptions.mipCount;
  samplerOptions.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerOptions.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  this->_environmentMap.sampler = Sampler(app, samplerOptions);

  // TODO: create straight from image details?
  this->_environmentMap.view = ImageView(
      app,
      this->_environmentMap.image.getImage(),
      imageOptions.format,
      imageOptions.mipCount,
      1,
      VK_IMAGE_VIEW_TYPE_2D,
      VK_IMAGE_ASPECT_COLOR_BIT);

  // Create pre-filtered environment maps
  this->_preFilteredMap.reserve(5);
  for (uint32_t mipIndex = 1; mipIndex < 6; ++mipIndex) {
    ImageOptions mipOptions = imageOptions;
    mipOptions.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    mipOptions.width >>= mipIndex;
    mipOptions.height >>= mipIndex;

    ImageResource& mipImage = this->_preFilteredMap.emplace_back();
    mipImage.image = Image(app, mipOptions);
    mipImage.sampler = Sampler(app, samplerOptions);
    mipImage.view = ImageView(
        app,
        mipImage.image.getImage(),
        mipOptions.format,
        1,
        1,
        VK_IMAGE_VIEW_TYPE_2D,
        VK_IMAGE_ASPECT_COLOR_BIT);
  }

  // Create device-only resource for irradiance map
  ImageOptions irrMapOptions{};
  irrMapOptions.width = imageOptions.width;
  irrMapOptions.height = imageOptions.height;
  irrMapOptions.format = imageOptions.format;
  irrMapOptions.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_STORAGE_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

  this->_irradianceMap.image = Image(app, irrMapOptions);
  this->_irradianceMap.sampler = Sampler(app, samplerOptions);
  this->_irradianceMap.view = ImageView(
      app,
      this->_irradianceMap.image.getImage(),
      imageOptions.format,
      1,
      1,
      VK_IMAGE_VIEW_TYPE_2D,
      VK_IMAGE_ASPECT_COLOR_BIT);

  // Init irradiance map generation pass
  DescriptorSetLayoutBuilder genIrrResourcesLayout{};
  genIrrResourcesLayout
      // Environment map input
      .addTextureBinding(VK_SHADER_STAGE_COMPUTE_BIT)
      // Irradiance map output
      .addStorageImageBinding();

  this->_genIrradiancePass.materialAllocator =
      std::make_unique<DescriptorSetAllocator>(app, genIrrResourcesLayout, 1);
  this->_genIrradiancePass.material = std::make_unique<DescriptorSet>(
      this->_genIrradiancePass.materialAllocator->allocate());
  this->_genIrradiancePass.material
      ->assign()
      // Bind environment map input
      .bindTextureDescriptor(
          this->_environmentMap.view,
          this->_environmentMap.sampler)
      // Bind irradiance map output
      .bindStorageImage(
          this->_irradianceMap.view,
          this->_irradianceMap.sampler);

  ComputePipelineBuilder computeIrrBuilder;
  computeIrrBuilder.setComputeShader(
      GProjectDirectory + "/Shaders/GenIrradianceMap.comp");
  computeIrrBuilder.layoutBuilder
      .addDescriptorSet(this->_genIrradiancePass.materialAllocator->getLayout())
      .addPushConstants<ImageDetailsPushConstants>(VK_SHADER_STAGE_COMPUTE_BIT);

  this->_genIrradiancePass.pipeline =
      ComputePipeline(app, std::move(computeIrrBuilder));

  // Init environment map filtering pass
  DescriptorSetLayoutBuilder prefilterEnvResourcesLayout{};
  prefilterEnvResourcesLayout
      // Environment map input
      .addTextureBinding(VK_SHADER_STAGE_COMPUTE_BIT)
      // Filtered env mip output
      .addStorageImageBinding();

  this->_prefilterEnvMapPasses.materialAllocator =
      std::make_unique<DescriptorSetAllocator>(
          app,
          prefilterEnvResourcesLayout,
          5);

  this->_prefilterEnvMapPasses.materials.reserve(5);
  for (uint32_t i = 0; i < 5; ++i) {
    auto& pMaterial = this->_prefilterEnvMapPasses.materials.emplace_back(
        std::make_unique<DescriptorSet>(
            this->_prefilterEnvMapPasses.materialAllocator->allocate()));
    pMaterial
        ->assign()
        // Environment map
        .bindTextureDescriptor(
            this->_environmentMap.view,
            this->_environmentMap.sampler)
        // Current prefiltered mip index
        .bindStorageImage(
            this->_preFilteredMap[i].view,
            this->_preFilteredMap[i].sampler);
  }

  ComputePipelineBuilder computePrefilteredEnvBuilder{};
  computePrefilteredEnvBuilder.setComputeShader(
      GProjectDirectory + "/Shaders/PrefilterEnvMap.comp");
  computePrefilteredEnvBuilder.layoutBuilder
      .addDescriptorSet(
          this->_prefilterEnvMapPasses.materialAllocator->getLayout())
      .addPushConstants<PrefilterEnvMapPushConstants>(
          VK_SHADER_STAGE_COMPUTE_BIT);

  this->_prefilterEnvMapPasses.pipeline =
      ComputePipeline(app, std::move(computePrefilteredEnvBuilder));

  // Init render pass

  // Global resources
  DescriptorSetLayoutBuilder globalResourcesBuilder{};
  globalResourcesBuilder
      // Environment map
      .addTextureBinding()
      // Irradiance map
      .addTextureBinding()
      // Camera uniforms
      .addUniformBufferBinding();

  this->_renderPass.pGlobalResources =
      std::make_unique<PerFrameResources>(app, globalResourcesBuilder);
  this->_renderPass.pGlobalUniforms =
      std::make_unique<TransientUniforms<CameraUniforms>>(app, commandBuffer);

  this->_renderPass.pGlobalResources
      ->assign()
      // Environment map
      .bindTexture(this->_environmentMap.view, this->_environmentMap.sampler)
      // Irradiance map
      .bindTexture(this->_irradianceMap.view, this->_irradianceMap.sampler)
      // Camera uniforms
      .bindTransientUniforms(*this->_renderPass.pGlobalUniforms);

  // TODO: Default color and depth-stencil clear values for attachments?
  VkClearValue colorClear;
  colorClear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkClearValue depthClear;
  depthClear.depthStencil = {1.0f, 0};

  std::vector<Attachment> attachments = {
      {AttachmentType::Color,
       app.getSwapChainImageFormat(),
       colorClear,
       std::nullopt,
       false},
      {AttachmentType::Depth,
       app.getDepthImageFormat(),
       depthClear,
       app.getDepthImageView(),
       true}};

  std::vector<SubpassBuilder> subpasses;
  {
    SubpassBuilder& subpass = subpasses.emplace_back();
    subpass.colorAttachments.push_back(0);
    // subpass.depthAttachment = 1;

    subpass.pipelineBuilder
        .addVertexShader(GProjectDirectory + "/Shaders/Skybox.vert")
        .addFragmentShader(GProjectDirectory + "/Shaders/Skybox.frag")

        .setCullMode(VK_CULL_MODE_FRONT_BIT)
        .setDepthTesting(false);

    subpass.pipelineBuilder.layoutBuilder.addDescriptorSet(
        this->_renderPass.pGlobalResources->getLayout());
  }

  this->_renderPass.pRenderPass = std::make_unique<RenderPass>(
      app,
      std::move(attachments),
      std::move(subpasses));
}

void IBLPrecompute::destroyRenderState(Application& app) {
  this->_genIrradiancePass.material
      .reset(); // Assignment destroys in wrong order!
  this->_genIrradiancePass = {};
  this->_prefilterEnvMapPasses.materials
      .clear(); // Assignment destroys in wrong order!
  this->_prefilterEnvMapPasses = {};
  this->_renderPass = {};
  this->_environmentMap = {};
  this->_irradianceMap = {};
  this->_preFilteredMap.clear();
  this->_recomputeMaps = true;
}

void IBLPrecompute::tick(Application& app, const FrameContext& frame) {
  this->_pCameraController->tick(frame.deltaTime);
  const Camera& camera = this->_pCameraController->getCamera();

  const glm::mat4& projection = camera.getProjection();

  CameraUniforms uniforms;
  uniforms.projection = camera.getProjection();
  uniforms.inverseProjection = glm::inverse(uniforms.projection);
  uniforms.view = camera.computeView();
  uniforms.inverseView = glm::inverse(uniforms.view);

  this->_renderPass.pGlobalUniforms->updateUniforms(uniforms, frame);
}

namespace {
struct DrawableEnvMap {
  void draw(const DrawContext& context) const {
    context.bindDescriptorSets();
    context.draw(3);
  }
};
} // namespace

void IBLPrecompute::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {
  if (this->_recomputeMaps) {
    this->_environmentMap.image.transitionLayout(
        commandBuffer,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // Generate irradiance map
    this->_irradianceMap.image.transitionLayout(
        commandBuffer,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    this->_genIrradiancePass.pipeline.bindPipeline(commandBuffer);

    VkDescriptorSet material =
        this->_genIrradiancePass.material->getVkDescriptorSet();
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        this->_genIrradiancePass.pipeline.getLayout(),
        0,
        1,
        &material,
        0,
        nullptr);

    const ImageOptions& imageDetails = this->_environmentMap.image.getOptions();
    ImageDetailsPushConstants genIrrConstants{};
    genIrrConstants.width = static_cast<float>(imageDetails.width);
    genIrrConstants.height = static_cast<float>(imageDetails.height);
    vkCmdPushConstants(
        commandBuffer,
        this->_genIrradiancePass.pipeline.getLayout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(ImageDetailsPushConstants),
        &genIrrConstants);

    uint32_t groupCountX = imageDetails.width / 16;
    uint32_t groupCountY = imageDetails.height / 16;
    vkCmdDispatch(commandBuffer, groupCountX, groupCountY, 1);

    saveHdriImage(
        app,
        commandBuffer,
        this->_irradianceMap.image,
        GProjectDirectory + "/PrecomputedMaps/IrradianceMap.hdr");

    // Generate prefiltered environment map mips.
    this->_prefilterEnvMapPasses.pipeline.bindPipeline(commandBuffer);
    for (uint32_t i = 0; i < 5; ++i) {
      ImageResource& mip = this->_preFilteredMap[i];
      uint32_t width = mip.image.getOptions().width;
      uint32_t height = mip.image.getOptions().height;

      mip.image.transitionLayout(
          commandBuffer,
          VK_IMAGE_LAYOUT_GENERAL,
          VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

      VkDescriptorSet material =
          this->_prefilterEnvMapPasses.materials[i]->getVkDescriptorSet();
      vkCmdBindDescriptorSets(
          commandBuffer,
          VK_PIPELINE_BIND_POINT_COMPUTE,
          this->_prefilterEnvMapPasses.pipeline.getLayout(),
          0,
          1,
          &material,
          0,
          nullptr);

      PrefilterEnvMapPushConstants prefilterEnvConstants{};
      prefilterEnvConstants.width = static_cast<float>(width);
      prefilterEnvConstants.height = static_cast<float>(height);
      prefilterEnvConstants.roughness = static_cast<float>(i + 1.0f) / 5.0f;

      vkCmdPushConstants(
          commandBuffer,
          this->_prefilterEnvMapPasses.pipeline.getLayout(),
          VK_SHADER_STAGE_COMPUTE_BIT,
          0,
          sizeof(PrefilterEnvMapPushConstants),
          &prefilterEnvConstants);

      groupCountX = width / 16;
      groupCountY = height / 16;
      vkCmdDispatch(commandBuffer, groupCountX, groupCountY, 1);

      saveHdriImage(
          app,
          commandBuffer,
          mip.image,
          GProjectDirectory + "/PrecomputedMaps/Prefiltered" +
              std::to_string(i + 1) + ".hdr");
    }

    this->_environmentMap.image.transitionLayout(
        commandBuffer,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    this->_recomputeMaps = false;
  }

  VkDescriptorSet globalDescriptorSet =
      this->_renderPass.pGlobalResources->getCurrentDescriptorSet(frame);
  this->_renderPass.pRenderPass
      ->begin(app, commandBuffer, frame)
      // Bind global descriptor sets
      .setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1))
      .draw(DrawableEnvMap{});
}

} // namespace IBLPrecompute
} // namespace AltheaDemo