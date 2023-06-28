#pragma once

#include <Althea/Application.h>
#include <Althea/VertexBuffer.h>
#include <Althea/IndexBuffer.h>

#include <glm/glm.hpp>

#include <vulkan/vulkan.h>

using namespace AltheaEngine;

struct Sphere {
  VertexBuffer<glm::vec3> vertexBuffer;
  IndexBuffer indexBuffer;

  Sphere() = default;
  Sphere(Application& app, VkCommandBuffer commandBuffer);
};