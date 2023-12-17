
#version 450

layout(local_size_x = LOCAL_SIZE_X) in;

#include "SimResources.glsl"

void main() {
  uint slotIdx = uint(gl_GlobalInvocationID.x);
  if (slotIdx >= spatialHashSize) {
    return;
  }

  // Is this hacky??
  if (slotIdx < freeListsCount)
  {
    nextFreeBucket[slotIdx] = 0;
  }

  // ?????
  barrier();

  allocateBucketForCell(slotIdx);
}
