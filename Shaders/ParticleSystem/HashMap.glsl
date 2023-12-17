#ifndef _HASHMAP_
#define _HASHMAP_

// TODO: Actually fix this garbage up to be somewhat repurposable 
// Note: This whole thing is unused, just prototyped some ideas here

#define HASH_MASK 0xFFFF0000
#define VALUE_MASK 0x0000FFFF
#define INVALID_INDEX 0xFFFFFFFF

// Expects a spatial hash buffer and size to be defined called HASH_MAP_BUFFER and 
// HASH_MAP_SIZE respectively

// Can optionally specify a probe limit with HASH_MAP_PROBE_STEPS. Only applies to
// hash variants where probing is used.
#ifndef HASH_MAP_PROBE_STEPS
#define HASH_MAP_PROBE_STEPS 20
#endif

// Variant where 16 bit hashes and 16 bit values are packed into entries.
// Each unique hash will find a unique slot, probing linearly until an empty
// slot is found
// The previous value is returned
uint hashMapExchangeSlot_UniqueBucket_16(uint hash, uint value) {
  // just to be safe
  value = value & VALUE_MASK;

  uint hashUpper16 = hash << 16;
  uint entryLocation = hash % HASH_MAP_SIZE;

  uint entry = hashUpper16 | value;

  for (uint i = 0; i < HASH_MAP_PROBE_STEPS; ++i) {
    uint prevEntry = atomicCompSwap(HASH_MAP_BUFFER[entryLocation], INVALID_INDEX, entry);
     
    if (prevEntry == INVALID_INDEX) 
    {
      // Encountered an empty slot and replaced it with a newly created entry
      return INVALID_INDEX;
    }
    
    if ((prevEntry & CELL_HASH_MASK) == hashUpper16)
    {
      // An entry already exists for this hash, we have so far peaked it and know
      // that the key cannot change for this entry. We atomically swap out the current
      // value and return it.
      prevEntry = atomicExchange(HASH_MAP_BUFFER[entryLocation], entry);
      return prevEntry & VALUE_MASK;
    }
    
    // This is a non-empty entry corresponding to a different grid-cell hash, continue
    // linearly probing the hashmap.
    ++entryLocation;
    if (entryLocation == HASH_MAP_SIZE)
      entryLocation = 0;
  }

  return INVALID_INDEX;
}

// Variant where 32 bit values are packed into entries. If there is a hash collision, the existing entry 
// will be exchanged and returned regardless of if the hashes match.
// The previous value is returned
uint hashMapExchangeSlot_CollidingHashBucket_32(uint hash, uint value) {
  uint entryLocation = hash % HASH_MAP_SIZE;
  return atomicExchange(HASH_MAP_BUFFER[entryLocation], value);
}

#endif _HASHMAP_
