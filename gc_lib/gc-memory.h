#ifndef H_GC_MEMORY
#define H_GC_MEMORY

#include <string.h>

#include "gc.h"
#include "gc-declarations.h"

#include "debugging.h"

#define GC_ARENA_ALIGN_BITS 20
#define GC_ARENA_ALIGNMENT  (1<<GC_ARENA_ALIGN_BITS)
#define GC_ARENA_SIZE       GC_ARENA_ALIGNMENT
#define GC_ARENA_ALIGN_MASK (GC_ARENA_ALIGNMENT-1)

#define GC_ARENA_MAX_BYTEMAP_OFFSET  0x800
#define GC_ARENA_BYTEMAP_OFFSET_MASK 0x7f0

ArenaHeader * allocateAlignedArena(int segment);
ArenaHeader * allocateAlignedChunk(int segment, int length);

void freeArena(ArenaHeader * arena);

// Use the least significant non-zero bits (mod sizeof(void*)) of the aligned
// base address as an offset for the beginning of the arena to improve caching.
inline unsigned int arenaHeaderOffset(void * base) {
  unsigned int offset = (((uintptr_t)base >> GC_ARENA_ALIGN_BITS) &
                        GC_ARENA_BYTEMAP_OFFSET_MASK);
  assert(GC_ARENA_MAX_BYTEMAP_OFFSET > GC_ARENA_BYTEMAP_OFFSET_MASK);
  assert(offset >= 0 && offset < GC_ARENA_MAX_BYTEMAP_OFFSET);
  assert(offset % sizeof(void*) == 0);
  return offset;
}

inline uintptr_t getArenaEnd(ArenaHeader * arena) {
  return (uintptr_t)arena->first + (getObjectSize(arena)*getNumObjects(arena));
}

inline char * getBytemap(ArenaHeader * base) {
  return (char*)(base + 1);
}

inline ArenaHeader * chunkFromPtr(void * base) {
  base = (ArenaHeader*)(((uintptr_t)base & ~GC_ARENA_ALIGN_MASK));
  return (ArenaHeader*) ((uintptr_t)base + arenaHeaderOffset(base));
}

inline int getObjectBits(ArenaHeader * arena) {
  return arena->object_bits;
}

inline int getBytemapIndex(void * base, ArenaHeader * arena) {
  return ((uintptr_t)base - (uintptr_t)arena->first) >> getObjectBits(arena);
}

inline char * getMark(void * ptr) {
  ArenaHeader * arena = chunkFromPtr(ptr);
  char *        bm    = getBytemap(arena);
  int           idx   = getBytemapIndex(ptr, arena);
  return bm + idx;
}

inline void clearAllMarks(ArenaHeader * arena) {
  memset(getBytemap(arena), 0, arena->num_objects);
  arena->was_full = 0;
}

void buildGcSegmentSizeLookupTable();

#define __GC_SEGMENT_SIZE_LOOKUP_TABLE_SIZE \
  (LARGEST_FIXED_SEGMENT_SIZE/SLOT_SIZE)
int __gcSegmentSizeLookupTable[__GC_SEGMENT_SIZE_LOOKUP_TABLE_SIZE];

inline static int getFixedSegmentForLength(int length) {
  if (length >= __GC_SEGMENT_SIZE_LOOKUP_TABLE_SIZE) return -1;
  return __gcSegmentSizeLookupTable[length];
}


#endif
