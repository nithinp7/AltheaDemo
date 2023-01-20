#include "GenIrradianceMap.h"

#include <Althea/Application.h>
#include <Althea/InputManager.h>
#include <Althea/Utilities.h>
#include <CesiumGltf/ImageCesium.h>
#include <CesiumGltfReader/GltfReader.h>
#include <gsl/span>
#include <stb_image.h>

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace AltheaEngine;

static CesiumGltf::ImageCesium loadHdri(const std::string& path) {
  std::vector<char> data = Utilities::readFile(path);

  CesiumGltfReader::ImageReaderResult result =
      CesiumGltfReader::GltfReader::readImage(
          gsl::span<const std::byte>(
              reinterpret_cast<const std::byte*>(data.data()),
              data.size()),
          {});

  if (!result.image) {
    throw std::runtime_error("Could not load skybox image!");
  }

  if (CesiumGltfReader::GltfReader::generateMipMaps(*result.image)) {
    throw std::runtime_error("Could not generate mipmap for skybox image!");
  }

  return std::move(*result.image);
}

void GenIrradianceMap::initGame(Application& app) {
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
      }
  );
}

void GenIrradianceMap::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void GenIrradianceMap::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  // Environment map
  CesiumGltf::ImageCesium envMapImg =
      loadHdri(GProjectDirectory + "/Content/HDRI_Skybox/LuxuryRoom.hdr");
  this->_environmentMap.image = Image(
      app,
      envMapImg,
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
          VK_IMAGE_USAGE_SAMPLED_BIT);
  const ImageOptions& imageDetails = this->_environmentMap.image.getOptions();

  SamplerOptions samplerOptions{};
  samplerOptions.mipCount = imageDetails.mipCount;
  this->_environmentMap.sampler = Sampler(app, samplerOptions);

  // TODO: create straight from image details?
  this->_environmentMap.view = ImageView(
      app,
      this->_environmentMap.image.getImage(),
      imageDetails.format,
      imageDetails.mipCount,
      1,
      VK_IMAGE_VIEW_TYPE_2D,
      VK_IMAGE_ASPECT_COLOR_BIT);

  // Create device-only resource for irradiance map
  ImageOptions irrMapOptions{};
  irrMapOptions.width = imageDetails.width;
  irrMapOptions.height = imageDetails.height;
  irrMapOptions.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

  this->_irradianceMap.image = Image(app, irrMapOptions);
  this->_irradianceMap.sampler = Sampler(app, samplerOptions);
  this->_irradianceMap.view = ImageView(
      app,
      this->_irradianceMap.image.getImage(),
      imageDetails.format,
      1,
      1,
      VK_IMAGE_VIEW_TYPE_2D,
      VK_IMAGE_ASPECT_COLOR_BIT);

  // Init compute pass
  DescriptorSetLayoutBuilder computeResourcesLayout{};
  computeResourcesLayout
      // Environment map input
      .addStorageImageBinding()
      // Irradiance map output
      .addStorageImageBinding();

  this->_computePass.materialAllocator =
      std::make_unique<DescriptorSetAllocator>(app, computeResourcesLayout, 1);
  this->_computePass.material = std::make_unique<DescriptorSet>(
      this->_computePass.materialAllocator->allocate());
  this->_computePass.material
      ->assign()
      // Bind environment map input
      .bindStorageImage(
          this->_environmentMap.view,
          this->_environmentMap.sampler)
      // Bind irradiance map output
      .bindStorageImage(
          this->_irradianceMap.view,
          this->_irradianceMap.sampler);

  ComputePipelineBuilder computeBuilder;
  computeBuilder.setComputeShader(
      GProjectDirectory + "/Shaders/GenIrradianceMap.comp");
  computeBuilder.layoutBuilder.addDescriptorSet(
      this->_computePass.materialAllocator->getLayout());

  this->_computePass.pipeline = ComputePipeline(app, std::move(computeBuilder));

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
      std::make_unique<TransientUniforms<CameraUniforms>>(app);

  this->_renderPass.pGlobalResources
      ->assign()
      // Enviornment map
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

void GenIrradianceMap::destroyRenderState(Application& app) {
  this->_computePass.material.reset(); // Assignment destroys in wrong order!
  this->_computePass = {};
  this->_renderPass = {};
  this->_environmentMap = {};
  this->_irradianceMap = {};
}

void GenIrradianceMap::tick(Application& app, const FrameContext& frame) {
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

void GenIrradianceMap::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {
  if (this->_generateIrradianceMap) {
    this->_environmentMap.image.transitionLayout(
        commandBuffer,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    this->_irradianceMap.image.transitionLayout(
        commandBuffer,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    this->_computePass.pipeline.bindPipeline(commandBuffer);

    VkDescriptorSet material =
        this->_computePass.material->getVkDescriptorSet();
    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        this->_computePass.pipeline.getLayout(),
        0,
        1,
        &material,
        0,
        nullptr);

    const ImageOptions& imageDetails = this->_environmentMap.image.getOptions();
    uint32_t groupCountX = imageDetails.width / 16;
    uint32_t groupCountY = imageDetails.height / 16;
    vkCmdDispatch(commandBuffer, groupCountX, groupCountY, 1);

    this->_environmentMap.image.transitionLayout(
        commandBuffer,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    this->_irradianceMap.image.transitionLayout(
        commandBuffer,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    this->_generateIrradianceMap = false;
  }

  VkDescriptorSet globalDescriptorSet =
      this->_renderPass.pGlobalResources->getCurrentDescriptorSet(frame);
  this->_renderPass.pRenderPass
      ->begin(app, commandBuffer, frame)
      // Bind global descriptor sets
      .setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1))
      .draw(DrawableEnvMap{});
}