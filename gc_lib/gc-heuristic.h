#ifndef H_GC_HEURISTIC
#define H_GC_HEURISTIC

#include "gc-heap.h"

extern const int heapInitNumArena;

int  isFullGcDue();
void fullGcDone();
void growHeap(HeapStruct * heap, int segment);
void tryShrinkHeap(HeapStruct * heap, int segment);
int  checkReleaseVariableArenas();
int  isArenaConsideredFull(ArenaHeader * arena);

#endif
