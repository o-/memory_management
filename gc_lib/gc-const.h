#ifndef H_CG_CONST
#define H_CG_CONST

#define SLOT_SIZE sizeof(void*)

#define NUM_FIXED_HEAP_SEGMENTS 8
#define NUM_VARIABLE_HEAP_SEGMENTS 1
#define NUM_HEAP_SEGMENTS (NUM_FIXED_HEAP_SEGMENTS + NUM_VARIABLE_HEAP_SEGMENTS)
#define VARIABLE_LARGE_NODE_SEGMENT NUM_FIXED_HEAP_SEGMENTS

#define SMALLEST_SEGMENT_SIZE 32
#define LARGEST_FIXED_SEGMENT_SIZE \
  (SMALLEST_SEGMENT_SIZE<<(NUM_FIXED_HEAP_SEGMENTS-1))

#define MAX_FIXED_NODE_SIZE (SMALLEST_SEGMENT_SIZE<<(NUM_FIXED_HEAP_SEGMENTS-1))

#define WHITE_MARK ((char)0)
#define GREY_MARK  ((char)1)
#define BLACK_MARK ((char)2)

#endif
