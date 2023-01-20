#version 450

layout(location=0) smooth in vec3 direction;
layout(location=1) smooth in vec2 screenPosition;

layout(location=0) out vec4 color;

layout(set=0, binding=0) uniform sampler2D environmentMap; 
layout(set=0, binding=1) uniform sampler2D irradianceMap; 

void main() {
  // color = vec4(screenPosition, 0.0, 1.0);
  // color = texture(environmentMap, screenPosition);
  color = texture(irradianceMap, screenPosition);
}