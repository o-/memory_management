#ifndef H_ALIGNED_MEMORY
#define H_ALIGNED_MEMORY

#include <stdlib.h>
#include <stdint.h>

#define assert(t) \
  if (!(t)) { \
    printf("assertion '%s' failed: %s:%d\n", #t, __FILE__, __LINE__); \
    __asm("int3"); \
  }

extern const int chunkAlignment;
extern const int chunkAlignBits;
extern const int chunkAlignMask;

void alignedMemoryManagerInit();

typedef struct MemoryChunkHeader MemoryChunkHeader;
struct MemoryChunkHeader {
  MemoryChunkHeader * next;
  unsigned int length;
  unsigned int raw_size;
};

inline int isAligned(void * base, unsigned int align) {
  return (uintptr_t)base % align == 0;
}

inline int isMaskable(void * ptr, MemoryChunkHeader * chunk) {
  return (uintptr_t)ptr < (uintptr_t)chunk+chunkAlignment;
}

inline MemoryChunkHeader * chunkFromPtr(void * base) {
  return (MemoryChunkHeader*)((uintptr_t)base & ~chunkAlignMask);
}

void * chunkData(MemoryChunkHeader * base);

MemoryChunkHeader * allocateAligned(size_t min_usable_length);
MemoryChunkHeader * allocateAlignedChunk();

void freeChunk(MemoryChunkHeader * chunk);

#endif
