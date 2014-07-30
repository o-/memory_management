#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <sys/mman.h>

#include "gc.h"
#include "gc-const.h"
#include "gc-declarations.h"
#include "gc-memory.h"

#include "debugging.h"

const int arenaStartAlign = 16;

extern inline char * getBytemap(ArenaHeader * base);
extern inline ArenaHeader * chunkFromPtr(void * base);
extern inline int getObjectBits(ArenaHeader * arena);
extern inline int getBytemapIndex(void * base, ArenaHeader * arena);
extern inline unsigned int arenaHeaderOffset(ArenaHeader * base);

extern inline uintptr_t getArenaEnd(ArenaHeader * arena);
extern inline char * getMark(void * ptr);

int getNumObjects(ArenaHeader * arena) {
  return arena->num_objects;
}

int isAligned(void * base, unsigned int align) {
  return (uintptr_t)base % align == 0;
}

int isMaskable(void * ptr, ArenaHeader * chunk) {
  return (uintptr_t)ptr < (uintptr_t)chunk+GC_ARENA_ALIGNMENT;
}

uintptr_t nextAlignedAddress(uintptr_t base) {
  if ((base & ~GC_ARENA_ALIGN_MASK) == base) return base;
  return (base+GC_ARENA_ALIGNMENT) & ~GC_ARENA_ALIGN_MASK;
}

size_t roundUpMemory(size_t required, unsigned int align) {
  if (required % align == 0) return required;
  int diff = (required + align) % align;
  return required + align - diff;
}

size_t calcNumOfObjects(ArenaHeader * arena, int total_size, int object_size) {
  assert(total_size > 1024);

  // Area:
  // arena_offset | AreaHeader | bytemap | start_align | object_space | padding
  //
  // sizeof(bytemap)      = num_objects
  // sizeof(object_space) = num_objects * object_size

  int header = sizeof(ArenaHeader) + arenaHeaderOffset(arena) + arenaStartAlign;

  int num_objects = (total_size - header) / (1 + object_size);

  assert(num_objects > 0);
  assert(total_size >= header + num_objects +
                       (num_objects * object_size));

  // Ensure The bytemaps have a word aligned size
  num_objects &= ~(sizeof(void*) - 1);

  return (size_t)num_objects;
}

ArenaHeader * allocateAligned(int variable_length) {
  assert(1<<GC_ARENA_ALIGN_BITS == GC_ARENA_ALIGNMENT);
  assert(GC_ARENA_ALIGNMENT % sysconf(_SC_PAGESIZE) == 0);
  assert(GC_ARENA_SIZE      % sysconf(_SC_PAGESIZE) == 0);
  assert(GC_ARENA_SIZE <= GC_ARENA_ALIGNMENT);

  int OSPageAlignment = sysconf(_SC_PAGESIZE);

  int aligned_length = roundUpMemory(variable_length, OSPageAlignment);

  void * commited = NULL;

#ifdef USE_POSIX_MEMALIGN
  posix_memalign(&commited, GC_ARENA_ALIGNMENT, aligned_length);
  if(commited == NULL) return NULL;

#else

  // Length aligned to OS page size (required for mmap)
  assert(aligned_length % OSPageAlignment == 0);
  // Virtual memory needed to satisfy chunk alignment
  size_t request_length = roundUpMemory(aligned_length + GC_ARENA_ALIGNMENT,
                                        OSPageAlignment);

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
  commited = mmap(aligned_base_ptr,
                  aligned_length,
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                  -1,
                  0);
  if (commited != aligned_base_ptr) {
    munmap(aligned_base_ptr, aligned_length);
    return NULL;
  }

  assert(commited == aligned_base_ptr);
#endif

  ArenaHeader * arena = (ArenaHeader*) ((uintptr_t)commited +
                                        arenaHeaderOffset(commited));

  arena->raw_size = aligned_length;
  arena->raw_base = commited;
  return arena;
}

ArenaHeader * allocateAlignedArena(int segment) {
  ArenaHeader * chunk = NULL;
  assert(segment < NUM_FIXED_HEAP_SEGMENTS);

  chunk = allocateAligned(GC_ARENA_SIZE);

  chunk->segment           = segment;
  int num_objects          = calcNumOfObjects(chunk,
                                              GC_ARENA_SIZE,
                                              heapSegmentNodeSize(segment));
  chunk->num_objects       = num_objects;

  chunk->first             = &getBytemap(chunk)[num_objects];

  uintptr_t f = (uintptr_t)chunk->first;
  if (f % arenaStartAlign != 0) {
    chunk->first = (void*)(f + f % arenaStartAlign);
  }

  chunk->free              = chunk->first;
  chunk->free_list         = NULL;
  chunk->num_alloc         = 0;
  chunk->object_size       = heapSegmentNodeSize(segment);
  chunk->object_bits       = log2(heapSegmentNodeSize(segment));

  assert(f + (chunk->num_objects * chunk->object_size) <=
         (uintptr_t)chunk->raw_base + chunk->raw_size);

  // Zero bytemap
  memset(getBytemap(chunk), 0, num_objects);

  return chunk;
}

ArenaHeader * allocateAlignedChunk(int segment, int length) {
  assert(segment >= NUM_FIXED_HEAP_SEGMENTS);

  int object_size = objectLengthToSize(length);

  int header = sizeof(ArenaHeader) + GC_ARENA_MAX_BYTEMAP_OFFSET +
               arenaStartAlign;

  ArenaHeader * arena = allocateAligned(object_size + header);

  arena->num_alloc         = 1;
  arena->segment           = segment;
  arena->num_objects       = 1;
  arena->object_size       = object_size;
  arena->object_bits       = GC_ARENA_ALIGN_BITS;
  arena->free_list         = NULL;
  arena->first             = &getBytemap(arena)[1];
  uintptr_t f = (uintptr_t)arena->first;
  if (f % arenaStartAlign != 0) {
    arena->first = (void*)(f + f % arenaStartAlign);
  }
  arena->free              = (void*)getArenaEnd(arena);

  // Zero bytemap
  memset(getBytemap(arena), 0, 1);

  return arena;
}


void freeArena(ArenaHeader * arena) {
#ifdef USE_POSIX_MEMALIGN
  free(arena);
#else
  munmap(arena->raw_base, arena->raw_size);
#endif
}


