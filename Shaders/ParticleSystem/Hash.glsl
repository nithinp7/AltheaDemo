
// From Mathias Muller
uint hashCoords(int x, int y, int z) {
  return abs((x * 92837111) ^ (y * 689287499) ^ (z * 283923481));
}
