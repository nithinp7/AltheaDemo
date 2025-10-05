#include "TestCommon.hlsl"

VertexOutput main(uint vertexId : SV_VertexID) {
  VertexOutput OUT;
  float2 screenPos = float2((vertexId << 1) & 2, vertexId & 2);
  OUT.pos = float4(screenPos * 2.0f - 1.0f, 0.0f, 1.0f);
  OUT.uv = screenPos;
  OUT.color = float4(OUT.uv, 0.0, 1.0);
  return OUT;
}