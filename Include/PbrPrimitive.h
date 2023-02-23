#pragma once

#include <Althea/Application.h>
#include <Althea/DrawContext.h>
#include <Althea/GraphicsPipeline.h>
#include <Althea/IndexBuffer.h>
#include <Althea/Material.h>
#include <Althea/SingleTimeCommandBuffer.h>
#include <Althea/VertexBuffer.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace AltheaDemo {
struct Vertex {
  glm::vec3 position{};
  glm::vec3 normal{};
};

struct PbrPrimitiveConstants {
  glm::vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};
  float roughness = 1.0f;
  float metallic = 0.0f;
};

// TODO: support instancing
class PbrPrimitive {
public:
  glm::mat4 transform;

  static void buildPipeline(AltheaEngine::GraphicsPipelineBuilder& builder);
  static void
  buildMaterial(AltheaEngine::DescriptorSetLayoutBuilder& materialBuilder);

  PbrPrimitive(
      const AltheaEngine::Application& app,
      AltheaEngine::SingleTimeCommandBuffer& commandBuffer,
      std::vector<Vertex>&& vertices,
      std::vector<uint32_t>&& indices,
      const PbrPrimitiveConstants& constants,
      AltheaEngine::DescriptorSetAllocator& materialAllocator);

  void draw(const AltheaEngine::DrawContext& context) const;

private:
  PbrPrimitiveConstants _constants;

  AltheaEngine::VertexBuffer<Vertex> _vertexBuffer;
  AltheaEngine::IndexBuffer _indexBuffer;

  AltheaEngine::Material _material;
};
} // namespace AltheaDemo