#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "object.h"

#include "debugging.h"

#include "gc.h"
#include "gc-const.h"
#include "gc-declarations.h"
#include "gc-heap.h"
#include "gc-heuristic.h"
#include "gc-mark-stack.h"
#include "gc-memory.h"

ObjectHeader * Nil;
ObjectHeader * Root;

static int gcReportingEnabled = 0;

static HeapStruct Heap;

int objectSizeToLength(int size) {
  return (size - sizeof(ObjectHeader)) / SLOT_SIZE;
}

int objectLengthToSize(int length) {
  return (length * SLOT_SIZE) + sizeof(ObjectHeader);
}

int getObjectSize(ArenaHeader * arena) {
  return arena->object_size;
}

int getMaxObjectLength(ArenaHeader * arena) {
  return objectSizeToLength(arena->object_size);
}


void setGen(ObjectHeader * o, int gen) {
  if (o->old != gen) {
    o->old = gen;
  }
}

#ifdef DEBUG
static ObjectHeader * kGcZapPointer = ((ObjectHeader*)0xdeadbeef);
#endif

extern inline ObjectHeader ** getSlots(ObjectHeader * o);
extern inline ObjectHeader * getSlot(ObjectHeader * o, int i);
extern inline void setSlot(ObjectHeader * o, int i, ObjectHeader * c);

ObjectHeader * allocFromArena(ArenaHeader * arena) {
  ObjectHeader * o = NULL;
  if ((uintptr_t)arena->free < getArenaEnd(arena)) {
    // Bump pointer allocation in virgin space
    o = (ObjectHeader*)arena->free;
    arena->free = arena->free + getObjectSize(arena);
    arena->num_alloc++;
  }
  if (arena->free_list != NULL) {
    // Freelist allocation in recycled space
    o = (ObjectHeader*)arena->free_list;
    arena->free_list = arena->free_list->next;
    arena->num_alloc++;
  }
  return o;
}

ArenaHeader * newArena(int segment) {
  ArenaHeader * new_arena = allocateAlignedArena(segment);
  ArenaHeader * first     = Heap.free_arena[segment];
  new_arena->next = first;
  Heap.free_arena[segment] = new_arena;
  return new_arena;
}

ObjectHeader * allocFromSegment(int segment,
                                int length,
                                int new_arena) {
  if (segment >= NUM_FIXED_HEAP_SEGMENTS) {
    ArenaHeader * arena = allocateAlignedChunk(segment, length);

    ArenaHeader * first     = Heap.full_arena[segment];
    arena->next = first;
    Heap.full_arena[segment] = arena;

    return arena->first;
  }

  ArenaHeader * arena = Heap.free_arena[segment];
  while (1) {
    if (arena == NULL && new_arena == 1) {
      arena = newArena(segment);
      if (arena != NULL) {
        Heap.heap_size[segment]++;
      }
    }
    if (arena == NULL) return NULL;

    ObjectHeader * o = allocFromArena(arena);
    if (o != NULL) {
      assert(chunkFromPtr(o) == arena);
      assert(arena->object_size < 2 * objectLengthToSize(length));
      return o;
    }

    // This arena is full. Move to full_arena to not have to search it again
    ArenaHeader * next = arena->next;

    ArenaHeader * full = Heap.full_arena[segment];
    arena->next = full;
    Heap.full_arena[segment] = arena;

    Heap.free_arena[segment] = next;
    arena = next;
  }
}

void doGc(int full);
extern inline int getFixedSegmentForLength(int length);

ObjectHeader * alloc(size_t length) {
  assert(length >= 0);

  int segment = getFixedSegmentForLength(length);
  if (segment == -1) {
    segment = VARIABLE_LARGE_NODE_SEGMENT;
  }

  // Assert the next smaller segment is actually smaller
  assert(segment == 0 ||
         (segment < NUM_FIXED_HEAP_SEGMENTS ?
          heapSegmentNodeSize(segment-1) < objectLengthToSize(length) :
          MAX_FIXED_NODE_SIZE            < objectLengthToSize(length)));
  // Assert segment is big enough
  assert(segment >= NUM_FIXED_HEAP_SEGMENTS ||
         (heapSegmentNodeSize(segment) >= objectLengthToSize(length)));

  int grow = segment < NUM_FIXED_HEAP_SEGMENTS ?
    Heap.heap_size[segment] < Heap.heap_size_limit[segment] : 0;

  ObjectHeader * o = allocFromSegment(segment, length, grow);
  if (o == NULL) {
    if (isFullGcDue()) {
      doGc(1);
      fullGcDone();
    } else {
      doGc(0);
    }

    o = allocFromSegment(segment, length, 0);
    if (o == NULL && segment < NUM_FIXED_HEAP_SEGMENTS) {
      growHeap(&Heap, segment);
      o = allocFromSegment(segment, length, 1);
    }
  }

  if (o == NULL) return NULL;

  setGen(o, 0);
  o->length = length;

  ObjectHeader ** s = getSlots(o);

  if (length == 0) return o;

  // Initialize all slots to Nil (Duff device)
  uintptr_t end = (uintptr_t)(s + length);
  switch(length % 8) {
    do {
      case 0: *s++ = Nil; case 7: *s++ = Nil;
      case 6: *s++ = Nil; case 5: *s++ = Nil;
      case 4: *s++ = Nil; case 3: *s++ = Nil;
      case 2: *s++ = Nil; case 1: *s++ = Nil;
    } while((uintptr_t)s < end);
  }

  return o;
}


void gcMark(ObjectHeader * root) {
  *getMark(Nil)  = GREY_MARK;
  *getMark(root) = GREY_MARK;
  markStackPush(Nil);
  markStackPush(root);

  while(!markStackEmpty()) {
    ObjectHeader * cur = markStackPop();
    char * mark         = getMark(cur);
#ifdef DEBUG
    ObjectHeader ** child = getSlots(cur);
    assert(*mark != WHITE_MARK);
    if (*mark == BLACK_MARK) {
      for (int i = 0; i < cur->length; i++) {
        assert(*getMark(*child) != WHITE_MARK);
        child++;
      }
    }
#endif
    if (*mark != BLACK_MARK) {
      long length           = cur->length;
      ObjectHeader ** child = getSlots(cur);
      for (int i = 0; i < length; i++) {
        char * child_mark = getMark(*child);
        if (*child_mark == WHITE_MARK) {
          *child_mark = GREY_MARK;
          markStackPush(*child);
        }
        child++;
      }
      setGen(cur, 1);
      *mark = BLACK_MARK;
    }
  }

  resetMarkStack();
}

void writeBarrier(ObjectHeader * parent, ObjectHeader * child) {
  if (parent->old > child->old) {
    char * p_mark = getMark(parent);
    if (*p_mark != GREY_MARK) {
      *p_mark = GREY_MARK;
      markStackPush(parent);
    }
  }
}

void nextObject(ObjectHeader ** o, ArenaHeader * arena) {
  *o = (ObjectHeader*)(((char*)(*o)) + arena->object_size);
}

void sweepArena(ArenaHeader * arena) {
  ObjectHeader * finger = arena->first;
  arena->free_list = NULL;
  arena->num_alloc = 0;
  float bump_space = (float)((uintptr_t)getArenaEnd(arena) -
                             (uintptr_t)arena->free) /
                     (float)((uintptr_t)getArenaEnd(arena) -
                             (uintptr_t)arena->first);

  FreeObject * free_list = NULL;
  char * mark = getBytemap(arena);
  while ((uintptr_t)finger < (uintptr_t)arena->free) {
    assert(getMark(finger) == mark);
    assert((void*)mark < arena->first);
    assert((void*)mark >= (void*)(arena+1));
    assert(*mark != GREY_MARK);

    if (*mark == WHITE_MARK) {
#ifdef DEBUG
      // Zap slots
      ObjectHeader * o  = (ObjectHeader*)finger;
      ObjectHeader ** s = getSlots(o);
      for (int i = 0; i < o->length; i++) {
        s[i] = kGcZapPointer;
      }
#endif
      // Do not create freelist for an almost empty area.
      if (bump_space < 0.8) {
        FreeObject * f = (FreeObject*)finger;
        if (free_list == NULL) {
          free_list = f;
          arena->free_list = f;
        } else {
          free_list->next = f;
          free_list = f;
        }
      }
    } else {
      assert(*mark == BLACK_MARK);
      assert(finger->old == 1);
      arena->num_alloc++;
    }
    nextObject(&finger, arena);
    mark++;
  }
  if (free_list != NULL) {
    free_list->next = NULL;
  }
}

void removeArena(ArenaHeader * arena) {
  if (arena->segment < NUM_FIXED_HEAP_SEGMENTS) {
    Heap.heap_size[arena->segment]--;
  }
  freeArena(arena);
}

void gcSweep(int full_gc) {
  for (int i = 0; i < NUM_HEAP_SEGMENTS; i++) {
    if (i >= NUM_FIXED_HEAP_SEGMENTS && !checkReleaseVariableArenas()) {
      continue;
    }

    ArenaHeader * arena     = Heap.free_arena[i];
    ArenaHeader * last_free = NULL;
    while (arena != NULL) {
      sweepArena(arena);
      sweepingDone(arena);
      // Release empty arenas
      if (arena->num_alloc == 0) {
        if (last_free != NULL) {
          last_free->next = arena->next;
        } else {
          Heap.free_arena[i] = arena->next;
        }
        ArenaHeader * to_free = arena;
        arena = arena->next;
        removeArena(to_free);
      } else {
        last_free = arena;
        arena = arena->next;
      }
    }
    arena = Heap.full_arena[i];
    ArenaHeader * prev_full = NULL;
    while (arena != NULL) {
      if (full_gc || isSweepingCandidate(arena)) {
        sweepArena(arena);
        sweepingDone(arena);
        // Move arenas with empty space to the free_arena list
        if (!isArenaConsideredFull(arena)) {
          if (last_free != NULL) {
            last_free->next = arena;
          } else {
            Heap.free_arena[i] = arena;
          }
          last_free = arena;
          if (prev_full == NULL) {
            Heap.full_arena[i] = arena->next;
          } else {
            prev_full->next = arena->next;
          }
          ArenaHeader * next = arena->next;
          arena->next = NULL;
          arena = next;
          continue;
        }
      }
      prev_full = arena;
      arena = arena->next;
    }
    if (i < NUM_FIXED_HEAP_SEGMENTS) {
      tryShrinkHeap(&Heap, i);
    }
  }
}


void initGc() {
  assert(heapInitNumArena > 0);
  for (int i = 0; i < NUM_HEAP_SEGMENTS; i++) {
    Heap.free_arena[i]      = NULL;
    Heap.full_arena[i]      = NULL;
    if (i < NUM_FIXED_HEAP_SEGMENTS) {
      Heap.heap_size_limit[i] = heapInitNumArena;
      Heap.heap_size[i]       = 0;
    }
  }

  assert(1<<((int)log2(SMALLEST_SEGMENT_SIZE)) == SMALLEST_SEGMENT_SIZE);
  assert(SMALLEST_SEGMENT_SIZE >= sizeof(ObjectHeader));
  assert(SMALLEST_SEGMENT_SIZE <= (1<<(1+(int)log2(sizeof(ObjectHeader)))));

  buildGcSegmentSizeLookupTable();

  Nil = alloc(0);
  resetMarkStack();
}

unsigned int getDiff(struct timespec a, struct timespec b) {
  if (a.tv_sec == b.tv_sec) {
    return (b.tv_nsec - a.tv_nsec) / 1000000;
  } else {
    return (1000000000L - a.tv_nsec + b.tv_nsec +
            (1000000000L * (b.tv_sec - a.tv_sec - 1))) / 1000000;
  }
}

void verifyArena(ArenaHeader * arena) {
#ifdef VERIFY_HEAP
  ObjectHeader * o    = arena->first;
  char *         mark = getBytemap(arena);
  while((uintptr_t)o < getArenaEnd(arena) &&
        (uintptr_t)o < (uintptr_t)arena->free) {
    assert(getMark(o) == mark);
    if (*mark != WHITE_MARK) {
      assert(o->old == 1);
      assert(o->length >= 0);
      ObjectHeader ** children = getSlots(o);
      for (int i = 0; i < o->length; i++) {
        ObjectHeader * child = children[i];
        assert(child != kGcZapPointer);
        ArenaHeader * child_arena = chunkFromPtr(child);
        assert(child_arena->num_alloc > 0);
        if (*mark == BLACK_MARK) {
          assert(*getMark(child) != WHITE_MARK);
        }
        assert(child->length >= 0);
        child++;
      }
    }
    nextObject(&o, arena);
    mark++;
  }
#endif
}

void verifyHeap() {
  for (int i = 0; i < NUM_HEAP_SEGMENTS; i++) {
    ArenaHeader * arena = Heap.free_arena[i];
    while (arena != NULL) {
      verifyArena(arena);
      arena = arena->next;
    }
    arena = Heap.full_arena[i];
    while (arena != NULL) {
      verifyArena(arena);
      arena = arena->next;
    }
  }
}

static int lastFullGcTime     = 0;

void doGc(int full_gc) {
#ifdef DEBUG
  verifyHeap();
#endif

  if (full_gc) {
    resetMarkStack();

    for (int i = 0; i < NUM_HEAP_SEGMENTS; i++) {
      ArenaHeader * arena = Heap.free_arena[i];
      while (arena != NULL) {
        clearAllMarks(arena);
        arena = arena->next;
      }
      arena = Heap.full_arena[i];
      while (arena != NULL) {
        clearAllMarks(arena);
        arena = arena->next;
      }
    }
  }

  static struct timespec a, b, c;
  if (gcReportingEnabled) clock_gettime(CLOCK_REALTIME, &a);
  gcMark(Root);
  if (gcReportingEnabled) clock_gettime(CLOCK_REALTIME, &b);
  gcSweep(full_gc);
  if (gcReportingEnabled) clock_gettime(CLOCK_REALTIME, &c);

#ifdef DEBUG
  verifyHeap();
#endif

  if (gcReportingEnabled) {
#ifdef DEBUG
    int isStale = 3;
#else
    int isStale = 8;
#endif
    if (full_gc ||
        (lastFullGcTime != 0 && (getDiff(a, c) * isStale > lastFullGcTime))) {
      if (full_gc) {
        printf("* Full GC:\n");
        lastFullGcTime = getDiff(a, c);
      } else {
        printf("* Stale newspace collection:\n");
      }
      printf("marking took: %d ms\n", getDiff(a, b));
      printf("sweeping took: %d ms\n", getDiff(b, c));
      printMemoryStatistics();
    }
  }
}

void teardownGc() {
  doGc(1);
  for (int i = 0; i < NUM_HEAP_SEGMENTS; i++) {
    ArenaHeader * arena = Heap.free_arena[i];
    while (arena != NULL) {
      ArenaHeader * next = arena->next;
      removeArena(arena);
      arena = next;
    }
    arena = Heap.full_arena[i];
    while (arena != NULL) {
      ArenaHeader * next = arena->next;
      removeArena(arena);
      arena = next;
    }
    if (i < NUM_FIXED_HEAP_SEGMENTS) {
      assert(Heap.heap_size[i] == 0);
    }
  }
}

void inspectArena(ArenaHeader * arena) {
  printf("Arena : %p = {\n", arena);
  printf("  object_size  : %d\n", getObjectSize(arena));
  printf("  num_objects  : %d\n", getNumObjects(arena));
  printf("  mark_bits    : [");
  char * bm    = getBytemap(arena);
  int    count = 0;
  int    col   = 0;
  int    total = 0;
  for (int i = 0; i < getNumObjects(arena); i++) {
    if (bm[i] != 0) {
      count++;
    }
    if (i % 512 == 511) {
      printf("%4i", count);
      total += count;
      count = 0;
      col++;
    }
    if (col == 24) {
      printf("\n                  ");
      col = 0;
    }
  }
  total += count;
  printf("] (%d) \n}\n", total);
}

int getNumberOfMarkBits(ArenaHeader * arena) {
  char * bm    = getBytemap(arena);
  int    count = 0;
  for (int i = 0; i < getNumObjects(arena); i++) {
    if (bm[i] != 0) {
      count++;
    }
  }
  return count;
}

void printMemoryStatistics() {
  unsigned long space  = 0;
  unsigned long usable = 0;
  unsigned long used   = 0;
  for (int i = 0; i < NUM_HEAP_SEGMENTS; i++) {
    ArenaHeader * arena = Heap.free_arena[i];
    while (arena != NULL) {
      space  += arena->raw_size;
      usable += getNumObjects(arena) * getObjectSize(arena);
      used   += arena->num_alloc * getObjectSize(arena);
      arena   = arena->next;
    }
    arena = Heap.full_arena[i];
    while (arena != NULL) {
      space  += arena->raw_size;
      usable += getNumObjects(arena) * getObjectSize(arena);
      used   += arena->num_alloc * getObjectSize(arena);
      arena   = arena->next;
    }
  }
  printf("Reserverd: %lu mb | Usable: %lu mb | Used: %lu mb\n",
      space / 1048576, usable / 1048576, used / 1048576);
}


