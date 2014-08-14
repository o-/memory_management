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
  unsigned char object_bits;
  unsigned int  first_offset;
  unsigned char segment;
  unsigned char gc_class;
  size_t        object_size;
  unsigned int  num_objects;
  unsigned int  num_alloc;
  void *        free;
  StackChunk *  free_list;
  char          was_full;
  ArenaHeader * next;
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

void gcMark();

int gcCurrentClass();

void gcForceRun();
void gcForward(ObjectHeader * object);

void gcEnableReporting(int i);

/* Inlined access functions */

#define WHITE_MARK ((char)0)
#define GREY_MARK  ((char)1)
#define BLACK_MARK ((char)2)

#define GC_ARENA_ALIGN_BITS 22
#define GC_ARENA_ALIGNMENT  (1<<GC_ARENA_ALIGN_BITS)
#define GC_ARENA_SIZE       GC_ARENA_ALIGNMENT
#define GC_ARENA_ALIGN_MASK (GC_ARENA_ALIGNMENT-1)

inline char * getBytemap(ArenaHeader * base) {
  return (char*)(base + 1);
}

inline ArenaHeader * chunkFromPtr(void * base) {
  base = (ArenaHeader*)(((uintptr_t)base & ~GC_ARENA_ALIGN_MASK));
  return (ArenaHeader*) ((uintptr_t)base);
}

inline int getObjectBits(ArenaHeader * arena) {
  return arena->object_bits;
}

inline uintptr_t getArenaFirst(ArenaHeader * arena) {
  return (uintptr_t)(getBytemap(arena) + arena->first_offset);
}

inline int getBytemapIndex(void * base, ArenaHeader * arena) {
  return (((uintptr_t)base - getArenaFirst(arena)) >> getObjectBits(arena));
}

inline char * getMark(void * ptr) {
  ArenaHeader * arena = chunkFromPtr(ptr);
  char *        bm    = getBytemap(arena);
  int           idx   = getBytemapIndex(ptr, arena);
  return bm + idx;
}

inline void gcWriteBarrier(ObjectHeader * parent, ObjectHeader * child) {
  if (*getMark(parent) == BLACK_MARK && *getMark(child) == WHITE_MARK) {
    gcForward(parent);
  }
}

#endif
