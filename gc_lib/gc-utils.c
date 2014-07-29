#include "gc-utils.h"
#include "gc-const.h"
#include "gc-declarations.h"
#include "debugging.h"

#define __GC_SEGMENT_SIZE_LOOKUP_TABLE_SIZE \
  (LARGEST_FIXED_SEGMENT_SIZE/SLOT_SIZE)
static int __gcSegmentSizeLookupTable[__GC_SEGMENT_SIZE_LOOKUP_TABLE_SIZE];

void buildGcSegmentSizeLookupTable() {
  // Build the lookup tables
  int segment = 0;
  for (int i = 0; i < __GC_SEGMENT_SIZE_LOOKUP_TABLE_SIZE; i++) {
    int size = objectLengthToSize(i);
    if (size <= MAX_FIXED_NODE_SIZE) {
      if (heapSegmentNodeSize(segment) < size) {
        segment++;
      }
      assert(segment == 0 || heapSegmentNodeSize(segment-1) < size);
      assert(heapSegmentNodeSize(segment) >= size);
      assert(segment < NUM_FIXED_HEAP_SEGMENTS);
    } else {
      segment  = VARIABLE_LARGE_NODE_SEGMENT;
      assert(size > MAX_FIXED_NODE_SIZE);
    }
    __gcSegmentSizeLookupTable[i] = segment;
  }
}

int getFixedSegmentForLength(int length) {
  if (length >= __GC_SEGMENT_SIZE_LOOKUP_TABLE_SIZE) return -1;
  return __gcSegmentSizeLookupTable[length];
}
