#ifndef H_GC
#define H_GC

#include <stdlib.h>
#include <stdint.h>

#include "object.h"

extern ObjectHeader * Nil;
extern ObjectHeader * Root;

typedef struct ArenaHeader ArenaHeader;

ObjectHeader * alloc(size_t length);

int getNumberOfMarkBits(ArenaHeader * arena);
void inspectArena(ArenaHeader * arena);
void printMemoryStatistics();

void initGc();
void teardownGc();

void setSlot(ObjectHeader * o, int i, ObjectHeader * c);

#endif
