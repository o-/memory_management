#ifndef H_GC
#define H_GC

#include <stdlib.h>
#include <stdint.h>

#include "object.h"

#define GC_ZAP_POINTER ((ObjectHeader*)0xdeadbeef)

extern ObjectHeader * Nil;
extern ObjectHeader * Root;

typedef struct ArenaHeader ArenaHeader;

ObjectHeader * alloc(size_t length);

void writeBarrier(ObjectHeader * parent, ObjectHeader * child);

int getNumberOfMarkBits(ArenaHeader * arena);
void inspectArena(ArenaHeader * arena);
void printMemoryStatistics();

void initGc();
void teardownGc();
#endif
