#ifndef H_GC_MEMORY
#define H_GC_MEMORY

#include "gc.h"
#include "gc-declarations.h"

#include "debugging.h"

#define GC_ARENA_ALIGN_BITS 20
#define GC_ARENA_ALIGNMENT  (1<<GC_ARENA_ALIGN_BITS)
#define GC_ARENA_SIZE       GC_ARENA_ALIGNMENT
#define GC_ARENA_ALIGN_MASK (GC_ARENA_ALIGNMENT-1)

#define GC_ARENA_MAX_BYTEMAP_OFFSET 2000

ArenaHeader * allocateAlignedArena(int segment);
ArenaHeader * allocateAlignedChunk(int segment, int length);

void freeArena(ArenaHeader * arena);

// Offset the beginning of the area to the aligned base address for better
// cache efficiency
inline unsigned int arenaHeaderOffset(ArenaHeader * base) {
  unsigned int offset = (((uintptr_t)base >> GC_ARENA_ALIGN_BITS) &
                        (GC_ARENA_MAX_BYTEMAP_OFFSET - 1));
  // Ensure offset is word aligned
  offset &= ~(sizeof(void*) - 1);
  assert(offset >= 0 && offset < GC_ARENA_MAX_BYTEMAP_OFFSET);
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

#endif
