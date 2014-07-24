#ifndef H_GC
#define H_GC

#include <stdlib.h>
#include <stdint.h>

#ifdef DEBUG
#define assert(t) \
  if (!(t)) { \
    printf("assertion '%s' failed: %s:%d\n", #t, __FILE__, __LINE__); \
    __asm("int3"); \
    printf(" "); \
  }
#else
#define assert(t) ((void)0)
#endif

#ifdef DEBUG_PRINT
#define debug(str, ...) (printf(str, __VA_ARGS__))
#else
#define debug(str, ...) ((void)0)
#endif

typedef struct ObjectHeader ObjectHeader;
struct ObjectHeader {
  int            some_header_bits : 32;
  size_t         length;
  ObjectHeader * attrib;
};

extern const int arenaAlignment;
extern const int arenaAlignBits;
extern const int arenaAlignMask;

typedef struct FreeObject FreeObject;
struct FreeObject {
  FreeObject * next;
};

typedef struct ArenaHeader ArenaHeader;
struct ArenaHeader {
  ArenaHeader * next;
  size_t        object_size;
  unsigned char object_bits;
  unsigned int  num_objects;
  unsigned int  num_alloc;
  size_t        bytemap_size;
  unsigned int  arena_size;
  void *        first;
  void *        free;
  FreeObject *  free_list;
};

inline ObjectHeader ** getSlots(ObjectHeader * o) {
  return (ObjectHeader**)(o+1);
}

ObjectHeader * alloc(size_t length);

int getNumberOfMarkBits(ArenaHeader * arena);
void inspectArena(ArenaHeader * arena);
void printMemoryStatistics();

void gcMark(ObjectHeader * root);
void gcSweep();

void initGc();
void teardownGc();
#endif
