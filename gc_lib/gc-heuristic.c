#include "gc.h"
#include "gc-heuristic.h"
#include "gc-const.h"
#include "gc-declarations.h"

#include "debugging.h"

/* Variables to adjust GC heuristics */
const float heapGrowFactor                = 1.4;
const float heapShrinkFactor              = 1.2;

const int   releaseVariableArenasInterval = 2;

const float arenaFullPercentage           = 0.95;

const int fullGcInterval = 4;
/* end */

// Initial Values
const int heapInitNumArena = 4;

static int  releaseVariableArenas = 5;

static int fullGcDue = 10;

int isFullGcDue(HeapStruct * heap, int segment) {
  if (fullGcDue-- < 0) {
    fullGcDue = fullGcInterval;
  }
  return fullGcDue == 0;
}

void growHeap(HeapStruct * heap, int segment) {
  fullGcDue--;
  int grow = heap->heap_size_limit[segment] * heapGrowFactor + 1;
  debug("segment %d : raising limit from %d to %d arenas. used: %d\n",
      segment, heap->heap_size_limit[segment], grow, heap->heap_size[segment]);
  heap->heap_size_limit[segment] = grow;
}

void tryShrinkHeap(HeapStruct * heap, int segment) {
  int shrink = heap->heap_size[segment] * heapShrinkFactor + 1;
  if (heap->heap_size_limit[segment] > shrink && shrink >= heapInitNumArena) {
    debug("segment %d : shrinking limit from %d to %d arenas. used: %d\n",
      segment, heap->heap_size_limit[segment],
      shrink, heap->heap_size[segment]);
    fullGcDue--;
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

