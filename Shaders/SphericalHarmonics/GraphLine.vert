#version 460 core

layout(location = 0) out vec4 color;

layout(push_constant) uniform PushConstants {
  uint vertexCount;
};

// example function
float f(float x) {
  return x * x;
}

void main() {
  float x;

  // normalize between [0,1]
  // non-dotted
#ifndef DOTTED
  {
    // vertexCount = 2 + interiorPoints * 2
    // ==> 
    uint interiorPoints = (vertexCount - 2) / 2;
    uint segments = interiorPoints + 1;
    float segmentWidth = 1.0 / segments;

    if (gl_VertexIndex == 0) {
      x = 0.0;
    } else if (gl_VertexIndex == (vertexCount - 1)) {
      x = 1.0;
    } else {
      x = segmentWidth * ((gl_VertexIndex + 1) / 2);
    }
  }
#endif

  // dotted
#ifdef DOTTED
  {
    x = float(gl_VertexIndex) / float(vertexCount - 1);
  }
#endif

  // sample function
  float y = f(x);

  // convert from uv space to ndc
  gl_Position = 
      vec4(2.0 * x - 1.0, 1.0 - 2.0 * y, 0.0, 1.0);

  color = vec4(1.0, 0.0, 0.0, 1.0);
}