#include "gc.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <sys/mman.h>

#define VariableHeapSegment 7
#define FixedHeapSegments   7
#define HeapSegments (VariableHeapSegment + 1)
static int HeapSegmentSize[FixedHeapSegments] = {8, 16, 32, 64, 128, 256, 512};

typedef struct Heap {
  ArenaHeader * free_arena[HeapSegments];
  ArenaHeader * full_arena[HeapSegments];
} Heap;

static Heap HEAP;

#define _CHUNK_ALIGN_BITS 20
const int arenaAlignment = 1<<_CHUNK_ALIGN_BITS;
const int arenaAlignBits = _CHUNK_ALIGN_BITS;
const int arenaAlignMask = (1<<_CHUNK_ALIGN_BITS)-1;

size_t roundUpMemory(size_t required, unsigned int align);

extern inline int getBytemapIndex(void * base, ArenaHeader * arena) {
  return ((uintptr_t)base - (uintptr_t)arena->first) >> arena->object_bits;
}

extern inline char * getBytemap(ArenaHeader * base) {
  return (char*)(base + 1);
}

inline int isAligned(void * base, unsigned int align) {
  return (uintptr_t)base % align == 0;
}

inline int isMaskable(void * ptr, ArenaHeader * chunk) {
  return (uintptr_t)ptr < (uintptr_t)chunk+arenaAlignment;
}

extern inline ArenaHeader * chunkFromPtr(void * base) {
  return (ArenaHeader*)((uintptr_t)base & ~arenaAlignMask);
}

extern inline void mark(void * ptr, char color) {
  ArenaHeader * arena = chunkFromPtr(ptr);
  char *        bm    = getBytemap(arena);
  int           idx   = getBytemapIndex(ptr, arena);
  bm[idx] = color;
}

extern inline char getMark(void * ptr) {
  ArenaHeader * arena = chunkFromPtr(ptr);
  char *        bm    = getBytemap(arena);
  int           idx   = getBytemapIndex(ptr, arena);
  return bm[idx];
}

size_t calcNumOfObjects(int total_size, int object_size) {
  assert(total_size > sizeof(ArenaHeader));

  // First estimate
  int usable       = total_size - sizeof(ArenaHeader);
  int bytemap_size = usable / object_size;
  usable          -= bytemap_size;

  // Increase as long as the bytemap fits
  assert(((float)usable / (float)object_size) < bytemap_size);
  while (((float)usable / (float)object_size) < bytemap_size) {
    bytemap_size--;
    usable++;
  }
  bytemap_size++;
  usable--;

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

ArenaHeader * allocateAligned(size_t object_size) {
  int OSPageAlignment = sysconf(_SC_PAGESIZE);

  assert(1<<arenaAlignBits == arenaAlignment);
  assert(arenaAlignment % OSPageAlignment == 0);

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

  int num_objects      = calcNumOfObjects(aligned_length, object_size);

  chunk->num_objects   = num_objects;
  chunk->object_size   = object_size;
  chunk->object_bits   = object_bits;
  chunk->arena_size    = aligned_length;
  chunk->first         = (void*) ((uintptr_t)(chunk + 1) + num_objects);
  chunk->free          = chunk->first;
  chunk->free_list     = NULL;

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

extern inline uintptr_t getArenaEnd(ArenaHeader * arena) {
  return (uintptr_t)arena->first + (arena->object_size * arena->num_objects);
}

ObjectHeader * allocFromArena(ArenaHeader * arena) {
  if (arena->free_list != NULL) {
    // Freelist allocation in recycled space
    ObjectHeader * o = (ObjectHeader*)arena->free_list;
    arena->free_list = arena->free_list->next;
    return o;
  }
  if ((uintptr_t)arena->free < getArenaEnd(arena)) {
    // Bump pointer allocation in virgin space
    ObjectHeader * o = (ObjectHeader*)arena->free;
    arena->free = arena->free + arena->object_size;
    return o;
  }
  return NULL;
}

ArenaHeader * newArena(int segment) {
  ArenaHeader * new_arena = allocateAlignedArena(HeapSegmentSize[segment]);
  ArenaHeader * first     = HEAP.free_arena[segment];
  new_arena->next = first;
  HEAP.free_arena[segment] = new_arena;
  debug("allocating new arena %p in segment %d\n", new_arena, segment);
  return new_arena;
}

ObjectHeader * allocFromSegment(int segment) {
  ArenaHeader * arena = HEAP.free_arena[segment];
  while (1) {
    if (arena == NULL) {
      arena = newArena(segment);
      if (arena == NULL) return NULL;
    }
      debug("allocating object in arena %p\n", arena);

    ObjectHeader * o = allocFromArena(arena);
    if (o != NULL) {
      assert(chunkFromPtr(o) == arena);
      return o;
    }

    // This arena is full
    ArenaHeader * next = arena->next;

    ArenaHeader * full = HEAP.full_arena[segment];
    arena->next = full;
    HEAP.full_arena[segment] = arena;

    HEAP.free_arena[segment] = next;
    arena = next;
  }
}

ObjectHeader * alloc(size_t length) {
  assert(length >= 0);
  int size    = sizeof(ObjectHeader) + (length * sizeof(double));
  int segment = FixedHeapSegments/2;
  int step    = FixedHeapSegments/4+1;

  // Bin search for segment size
  while (step > 0) {
    if (HeapSegmentSize[segment] < size) {
      segment += step;
    } else if (HeapSegmentSize[segment-1] < size) {
      break;
    } else {
      segment -= step;
    }
    assert(segment >= 0 && segment < FixedHeapSegments);
    step = (step >> 1);
  }
  if (HeapSegmentSize[segment] < size) {
    segment = VariableHeapSegment;
  }

  assert(!(size < HeapSegmentSize[FixedHeapSegments] &&
           segment == VariableHeapSegment));
  assert(segment >=0 &&
         (segment < FixedHeapSegments || segment == VariableHeapSegment));
  assert(segment == VariableHeapSegment ||
         (HeapSegmentSize[segment] >= size &&
          HeapSegmentSize[segment-1] < size));

  ObjectHeader * o = allocFromSegment(segment);
  if (o == NULL) return NULL;
  o->length = length;
  return o;
}

void initGc() {
  for (int i = 0; i < HeapSegments; HEAP.free_arena[i++] = NULL);
  for (int i = 0; i < HeapSegments; HEAP.full_arena[i++] = NULL);
}

void teardownGc() {
  for (int i = 0; i < HeapSegments; i++) {
    ArenaHeader * arena = HEAP.free_arena[i];
    while (arena != NULL) {
      ArenaHeader * next = arena->next;
      freeArena(arena);
      arena = next;
    }
    arena = HEAP.full_arena[i];
    while (arena != NULL) {
      ArenaHeader * next = arena->next;
      freeArena(arena);
      arena = next;
    }
  }
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


