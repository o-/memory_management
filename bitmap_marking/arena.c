#include "arena.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <sys/mman.h>

static int OSPageAlignment;

#define _CHUNK_ALIGN_BITS 20
const int arenaAlignment = 1<<_CHUNK_ALIGN_BITS;
const int arenaAlignBits = _CHUNK_ALIGN_BITS;
const int arenaAlignMask = (1<<_CHUNK_ALIGN_BITS)-1;

size_t roundUpMemory(size_t required, unsigned int align);

extern inline int isAligned(void * base, unsigned int align);
extern inline int isMaskable(void * ptr, ArenaHeader * chunk);
extern inline ArenaHeader * chunkFromPtr(void * base);
extern inline char * getBytemap(ArenaHeader * base);
extern inline char getMark(void * ptr);
extern inline void mark(void * ptr, char color);
extern inline int getBytemapIndex(void * base, ArenaHeader * arena);
extern inline char * getBytemap(ArenaHeader * base);

void alignedMemoryManagerInit() {
  OSPageAlignment = sysconf(_SC_PAGESIZE);
  assert(1<<arenaAlignBits == arenaAlignment);
  assert(arenaAlignment % OSPageAlignment == 0);
}

size_t calcNumOfObjects(int total_size, int object_size) {
  assert(total_size > sizeof(ArenaHeader));

  // First estimate
  int usable       = total_size - sizeof(ArenaHeader);
  int bytemap_size = usable / object_size;
  usable          -= bytemap_size;

  // Increase as long as the bytemap fits
  assert(((float)usable / (float)object_size) <= bytemap_size);
  while (((float)usable / (float)object_size) <= bytemap_size) {
    bytemap_size--;
    usable++;
  }

  assert(usable > 0);

  int num_objects = usable/object_size;

  assert(bytemap_size >= num_objects);

  return (size_t)num_objects;
}

size_t roundUpMemory(size_t required, unsigned int align) {
  int diff = (required + align) % align;
  return required + align - diff;
}

uintptr_t nextAlignedAddress(uintptr_t base) {
  if ((base & ~arenaAlignMask) == base) return base;
  return (base+arenaAlignment) & ~arenaAlignMask;
}

void inspectArena(ArenaHeader * arena) {
  printf("Arena : %p = {\n", arena);
  printf("  object_size  : %d\n", (int)arena->object_size);
  printf("  object_bits  : %d\n", arena->object_bits);
  printf("  num_objects  : %d\n", arena->num_objects);
  printf("  arena_size   : %d\n", arena->arena_size);
  printf("  mark_bits    : [");
  char * bm    = getBytemap(arena);
  int    count = 0;
  int    col   = 0;
  int    total = 0;
  for (int i = 0; i < arena->num_objects; i++) {
    if (bm[i] != 0) {
      count++;
    }
    if (i % 512 == 511) {
      printf("%4i", count);
      total += count;
      count = 0;
      col++;
    }
    if (col == 24) {
      printf("\n                  ");
      col = 0;
    }
  }
  total += count;
  printf("] (%d) \n}\n", total);
}

int getNumberOfMarkBits(ArenaHeader * arena) {
  char * bm    = getBytemap(arena);
  int    count = 0;
  for (int i = 0; i < arena->num_objects; i++) {
    if (bm[i] != 0) {
      count++;
    }
  }
  return count;
}

ArenaHeader * allocateAligned(size_t object_size) {
  int object_bits = log2(object_size);
  assert (1<<object_bits == object_size);

  int aligned_length = arenaAlignment;
  // Length aligned to OS page size (required for mmap)
  assert(aligned_length % OSPageAlignment == 0);
  // Virtual memory needed to satisfy chunk alignment
  size_t request_length = roundUpMemory(2 * aligned_length, OSPageAlignment);

  // Reserve virtual memory
  void * reserved = mmap(NULL,
                         request_length,
                         PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                         -1,
                         0);
  if (reserved == NULL) {
    return NULL;
  }

  // Determine next chunk aligned base address
  uintptr_t base          = (uintptr_t)reserved;
  uintptr_t aligned_base  = nextAlignedAddress(base);
  void * aligned_base_ptr = (void*)aligned_base;
  assert(aligned_base >= base);

  // Chop off unused pages before and after the aligned block
  int prefix = (size_t)aligned_base - base;
  assert(prefix >= 0);
  if (prefix > 0) {
    munmap((void*)base, prefix);
  }

  int suffix = (base + request_length) - (aligned_base + aligned_length);
  assert(suffix >= 0);
  if (suffix > 0) {
    munmap((void*)(aligned_base + aligned_length), suffix);
  }

  assert(request_length = prefix + aligned_length + suffix);

  // Reserve memory for the aligned block
  void * commited = mmap(aligned_base_ptr,
                         aligned_length,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                         -1,
                         0);
  if (commited == NULL) {
    munmap(aligned_base_ptr, aligned_length);
    return NULL;
  }

  assert(commited == aligned_base_ptr);
  ArenaHeader * chunk = (ArenaHeader*) aligned_base_ptr;

  int num_objects     = calcNumOfObjects(aligned_length, object_size);

  chunk->num_objects  = num_objects;
  chunk->object_size  = object_size;
  chunk->object_bits  = object_bits;
  chunk->arena_size   = aligned_length;
  chunk->first        = (void*) ((uintptr_t)(chunk + 1) + num_objects);

  assert((num_objects+1)*object_size + sizeof(ArenaHeader) <= aligned_length);

  // Zero bytemap
  memset(getBytemap(chunk), 0, num_objects);

  return chunk;
}

void freeArena(ArenaHeader * chunk) {
  munmap(chunk, chunk->arena_size);
}

ArenaHeader * allocateAlignedArena(size_t object_size) {
  return allocateAligned(object_size);
}
