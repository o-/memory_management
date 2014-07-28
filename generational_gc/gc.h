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

#define GC_ZAP_POINTER ((ObjectHeader*)0xdeadbeef)

typedef struct ObjectHeader ObjectHeader;
struct ObjectHeader {
  unsigned int   some_header_bits : 31;
  unsigned int   old              : 1;
  size_t         length;
  ObjectHeader * attrib;
};

extern ObjectHeader * Nil;
extern ObjectHeader * Root;

typedef struct ArenaHeader ArenaHeader;

inline ObjectHeader ** getSlots(ObjectHeader * o) {
  return (ObjectHeader**)(o+1);
}

ObjectHeader * alloc(size_t length);

void writeBarrier(ObjectHeader * parent, ObjectHeader * child);

int getNumberOfMarkBits(ArenaHeader * arena);
void inspectArena(ArenaHeader * arena);
void printMemoryStatistics();

void gcMark(ObjectHeader * root);
void gcSweep();

void initGc();
void teardownGc();
#endif
