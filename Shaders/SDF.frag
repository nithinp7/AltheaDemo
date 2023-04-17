
#version 450

#define PI 3.14159265359

layout(location=0) in vec3 direction;

layout(location=0) out vec4 GBuffer_Position;
layout(location=1) out vec4 GBuffer_Normal;
layout(location=2) out vec4 GBuffer_Albedo;
layout(location=3) out vec4 GBuffer_MetallicRoughnessOcclusion;

#define GLOBAL_UNIFORMS_SET 0
#define GLOBAL_UNIFORMS_BINDING 4
#include <GlobalUniforms.glsl>

float SDFSphere(vec3 pos) {
  // TODO: Don't hardcode
  vec3 c = vec3(10.0, 0.0, 0.0);
  float r = 5.0;
  float dist = length(pos - c);
  
  return dist - r;
}

float SDF(vec3 pos) {
  return SDFSphere(pos);
}

#define SDF_GRAD_DX 0.01
vec3 gradSDF(vec3 pos) {
  return 
      normalize(
        vec3(
          SDF(pos + vec3(SDF_GRAD_DX, 0.0, 0.0)) - SDF(pos - vec3(SDF_GRAD_DX, 0.0, 0.0)),
          SDF(pos + vec3(0.0, SDF_GRAD_DX, 0.0)) - SDF(pos - vec3(0.0, SDF_GRAD_DX, 0.0)),
          SDF(pos + vec3(0.0, 0.0, SDF_GRAD_DX)) - SDF(pos - vec3(0.0, 0.0, SDF_GRAD_DX))));
}

#define RAYMARCH_STEPS 50
float rayMarchSdf(vec3 startPos, vec3 dir) {
  vec3 curPos = startPos;
  float t = 0.0;
  for (int i = 0; i < RAYMARCH_STEPS; ++i) {
    float signedDist = SDF(curPos);
    if (signedDist <= 0.0001) {
      return t;
    }

    t += signedDist;
    curPos = startPos + t * dir;
  }

  return -1.0;
}

void main() {
  vec3 cameraPos = globals.inverseView[3].xyz;

  float t = rayMarchSdf(cameraPos, direction);

  if (t >= 0.0) {
    vec3 pos = cameraPos + t * direction;
    GBuffer_Position = vec4(pos, 1.0);
    GBuffer_Normal = vec4(gradSDF(pos), 1.0); 
    GBuffer_Albedo = vec4(1.0, 0.0, 0.0, 1.0);
  } else {
    GBuffer_Position = vec4(0.0);
    GBuffer_Normal = vec4(0.0);
    GBuffer_Albedo = vec4(0.0, 1.0, 0.0, 1.0);
  }

  // Occlusion should be 1 or 0 by default??
  GBuffer_MetallicRoughnessOcclusion = vec4(0.0, 0.15, 1.0, 1.0);
}
