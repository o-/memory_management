#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <pthread.h>
#include <sched.h>

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

static int gcReportingEnabled = 1;

static HeapStruct Heap;

void fatalError(const char * msg) {
  puts(msg);
#ifdef DEBUG
  __asm("int3");
#endif
  exit(1);
}

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

#ifdef DEBUG
static ObjectHeader * kGcZapPointer = ((ObjectHeader*)0xdeadbeef);
#endif

extern inline ObjectHeader ** getSlots(ObjectHeader * o);
extern inline ObjectHeader * getSlot(ObjectHeader * o, int i);

void setSlot(ObjectHeader * o, int i, ObjectHeader * c) {
  getSlots(o)[i] = c;
  writeBarrier(o, c);
}

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
  if (new_arena == NULL) return NULL;

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
    if (arena == NULL) {
      return NULL;
    }

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

extern inline int getFixedSegmentForLength(int length);

void doGc();
void startFullGc();

int fullSweep = 0;
int skipGc    = 0;

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
    if (skipGc == 0) {
      doGc();
      if (isFullGcDue(&Heap, segment)) {
        startFullGc();
        skipGc = 1;
      }
    }

    o = allocFromSegment(segment, length, 0);
    if (o == NULL && segment < NUM_FIXED_HEAP_SEGMENTS) {
      skipGc = 0;
      growHeap(&Heap, segment);
      o = allocFromSegment(segment, length, 1);
    }
  }

  if (o == NULL) {
    // emergency gc
    startFullGc();
    doGc();
    o = allocFromSegment(segment, length, 1);
    if (o == NULL) {
      fatalError("Out of memory");
    }
  }

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

pthread_mutex_t mark_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mark_thread_mutex = PTHREAD_MUTEX_INITIALIZER;

void collectRoots() {
  *getMark(Nil)  = GREY_MARK;
  markStackPush(Nil);
  *getMark(Root) = GREY_MARK;
  markStackPush(Root);
}

static int markThreadPause = 0;
static int writeBarrierWaiting = 0;
void gcMark(int concurrent) {
  while(!markStackEmpty() && (!concurrent || markThreadPause == 0)) {
    ObjectHeader * cur = markStackPop();
    char * mark         = getMark(cur);
#ifdef DEBUG
    ObjectHeader ** child = getSlots(cur);
    assert(*mark != WHITE_MARK);
    if (concurrent) pthread_mutex_lock(&mark_mutex);
    if (*mark == BLACK_MARK) {
      for (int i = 0; i < cur->length; i++) {
        assert(*getMark(*child) != WHITE_MARK);
        child++;
      }
    }
    if (concurrent) pthread_mutex_unlock(&mark_mutex);
#endif
    if (*mark != BLACK_MARK) {
      if (concurrent) pthread_mutex_lock(&mark_mutex);
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
      *mark = BLACK_MARK;
      if (concurrent) {
        pthread_mutex_unlock(&mark_mutex);
        // Write barrier is blocked
        if (writeBarrierWaiting == 1) sched_yield();
      }
    }
  }
}

void * gcMarkThread(void * unused) {
  while (1) {
    if (markThreadPause == 1) {
      usleep(6);
    }
    if (markStackEmpty()) {
      usleep(0);
    }
    pthread_mutex_lock(&mark_thread_mutex);
    gcMark(1);
    skipGc = 0;
    pthread_mutex_unlock(&mark_thread_mutex);
  }
  return NULL;
}

extern inline void writeBarrier(ObjectHeader * parent, ObjectHeader * child);
void deferredWriteBarrier(ObjectHeader * parent, ObjectHeader * child,
                          char * p_mark, char * c_mark) {
  writeBarrierWaiting = 1;
  pthread_mutex_lock(&mark_mutex);
  if (*p_mark == BLACK_MARK && *c_mark == WHITE_MARK) {
    *p_mark = GREY_MARK;
    markStackPush(parent);
  }
  writeBarrierWaiting = 0;
  pthread_mutex_unlock(&mark_mutex);
}

void nextObject(ObjectHeader ** o, ArenaHeader * arena) {
  *o = (ObjectHeader*)(((char*)(*o)) + arena->object_size);
}

void sweepArena(ArenaHeader * arena) {
  arena->free_list = NULL;
  arena->num_alloc = 0;
  float bump_space = (float)((uintptr_t)getArenaEnd(arena) -
                             (uintptr_t)arena->free) /
                     (float)((uintptr_t)getArenaEnd(arena) -
                             (uintptr_t)arena->first);

  FreeObject * free_list    = NULL;
  char * mark               = getBytemap(arena);
  ObjectHeader * finger     = arena->first;
  ObjectHeader * last_black = finger;

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
      last_black = finger;
      arena->num_alloc++;
    }
    nextObject(&finger, arena);
    mark++;
  }
  if (free_list != NULL) {
    free_list->next = NULL;
  }
  nextObject(&last_black, arena);
  // If there happens to be an empty area at the end of the arena lets
  // reenable bump allocation for that part.
  if (arena->free > (void*)last_black) {
    arena->free = last_black;
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

#ifndef DISABLE_CONCURENT_MARKING
static pthread_t markThread;
#endif

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

#ifndef DISABLE_CONCURENT_MARKING
  if(pthread_create(&markThread, NULL, gcMarkThread, NULL)) {
    fatalError("Couldn't create mark thread");
  }
#endif
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

void startFullGc() {
  markThreadPause = 1;
  pthread_mutex_lock(&mark_thread_mutex);

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
  fullSweep = 1;
  resetMarkStack();
  collectRoots();

  markThreadPause = 0;
  pthread_mutex_unlock(&mark_thread_mutex);
}

void doGc() {
  markThreadPause = 1;
  pthread_mutex_lock(&mark_thread_mutex);

  static struct timespec a, b, c;
  if (gcReportingEnabled) clock_gettime(CLOCK_REALTIME, &a);
  collectRoots();
  gcMark(0);

#ifdef DEBUG
  verifyHeap();
#endif
  if (gcReportingEnabled) clock_gettime(CLOCK_REALTIME, &b);
  gcSweep(fullSweep);
  if (gcReportingEnabled) clock_gettime(CLOCK_REALTIME, &c);

#ifdef DEBUG
  verifyHeap();
#endif


  if (gcReportingEnabled) {
#ifndef DISABLE_CONCURENT_MARKING
#ifndef VERIFY_HEAP
    // With concurrent marking we should have few big marking pauses
    if (getDiff(a,b) > 2) {
      printf("marking took: %d ms\n", getDiff(a, b));
    }
#endif
#endif
    if (fullSweep) {
#ifdef DISABLE_CONCURENT_MARKING
      printf("full marking took: %d ms\n", getDiff(a, b));
#endif
      printf("full sweeping took: %d ms\n", getDiff(b, c));
      printMemoryStatistics();
    }
  }

  fullSweep = 0;
  resetMarkStack();
  collectRoots();

  markThreadPause = 0;
  pthread_mutex_unlock(&mark_thread_mutex);
}

void teardownGc() {
#ifndef DISABLE_CONCURENT_MARKING
  markThreadPause = 1;
  pthread_cancel(markThread);
  if(pthread_join(markThread, NULL)) {
    fatalError("markThread not stopped gracefully");
  }
#endif
  startFullGc();
  doGc();
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


