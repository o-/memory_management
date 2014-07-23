#ifndef H_ALIGNED_MEMORY
#define H_ALIGNED_MEMORY

#include <stdlib.h>

#define assert(t) \
  if (!(t)) { \
    printf("assertion '%s' failed: %s:%d\n", #t, __FILE__, __LINE__); \
    __asm("int3"); \
  }

void alignedMemoryManagerInit();

typedef struct MemoryChunkHeader MemoryChunkHeader;
struct MemoryChunkHeader {
  MemoryChunkHeader * next;
  unsigned int length;
  unsigned int raw_size;
};

int memoryChunkAlignment();

int isAligned(void * base, unsigned int align);

int isMaskable(void * ptr, MemoryChunkHeader * chunk);

void * chunkData(MemoryChunkHeader * base);

MemoryChunkHeader * chunkFromPtr(void * base);

MemoryChunkHeader * allocateAligned(size_t min_usable_length);

MemoryChunkHeader * allocateAlignedChunk();

void freeChunk(MemoryChunkHeader * chunk);

#endif
