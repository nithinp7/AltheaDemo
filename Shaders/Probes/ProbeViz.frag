#version 460 

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec4 color;

void main() {
  color = vec4(inColor, 1.0);
}