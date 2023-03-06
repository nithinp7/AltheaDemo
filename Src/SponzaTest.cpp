#include "SponzaTest.h"

#include <Althea/Application.h>
#include <Althea/Camera.h>
#include <Althea/Cubemap.h>
#include <Althea/DescriptorSet.h>
#include <Althea/GraphicsPipeline.h>
#include <Althea/InputManager.h>
#include <Althea/ModelViewProjection.h>
#include <Althea/Primitive.h>
#include <Althea/SingleTimeCommandBuffer.h>
#include <Althea/Utilities.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace AltheaEngine;

namespace AltheaDemo {
namespace SponzaTest {

SponzaTest::SponzaTest() {}

void SponzaTest::initGame(Application& app) {
  const VkExtent2D& windowDims = app.getSwapChainExtent();
  this->_pCameraController = std::make_unique<CameraController>(
      app.getInputManager(),
      90.0f,
      (float)windowDims.width / (float)windowDims.height);

  // TODO: need to unbind these at shutdown
  InputManager& input = app.getInputManager();
  input.addKeyBinding(
      {GLFW_KEY_L, GLFW_PRESS, 0},
      [&adjustingLight = this->_adjustingLight, &input]() {
        adjustingLight = true;
        input.setMouseCursorHidden(false);
      });

  input.addKeyBinding(
      {GLFW_KEY_L, GLFW_RELEASE, 0},
      [&adjustingLight = this->_adjustingLight, &input]() {
        adjustingLight = false;
        input.setMouseCursorHidden(true);
      });

  // Recreate any stale pipelines (shader hot-reload)
  input.addKeyBinding(
      {GLFW_KEY_R, GLFW_PRESS, GLFW_MOD_CONTROL},
      [&app, that = this]() {
        for (Subpass& subpass : that->_pRenderPass->getSubpasses()) {
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
      [&adjustingLight = this->_adjustingLight,
       &lightDir = this->_lightDir](double x, double y, bool cursorHidden) {
        if (adjustingLight) {
          // TODO: consider current camera forward direction.
          float theta = glm::pi<float>() * static_cast<float>(x);
          float height = static_cast<float>(y) + 1.0f;

          lightDir = glm::normalize(glm::vec3(cos(theta), height, sin(theta)));
        }
      });
}

void SponzaTest::shutdownGame(Application& app) {
  this->_pCameraController.reset();
}

void SponzaTest::createRenderState(Application& app) {
  const VkExtent2D& extent = app.getSwapChainExtent();
  this->_pCameraController->getCamera().setAspectRatio(
      (float)extent.width / (float)extent.height);

  SingleTimeCommandBuffer commandBuffer(app);

  std::string envMapName = "LuxuryRoom";

  // Environment map
  CesiumGltf::ImageCesium envMapImg = Utilities::loadHdri(
      GEngineDirectory + "/Content/HDRI_Skybox/" + envMapName + ".hdr");

  ImageOptions envMapOptions{};
  envMapOptions.width = static_cast<uint32_t>(envMapImg.width);
  envMapOptions.height = static_cast<uint32_t>(envMapImg.height);
  envMapOptions.mipCount = 1;
  // Utilities::computeMipCount(envMapOptions.width, envMapOptions.height);
  envMapOptions.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  envMapOptions.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT;
  this->_environmentMap.image =
      Image(app, commandBuffer, envMapImg.pixelData, envMapOptions);

  this->_environmentMap.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  SamplerOptions envSamplerOptions{};
  envSamplerOptions.mipCount = envMapOptions.mipCount;
  envSamplerOptions.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  envSamplerOptions.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  envSamplerOptions.minFilter = VK_FILTER_LINEAR;
  envSamplerOptions.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  this->_environmentMap.sampler = Sampler(app, envSamplerOptions);

  // TODO: create straight from image details?
  this->_environmentMap.view = ImageView(
      app,
      this->_environmentMap.image.getImage(),
      envMapOptions.format,
      envMapOptions.mipCount,
      1,
      VK_IMAGE_VIEW_TYPE_2D,
      VK_IMAGE_ASPECT_COLOR_BIT);

  // Prefiltered map
  std::vector<CesiumGltf::ImageCesium> prefilteredMips;
  prefilteredMips.reserve(5);
  for (uint32_t i = 1; i < 6; ++i) {
    prefilteredMips.push_back(Utilities::loadHdri(
        GEngineDirectory + "/Content/PrecomputedMaps/" + envMapName +
        "/Prefiltered" + std::to_string(i) + ".hdr"));
  }

  ImageOptions prefilteredMapOptions{};
  prefilteredMapOptions.width = static_cast<uint32_t>(prefilteredMips[0].width);
  prefilteredMapOptions.height =
      static_cast<uint32_t>(prefilteredMips[0].height);
  prefilteredMapOptions.mipCount = 5;
  prefilteredMapOptions.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  prefilteredMapOptions.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT;

  this->_prefilteredMap.image = Image(app, prefilteredMapOptions);
  this->_prefilteredMap.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT);

  for (uint32_t i = 0; i < 5; ++i) {
    this->_prefilteredMap.image
        .uploadMip(app, commandBuffer, prefilteredMips[i].pixelData, i);
  }

  this->_prefilteredMap.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  SamplerOptions prefMapSamplerOptions{};
  prefMapSamplerOptions.mipCount = prefilteredMapOptions.mipCount;
  prefMapSamplerOptions.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  prefMapSamplerOptions.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  prefMapSamplerOptions.minFilter = VK_FILTER_LINEAR;
  prefMapSamplerOptions.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

  this->_prefilteredMap.sampler = Sampler(app, prefMapSamplerOptions);
  this->_prefilteredMap.view = ImageView(
      app,
      this->_prefilteredMap.image.getImage(),
      prefilteredMapOptions.format,
      prefilteredMapOptions.mipCount,
      1,
      VK_IMAGE_VIEW_TYPE_2D,
      VK_IMAGE_ASPECT_COLOR_BIT);

  // Irradiance map
  CesiumGltf::ImageCesium irrMapImg = Utilities::loadHdri(
      GEngineDirectory + "/Content/PrecomputedMaps/" + envMapName +
      "/IrradianceMap.hdr");

  ImageOptions irrMapOptions{};
  irrMapOptions.width = static_cast<uint32_t>(irrMapImg.width);
  irrMapOptions.height = static_cast<uint32_t>(irrMapImg.height);
  // Generate mips?
  irrMapOptions.mipCount = 1;
  irrMapOptions.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  irrMapOptions.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT;

  this->_irradianceMap.image =
      Image(app, commandBuffer, irrMapImg.pixelData, irrMapOptions);

  this->_irradianceMap.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  SamplerOptions irrSamplerOptions{};
  irrSamplerOptions.mipCount = irrMapOptions.mipCount;
  irrSamplerOptions.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  irrSamplerOptions.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  irrSamplerOptions.minFilter = VK_FILTER_LINEAR;

  this->_irradianceMap.sampler = Sampler(app, irrSamplerOptions);
  this->_irradianceMap.view = ImageView(
      app,
      this->_irradianceMap.image.getImage(),
      irrMapOptions.format,
      irrMapOptions.mipCount,
      1,
      VK_IMAGE_VIEW_TYPE_2D,
      VK_IMAGE_ASPECT_COLOR_BIT);

  // BRDF LUT
  CesiumGltf::ImageCesium brdf =
      Utilities::loadPng(GEngineDirectory + "/Content/PrecomputedMaps/brdf_lut.png");

  ImageOptions brdfOptions{};
  brdfOptions.width = static_cast<uint32_t>(brdf.width);
  brdfOptions.height = static_cast<uint32_t>(brdf.height);
  brdfOptions.format = VK_FORMAT_R8G8B8A8_UNORM;
  brdfOptions.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  brdfOptions.mipCount = 1;

  this->_brdfLut.image = Image(app, commandBuffer, brdf.pixelData, brdfOptions);

  this->_brdfLut.image.transitionLayout(
      commandBuffer,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

  SamplerOptions brdfSamplerOptions{};
  brdfSamplerOptions.mipCount = brdfOptions.mipCount;
  brdfSamplerOptions.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  brdfSamplerOptions.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  brdfSamplerOptions.minFilter = VK_FILTER_LINEAR;

  this->_brdfLut.sampler = Sampler(app, brdfSamplerOptions);
  this->_brdfLut.view = ImageView(
      app,
      this->_brdfLut.image.getImage(),
      brdfOptions.format,
      1,
      1,
      VK_IMAGE_VIEW_TYPE_2D,
      VK_IMAGE_ASPECT_COLOR_BIT);

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

  // Global resources
  DescriptorSetLayoutBuilder globalResourceLayout;

  globalResourceLayout
      // Add slot for environmentMap
      .addTextureBinding()
      // Add slot for prefiltered map
      .addTextureBinding()
      // Add slot for irradianceMap
      .addTextureBinding()
      // BRDF LUT
      .addTextureBinding()
      // Global uniforms.
      .addUniformBufferBinding();

  this->_pGlobalResources =
      std::make_shared<PerFrameResources>(app, globalResourceLayout);
  this->_pGlobalUniforms =
      std::make_unique<TransientUniforms<GlobalUniforms>>(app, commandBuffer);

  std::vector<SubpassBuilder> subpassBuilders;

  // SKYBOX PASS
  {
    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments.push_back(0);
    Skybox::buildPipeline(app, subpassBuilder.pipelineBuilder);

    subpassBuilder.pipelineBuilder
        .layoutBuilder
        // Global resources (view, projection, skybox)
        .addDescriptorSet(this->_pGlobalResources->getLayout());
  }

  // REGULAR PASS
  {
    // Per-primitive material resources
    DescriptorSetLayoutBuilder primitiveMaterialLayout;
    Primitive::buildMaterial(primitiveMaterialLayout);

    this->_pGltfMaterialAllocator =
        std::make_unique<DescriptorSetAllocator>(app, primitiveMaterialLayout);

    SubpassBuilder& subpassBuilder = subpassBuilders.emplace_back();
    subpassBuilder.colorAttachments.push_back(0);
    subpassBuilder.depthAttachment = 1;

    Primitive::buildPipeline(subpassBuilder.pipelineBuilder);

    subpassBuilder
        .pipelineBuilder
        // Vertex shader
        .addVertexShader(GEngineDirectory + "/Shaders/GltfPBR.vert")
        // Fragment shader
        .addFragmentShader(GEngineDirectory + "/Shaders/GltfPBR.frag")

        // Pipeline resource layouts
        .layoutBuilder
        // Global resources (view, projection, environment map)
        .addDescriptorSet(this->_pGlobalResources->getLayout())
        // Material (per-object) resources (diffuse, normal map,
        // metallic-roughness, etc)
        .addDescriptorSet(this->_pGltfMaterialAllocator->getLayout());
  }

  this->_pRenderPass = std::make_unique<RenderPass>(
      app,
      std::move(attachments),
      std::move(subpassBuilders));

  std::vector<Subpass>& subpasses = this->_pRenderPass->getSubpasses();

  this->_pSponzaModel = std::make_unique<Model>(
      app,
      commandBuffer,
      // GEngineDirectory + "/Content/Models/Sponza/glTF/Sponza.gltf",
      // GEngineDirectory + "/Content/Models/FlightHelmet/FlightHelmet.gltf",
      // GEngineDirectory + "/Content/Models/AntiqueCamera.glb",
      GEngineDirectory + "/Content/Models/DamagedHelmet.glb",
      *this->_pGltfMaterialAllocator);

  this->_pGlobalResources->assign()
      .bindTexture(this->_environmentMap.view, this->_environmentMap.sampler)
      .bindTexture(this->_prefilteredMap.view, this->_prefilteredMap.sampler)
      .bindTexture(this->_irradianceMap.view, this->_irradianceMap.sampler)
      .bindTexture(this->_brdfLut.view, this->_brdfLut.sampler)
      .bindTransientUniforms(*this->_pGlobalUniforms);
}

void SponzaTest::destroyRenderState(Application& app) {
  this->_pSkybox.reset();
  this->_pSponzaModel.reset();
  this->_pRenderPass.reset();
  this->_pGlobalResources.reset();
  this->_pGlobalUniforms.reset();
  this->_pGltfMaterialAllocator.reset();
  this->_environmentMap = {};
  this->_prefilteredMap = {};
  this->_irradianceMap = {};
  this->_brdfLut = {};
}

void SponzaTest::tick(Application& app, const FrameContext& frame) {
  this->_pCameraController->tick(frame.deltaTime);
  const Camera& camera = this->_pCameraController->getCamera();

  const glm::mat4& projection = camera.getProjection();

  GlobalUniforms globalUniforms;
  globalUniforms.projection = camera.getProjection();
  globalUniforms.inverseProjection = glm::inverse(globalUniforms.projection);
  globalUniforms.view = camera.computeView();
  globalUniforms.inverseView = glm::inverse(globalUniforms.view);
  globalUniforms.lightDir = this->_lightDir;
  globalUniforms.time = static_cast<float>(frame.currentTime);

  this->_pGlobalUniforms->updateUniforms(globalUniforms, frame);
}

void SponzaTest::draw(
    Application& app,
    VkCommandBuffer commandBuffer,
    const FrameContext& frame) {

  VkDescriptorSet globalDescriptorSet =
      this->_pGlobalResources->getCurrentDescriptorSet(frame);

  this->_pRenderPass
      ->begin(app, commandBuffer, frame)
      // Bind global descriptor sets
      .setGlobalDescriptorSets(gsl::span(&globalDescriptorSet, 1))
      // Draw skybox
      .draw(*this->_pSkybox)
      .nextSubpass()
      // Draw Sponza model
      .draw(*this->_pSponzaModel);
}
} // namespace SponzaTest
} // namespace AltheaDemo