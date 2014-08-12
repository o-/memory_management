#ifndef H_GC
#define H_GC

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "debugging.h"
#include "object.h"

// CHANGEME :
typedef TestObject ObjectHeader;

/* Fwd-Declarations */

typedef struct HeapStruct HeapStruct;
typedef struct StackChunk StackChunk;
typedef struct ArenaHeader ArenaHeader;

void fatalError(const char * msg);

/* structs */

struct ArenaHeader {
  unsigned char segment;
  unsigned char gc_class;
  void *        first;
  unsigned int  num_alloc;
  size_t        object_size;
  unsigned int  num_objects;
  void *        free;
  StackChunk *  free_list;
  ArenaHeader * next;
  int           was_full;
  size_t        raw_size;
  void *        raw_base;
};


/* API */

ObjectHeader * gcAlloc(size_t length, int class);

int getNumberOfMarkBits(ArenaHeader * arena);
void inspectArena(ArenaHeader * arena);
void printMemoryStatistics();

void gcInit();
void gcTeardown();

// IMPLEMENT ME :
void gcMarkWrapper();

void gcForward(ObjectHeader * object);
void gcMark();

int gcCurrentClass();

void gcForceRun();

void gcEnableReporting(int i);

/* Inlined access functions */

#define WHITE_MARK ((char)0)
#define GREY_MARK  ((char)1)
#define BLACK_MARK ((char)2)

#define GC_ARENA_ALIGN_BITS 20
#define GC_ARENA_ALIGNMENT  (1<<GC_ARENA_ALIGN_BITS)
#define GC_ARENA_SIZE       GC_ARENA_ALIGNMENT
#define GC_ARENA_ALIGN_MASK (GC_ARENA_ALIGNMENT-1)

#define GC_ARENA_START_ALIGN 16

#define GC_ARENA_BITS_PER_MARK 5

#define GC_ARENA_MAX_OFFSET  0x800
#define GC_ARENA_OFFSET_MASK 0x7f0

#define GC_ARENA_HEADER_SIZE \
  (sizeof(ArenaHeader) + GC_ARENA_MAX_OFFSET + GC_ARENA_START_ALIGN)

#define GC_ARENA_USABLE_SIZE \
  ((int)((GC_ARENA_SIZE - GC_ARENA_HEADER_SIZE) / \
         (1.0 + 1.0 / (double)(1 << GC_ARENA_BITS_PER_MARK))))

#define GC_ARENA_BYTEMAP_SIZE (GC_ARENA_USABLE_SIZE >> GC_ARENA_BITS_PER_MARK)

/*
 * Heap Constants
 *
 */

#define NUM_FIXED_HEAP_SEGMENTS 12
#define NUM_VARIABLE_HEAP_SEGMENTS 1
#define NUM_HEAP_SEGMENTS (NUM_FIXED_HEAP_SEGMENTS + NUM_VARIABLE_HEAP_SEGMENTS)
#define VARIABLE_LARGE_NODE_SEGMENT NUM_FIXED_HEAP_SEGMENTS

#define NUM_CLASSES 2

#define SMALLEST_SEGMENT_SIZE 32
#define LARGEST_FIXED_SEGMENT_SIZE \
  (SMALLEST_SEGMENT_SIZE<<(NUM_FIXED_HEAP_SEGMENTS-1))

#define MAX_FIXED_NODE_SIZE (SMALLEST_SEGMENT_SIZE<<(NUM_FIXED_HEAP_SEGMENTS-1))


// Use the least significant non-zero bits (mod sizeof(void*)) of the aligned
// base address as an offset for the beginning of the arena to improve caching.
inline unsigned int arenaHeaderOffset(void * base) {
  unsigned int offset = (((uintptr_t)base >> GC_ARENA_ALIGN_BITS) &
                        GC_ARENA_OFFSET_MASK);
  assert(GC_ARENA_MAX_OFFSET > GC_ARENA_OFFSET_MASK);
  assert(offset >= 0 && offset < GC_ARENA_MAX_OFFSET);
  assert(offset % sizeof(void*) == 0);
  return offset;
}

inline char * getBytemap(ArenaHeader * base) {
  return (char*)(base + 1);
}

inline ArenaHeader * chunkFromPtr(void * base) {
  base = (ArenaHeader*)(((uintptr_t)base & ~GC_ARENA_ALIGN_MASK));
  return (ArenaHeader*) ((uintptr_t)base + arenaHeaderOffset(base));
}

inline uintptr_t getFixedArenaStart(ArenaHeader * arena) {
  return (uintptr_t)&getBytemap(arena)[GC_ARENA_BYTEMAP_SIZE];
}

inline int getFixedBytemapIndex(void * base, ArenaHeader * arena) {
  return ((uintptr_t)base - getFixedArenaStart(arena)) >>
           GC_ARENA_BITS_PER_MARK;
}

inline char * getMark(void * ptr) {
  ArenaHeader * arena = chunkFromPtr(ptr);
  char *        bm    = getBytemap(arena);
  int           idx   = getFixedBytemapIndex(ptr, arena);
  // Variable sized arenas have a bytemapsize of 1, this we get a negative
  // index, since the base pointer of the one object in the page is where the
  // bytemap would be in a fixed size arena.
  assert((arena->segment < NUM_FIXED_HEAP_SEGMENTS &&
          idx >= 0 && idx < GC_ARENA_BYTEMAP_SIZE) ||
         (arena->segment >= NUM_FIXED_HEAP_SEGMENTS && idx < 0));
  if (idx < 0) {
    return bm;
  }
  assert((uintptr_t)arena->first > (uintptr_t)bm + idx);
  return bm + idx;
}

void deferredWriteBarrier(ObjectHeader * parent,
                          ObjectHeader * child,
                          char * p_mark);
inline void gcWriteBarrier(ObjectHeader * parent, ObjectHeader * child) {
  char * p_mark = getMark(parent);
  if (*p_mark == BLACK_MARK) {
    deferredWriteBarrier(parent, child, p_mark);
  }
}

#endif
