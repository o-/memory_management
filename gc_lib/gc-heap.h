#ifndef H_GC_HEAP
#define H_GC_HEAP

#include "gc-const.h"

int heapSegmentNodeSize(int segment);
int objectLengthToSize(int length);

typedef struct HeapStruct HeapStruct;
struct HeapStruct {
  ArenaHeader * free_arena[NUM_HEAP_SEGMENTS];
  ArenaHeader * full_arena[NUM_HEAP_SEGMENTS];
  unsigned int heap_size_limit[NUM_FIXED_HEAP_SEGMENTS];
  unsigned int heap_size[NUM_FIXED_HEAP_SEGMENTS];
  unsigned int heap_size_change[NUM_FIXED_HEAP_SEGMENTS];
};

#endif
