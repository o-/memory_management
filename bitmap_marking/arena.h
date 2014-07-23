#ifndef H_MEMORY_ARENA
#define H_MEMORY_ARENA

#include <stdlib.h>
#include <stdint.h>

#define assert(t) \
  if (!(t)) { \
    printf("assertion '%s' failed: %s:%d\n", #t, __FILE__, __LINE__); \
    __asm("int3"); \
    printf(" "); \
  }

extern const int arenaAlignment;
extern const int arenaAlignBits;
extern const int arenaAlignMask;

void alignedMemoryManagerInit();

typedef struct ArenaHeader ArenaHeader;
struct ArenaHeader {
  ArenaHeader * next;
  size_t        object_size;
  unsigned char object_bits;
  unsigned int  num_objects;
  size_t        bytemap_size;
  unsigned int  arena_size;
  void *        first;
};

inline int getBytemapIndex(void * base, ArenaHeader * arena) {
  return ((uintptr_t)base - (uintptr_t)arena->first) >> arena->object_bits;
}

inline char * getBytemap(ArenaHeader * base) {
  return (char*)(base + 1);
}

inline int isAligned(void * base, unsigned int align) {
  return (uintptr_t)base % align == 0;
}

inline int isMaskable(void * ptr, ArenaHeader * chunk) {
  return (uintptr_t)ptr < (uintptr_t)chunk+arenaAlignment;
}

inline ArenaHeader * chunkFromPtr(void * base) {
  return (ArenaHeader*)((uintptr_t)base & ~arenaAlignMask);
}

inline void mark(void * ptr, char color) {
  ArenaHeader * arena = chunkFromPtr(ptr);
  char *        bm    = getBytemap(arena);
  int           idx   = getBytemapIndex(ptr, arena);
  bm[idx] = color;
}

inline char getMark(void * ptr) {
  ArenaHeader * arena = chunkFromPtr(ptr);
  char *        bm    = getBytemap(arena);
  int           idx   = getBytemapIndex(ptr, arena);
  return bm[idx];
}

int getNumberOfMarkBits(ArenaHeader * arena);
void inspectArena(ArenaHeader * arena);

void * chunkData(ArenaHeader * base);

ArenaHeader * allocateAligned(size_t object_size);
ArenaHeader * allocateAlignedArena(size_t object_size);

void freeArena(ArenaHeader * chunk);

#endif
