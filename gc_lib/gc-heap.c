#include "debugging.h"

#include "gc.h"
#include "gc-const.h"
#include "gc-heap.h"

int heapSegmentNodeSize(int segment) {
  assert(segment >= 0 && segment < NUM_FIXED_HEAP_SEGMENTS);
  return SMALLEST_SEGMENT_SIZE<<segment;
}

