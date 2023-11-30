layout(set=0, binding=0) uniform SimUniforms {
  mat4 gridToWorld;
  mat4 worldToGrid;
  
  uint particleCount;
  uint spatialHashSize;
  uint spatialHashProbeSteps;
  uint jacobiIters;

  float deltaTime;
  float particleRadius;
  float detectionRadius;
  float padding;
};
