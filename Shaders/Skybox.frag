#version 450

#define PI 3.14159265359

layout(location=0) smooth in vec3 direction;
layout(location=1) smooth in vec2 screenPosition;

layout(location=0) out vec4 color;

layout(set=0, binding=0) uniform sampler2D environmentMap; 
layout(set=0, binding=1) uniform sampler2D irradianceMap; 

void main() {
  // color = vec4(screenPosition, 0.0, 1.0);
  // color = texture(environmentMap, screenPosition);
  float yaw = atan(direction.z, direction.x);
  float pitch = -atan(direction.y, length(direction.xz));
  vec2 uv = vec2(0.5 * yaw, pitch) / PI + 0.5;
  // color = texture(environmentMap, uv);
  color = texture(irradianceMap, uv);
}