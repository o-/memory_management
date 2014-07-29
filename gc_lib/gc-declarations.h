#ifndef H_GC_DECLARATIONS
#define H_GC_DECLARATIONS

#include "gc.h"

int objectLengthToSize(int length);
int heapSegmentNodeSize(int segment);
int getNumObjects(ArenaHeader * arena);

typedef struct FreeObject FreeObject;

struct ArenaHeader {
  unsigned char segment;
  unsigned char object_bits;
  void *        first;
  unsigned int  num_alloc;
  size_t        object_size;
  unsigned int  num_objects;
  void *        free;
  FreeObject *  free_list;
  ArenaHeader * next;
  int           was_full;
  size_t        raw_size;
  void *        raw_base;
};

struct FreeObject {
  FreeObject * next;
};

#endif
