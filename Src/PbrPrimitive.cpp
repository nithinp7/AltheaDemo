#include "PbrPrimitive.h"

using namespace AltheaEngine;

namespace AltheaDemo {
/*static*/
void PbrPrimitive::buildPipeline(GraphicsPipelineBuilder& builder) {
  builder.setPrimitiveType(PrimitiveType::TRIANGLES)
      .addVertexInputBinding<AltheaDemo::Vertex>()
      .addVertexAttribute(VertexAttributeType::VEC3, offsetof(Vertex, position))
      .addVertexAttribute(VertexAttributeType::VEC3, offsetof(Vertex, normal))

      .addVertexShader(GProjectDirectory + "/Shaders/PBR.vert")
      .addFragmentShader(GProjectDirectory + "/Shaders/PBR.frag")

      .layoutBuilder.addPushConstants<glm::mat4>();
}

/*static*/
void PbrPrimitive::buildMaterial(DescriptorSetLayoutBuilder& materialBuilder) {
  materialBuilder.addConstantsBufferBinding<PbrPrimitiveConstants>();
}

PbrPrimitive::PbrPrimitive(
    const Application& app,
    SingleTimeCommandBuffer& commandBuffer,
    std::vector<AltheaDemo::Vertex>&& vertices,
    std::vector<uint32_t>&& indices,
    const PbrPrimitiveConstants& constants,
    DescriptorSetAllocator& materialAllocator)
    : transform(1.0f),
      _constants(constants),
      _vertexBuffer(app, commandBuffer, std::move(vertices)),
      _indexBuffer(app, commandBuffer, std::move(indices)),
      _material(app, materialAllocator) {
  this->_material.assign().bindInlineConstants(this->_constants);
}

void PbrPrimitive::draw(const AltheaEngine::DrawContext& context) const {
  context.bindDescriptorSets(this->_material);
  context.updatePushConstants(this->transform, 0);
  context.drawIndexed(this->_vertexBuffer, this->_indexBuffer);
}
} // namespace AltheaDemo