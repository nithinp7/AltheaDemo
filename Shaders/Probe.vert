#version 450

layout(location=0) in vec3 pos;

layout(set=0, binding=4) uniform UniformBufferObject {
  mat4 projection;
  mat4 inverseProjection;
  mat4 view;
  mat4 inverseView;
  vec3 lightDir;
  float time;
  float exposure;
} globals;

layout(push_constant) uniform PushConstants {
  mat4 model;
} pushConstants;

layout(location=0) out vec3 normalOut;
layout(location=1) out vec3 directionOut;

void main() {
  vec3 cameraPos = globals.inverseView[3].xyz;
  vec4 worldPos = pushConstants.model * vec4(pos, 1.0);

  directionOut = worldPos.xyz - cameraPos;
  normalOut = normalize(pos);

  gl_Position = globals.projection * globals.view * worldPos;
}