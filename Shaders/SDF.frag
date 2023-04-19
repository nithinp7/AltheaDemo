
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

struct Material {
  vec3 color;
  float roughness;
  float metallic;
  float occlusion;
  float thinness;
};

// Random number generator and sample warping
// from ShaderToy https://www.shadertoy.com/view/4tXyWN
float rng(uvec2 seed) {
    seed += uvec2(1);
    uvec2 q = 1103515245U * ( (seed >> 1U) ^ (seed.yx) );
    uint  n = 1103515245U * ( (q.x) ^ (q.y >> 3U) );
    return float(n) * (1.0 / float(0xffffffffU));
}

float SDFBox(vec3 query, vec3 bounds ) {
  vec3 q = abs(query) - bounds;
  return length(max(q,0.0)) + min(max(q.x,max(q.y,q.z)),0.0);
}

float SDFSphere(vec3 center, float radius, vec3 pos) {
  float dist = length(pos - center);
  return dist - radius;
}

float smoothMin( float a, float b, float k ) {
    float h = max(k - abs(a - b), 0.0) / k;
    return min(a, b) - h * h * k * 0.25;
}

float hollow(float sdf, float thickness) {
  return abs(sdf) - thickness;
}

mat3 Rx(float deg) {
  float rad = radians(deg);
  float c = cos(rad);
  float s = sin(rad);

  mat3 R = mat3(1.0);
  R[1][1] = c;
  R[2][1] = s;
  R[1][2] = -s;
  R[2][2] = c;

  return R;
}

mat3 Ry(float deg) {
  float rad = radians(deg);
  float c = cos(rad);
  float s = sin(rad);

  mat3 R = mat3(1.0);
  R[0][0] = c;
  R[2][0] = -s;
  R[0][2] = s;
  R[2][2] = c;

  return R;
}

mat3 Rz(float deg) {
  float rad = radians(deg);
  float c = cos(rad);
  float s = sin(rad);

  mat3 R = mat3(1.0);
  R[0][0] = c;
  R[1][0] = s;
  R[0][1] = -s;
  R[1][1] = c;

  return R;
}

float SDFBoilingGold(vec3 pos) {
  uvec2 cell = uvec2(pos.xz / vec2(50.0));
  pos.xz = mod(pos.xz, vec2(50.0));
  float dist = SDFBox(pos - vec3(0.0, -10.0, 0.0), vec3(50.0, 5.0, 50.0));
  int holeCount = 10;
  for (int i = 0; i < holeCount; ++i) {
    for (int j = 0; j < holeCount; ++j) {
      vec3 holePos = 
          vec3(
            rng(uvec2(3 * i, 7 * j) + 5 * cell) * 100.0 - 50.0,
            -4.0,
            rng(uvec2(j, i) + 3 * cell) * 100.0 - 50.0);
      dist = -smoothMin(-dist, SDFSphere(holePos, 4.0, pos), 10.0 + 8.5 * sin(globals.time + i + j + cell.x + cell.y));
    }
  }

  return dist;
}

float SDFOcean(vec3 pos) {
  pos += vec3(100.0, 10.0, 100.0);
  pos *= 10.0;

  float cellSize = 50.0;
  ivec2 cell = ivec2(pos.xz / vec2(cellSize));
  if (cell.x < 1 || cell.y < 1) {// || cell.x > 20 || cell.y > 20) {
    return 1000.0;
  }

  pos.xz = mod(pos.xz, vec2(cellSize));
  float dist = 1000.0;
  int subGridSize = 1;
  float subcellSize = cellSize;
  for (int i = -1; i <= 1; ++i) {
    for (int j = -1; j <= 1; ++j) {
      if (cell.x + i < 0 || cell.y + j < 0) {
        continue;
      }

      uvec2 fullGridCell = uvec2(cell.xy * subGridSize + ivec2(i, j));
      
      for (int k = 0; k < 4; ++k) {
        vec3 holePos = 
            vec3(
              (i + 0.5 + 0.5 * sin(globals.time + 3 * fullGridCell.x + 7 * fullGridCell.y + k)) * subcellSize,
              25.0 * sin(globals.time + 5.0 * fullGridCell.x + 2.0 * fullGridCell.y + k),
              (j + 0.5 + 0.5 * sin(globals.time + 2.0 * fullGridCell.x + 5.0 * fullGridCell.y + k)) * subcellSize);
        dist = smoothMin(dist, SDFSphere(holePos, 10.0, pos), 15.0 + 10.0 * sin(globals.time + fullGridCell.x + fullGridCell.y));
      }
    }
  }

  // return pos.y;
  // return dist;
  // return max(pos.y, dist);
  // return max(pos.y, -dist);
  return -smoothMin(-pos.y, dist, 100.0) / 10.0;;//, dist);
}

float SDF(vec3 pos) {
  // pos = Ry(30.0 * globals.time) * pos;
  float dist;

  float ocean = SDFOcean(pos);
  dist = ocean;

  //SDFSphere(vec3(-2.0, 0.0, 4.0), 8.0, pos);
  float eyeBall = SDFSphere(vec3(5.0, 0.0, 0.0), 5.0, pos); 
  float pupil = SDFSphere(vec3(7.0, 0.0, 0.0), 3.17, pos);
  dist = min(dist, max(eyeBall, -pupil));

  float sphere1 = hollow(SDFSphere(vec3(25.0, 0.0, 0.0), 4.0, pos), 0.15);//0.5 + 0.25 * sin(3.0 * globals.time)); 
  float sphere2 = hollow(SDFSphere(vec3(45.0, 0.0, 0.0), 5.0, pos), 0.25); 

  dist = min(sphere1, dist);
  dist = min(sphere2, dist);

  return dist;

  // float sphereB = SDFSphere(vec3(3.0, 0.0, 7.0), 5.0, pos);

  // return smoothMin(sphereA, sphereB, 4.0);
  // return smoothMin(sphereA, sphereB, 4.0) + (sin(0.1 * pos.x * pos.y + 0 * globals.time)) * cos(0.25 * pos.z) * sin(0.5 * pos.x * pos.y + pos.z + 0 *globals.time);
}

#define AO_RAYMARCH_STEPS 5
#define AO_RAYMARCH_STEPSIZE 0.085
float rayMarchOcclusion(vec3 startPos, vec3 n) {
  float k = 5.0;
  float ao = 1.0;
  for (int i = 1; i <= AO_RAYMARCH_STEPS; ++i) {
    vec3 curPos = startPos + i * AO_RAYMARCH_STEPSIZE * n;
    float signedDist = SDF(curPos);
    ao -= k / pow(2.0, i) * (i * AO_RAYMARCH_STEPSIZE - signedDist);
  }

  return ao;
}

Material SDFMaterial(vec3 pos, vec3 rayDir, vec3 normal) {
  Material mat;
  mat.metallic = 0.0;
  mat.roughness = 0.1f;
  mat.occlusion = rayMarchOcclusion(pos, normal);
  vec3 gold = vec3(0.85, 0.7, 0.15);
  vec3 water = vec3(0.01, 0.1, 0.15);

  if (abs(SDFOcean(pos)) < 0.01) {
    mat.color = water;
  }

  if ((SDFSphere(vec3(25.0, 0.0, 0.0), 5.0, pos)) < 0.01) {
    mat.color = vec3(1.0, 0.0, 0.0);
    mat.roughness = 0.5;
  }

  if ((SDFSphere(vec3(45.0, 0.0, 0.0), 6.0, pos)) < 0.01) {
    mat.color = vec3(1.0, 0.0, 0.0);
    mat.roughness = 0.0;
  }

  // float ao = rayMarchOcclusion(pos, rayDir);

  float eyeBall = SDFSphere(vec3(5.0, 0.0, 0.0), 5.05, pos);
  float iris = SDFSphere(vec3(6.5, 0.0, 0.0), 4.0, pos);
  float pupil = SDFSphere(vec3(7.0, 0.0, 0.0), 3.2, pos);

  if (eyeBall < 0.0) {
    mat.color = vec3(1.0);
    mat.metallic = 0.0;
  }

  if (iris < 0.0) {
    mat.color = vec3(0.25, 0.55, 0.85);
    mat.metallic = 0.0;
  } 

  if (pupil < 0.0) {
    mat.color = vec3(0.0);
    mat.metallic = 0.0;
  }

  return mat;
}

#define SDF_GRAD_DX 0.001
vec3 gradSDF(vec3 pos) {
  return 
      normalize(
        vec3(
          SDF(pos + vec3(SDF_GRAD_DX, 0.0, 0.0)) - SDF(pos - vec3(SDF_GRAD_DX, 0.0, 0.0)),
          SDF(pos + vec3(0.0, SDF_GRAD_DX, 0.0)) - SDF(pos - vec3(0.0, SDF_GRAD_DX, 0.0)),
          SDF(pos + vec3(0.0, 0.0, SDF_GRAD_DX)) - SDF(pos - vec3(0.0, 0.0, SDF_GRAD_DX))));
}

#define RAYMARCH_STEPS 100
float rayMarchSdf(vec3 startPos, vec3 dir) {
  vec3 curPos = startPos;
  float t = 0.0;
  for (int i = 0; i < RAYMARCH_STEPS; ++i) {
    float signedDist = SDF(curPos);
    if (signedDist <= 0.001) {
      return t;
    }

    t += signedDist;
    curPos = startPos + t * dir;
  }

  return -1.0;
}

// vec3 subsurfaceColor(vec3 viewDir, vec3 normal, float thinness) {
//   vec3 lightDir = -viewDir;
//   float distortion = 1.0;
//   float glow = 1.0;
//   float scale = 1.0;

//   vec3 scatterDir = lightDir + normal * distortion;
//   float lightReachingEye = pow(clamp(dot(viewDir, -scatterDir), 0.0, 1.0), glow) * scale;
//   float attenuation = max(0.0, dot(normal, lightDir) + 1.0);
//   float totalLight = attenuation * (lightReachingEye + )
// }

void main() {
  vec3 cameraPos = globals.inverseView[3].xyz;

  vec3 rayDir = normalize(direction);
  float t = rayMarchSdf(cameraPos, rayDir);

  if (t >= 0.0) {
    vec3 pos = cameraPos + t * rayDir;
    vec3 normal = gradSDF(pos);
    Material mat = SDFMaterial(pos, rayDir, normal);

    GBuffer_Position = vec4(pos, 1.0);
    GBuffer_Normal = vec4(normal, 1.0); 
    GBuffer_Albedo = vec4(mat.color, 1.0);
    GBuffer_MetallicRoughnessOcclusion = vec4(mat.metallic, mat.roughness, mat.occlusion, 1.0);
  } else {
    GBuffer_Position = vec4(0.0);
    GBuffer_Normal = vec4(0.0);
    GBuffer_Albedo = vec4(0.0, 1.0, 0.0, 1.0);
    GBuffer_MetallicRoughnessOcclusion = vec4(0.0, 0.15, 1.0, 1.0);
  }

  // Occlusion should be 1 or 0 by default??
}
