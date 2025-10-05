#include "TestCommon.hlsl"

struct PixelOut {
  float4 color : SV_Target0;
};

PixelOut main(VertexOutput IN) {
  PixelOut OUT;
  OUT.color = IN.color;
  return OUT;
}