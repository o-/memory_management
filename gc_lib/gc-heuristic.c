#include "gc.h"
#include "gc-heuristic.h"
#include "gc-const.h"
#include "gc-declarations.h"

#include "debugging.h"

/* Variables to adjust GC heuristics */
const float heapGrowFactor                = 1.4;
const float heapShrinkFactor              = 1.2;

const int   fullGcInterval                = 5;

const int   releaseVariableArenasInterval = 2;

const float arenaFullPercentage           = 0.95;
/* end */

// Initial Values
const int heapInitNumArena = 4;

static int  doFullGc              = 5;
static int  releaseVariableArenas = 5;

int isFullGcDue(HeapStruct * heap, int segment) {
  return ((float)heap->heap_size[segment] /
          (float)heap->heap_size_limit[segment]) > 0.90;
}

void growHeap(HeapStruct * heap, int segment) {
  if (doFullGc > 0) doFullGc--;
  int grow = heap->heap_size_limit[segment] * heapGrowFactor + 1;
  debug("segment %d : raising limit from %d to %d arenas. currently used: %d\n",
      segment, heap->heap_size_limit[segment], grow, heap->heap_size[segment]);
  heap->heap_size_limit[segment] = grow;
}

void tryShrinkHeap(HeapStruct * heap, int segment) {
  int shrink = heap->heap_size[segment] * heapShrinkFactor + 1;
  if (heap->heap_size_limit[segment] > shrink && shrink >= heapInitNumArena) {
    if (doFullGc > 0) doFullGc--;
    heap->heap_size_limit[segment] = shrink;
  }
}

int checkReleaseVariableArenas() {
  if(releaseVariableArenas > 0) {
    releaseVariableArenas--;
    return 0;
  }
  releaseVariableArenas = releaseVariableArenasInterval;
  return 1;
}

int  isArenaConsideredFull(ArenaHeader * arena) {
  float population = (float)arena->num_alloc /
                     (float)getNumObjects(arena);
  return population >= arenaFullPercentage;
}

int  isSweepingCandidate(ArenaHeader * arena) {
  return arena->segment >= NUM_FIXED_HEAP_SEGMENTS ||
         !arena->was_full;
}

void sweepingDone(ArenaHeader * arena){
  if (isArenaConsideredFull(arena)) {
    arena->was_full = 1;
  }
}

