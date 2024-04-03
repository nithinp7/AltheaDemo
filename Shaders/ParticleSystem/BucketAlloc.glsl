
#version 450

layout(local_size_x = LOCAL_SIZE_X) in;

#include "SimResources.glsl"

void main() {
  uint slotIdx = uint(gl_GlobalInvocationID.x);
  if (slotIdx >= simUniforms.spatialHashSize) {
    return;
  }

  // Is this hacky??
  if (slotIdx < simUniforms.freeListsCount)
  {
    getBucketFreeList(slotIdx) = 0;
  }

  // ?????
  barrier();

  allocateBucketForCell(slotIdx);
}
