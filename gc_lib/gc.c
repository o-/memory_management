#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <sys/mman.h>

#include "gc.h"

#include "debugging.h"
#include "stack.h"

#define USE_POSIX_MEMALIGN

/*
 * Heap Constants
 *
 */

#define NUM_FIXED_HEAP_SEGMENTS 10
#define NUM_VARIABLE_HEAP_SEGMENTS 1
#define NUM_HEAP_SEGMENTS (NUM_FIXED_HEAP_SEGMENTS + NUM_VARIABLE_HEAP_SEGMENTS)
#define VARIABLE_LARGE_NODE_SEGMENT NUM_FIXED_HEAP_SEGMENTS

#define NUM_CLASSES 2

#define SMALLEST_SEGMENT_SIZE 32
#define LARGEST_FIXED_SEGMENT_SIZE \
  (SMALLEST_SEGMENT_SIZE<<(NUM_FIXED_HEAP_SEGMENTS-1))

#define MAX_FIXED_NODE_SIZE (SMALLEST_SEGMENT_SIZE<<(NUM_FIXED_HEAP_SEGMENTS-1))


/*
 * Structs
 *
 */

struct FreeObject {
  FreeObject * next;
  char         was_zapped;
};

struct HeapStruct {
  ArenaHeader * free_arena;
  ArenaHeader * full_arena;
  unsigned int size_limit;
  unsigned int size;
};


/*
 * Globals
 *
 */

static int gcReportingEnabled = 0;

static HeapStruct   Heap[NUM_CLASSES][NUM_HEAP_SEGMENTS];


/*
 * Heuristic Constants
 *
 */

const float heapGrowFactor                = 1.4;
const float heapShrinkFactor              = 1.2;

#define FULL_GC_INTERVAL 5
#define RELEASE_VARIABLE_ARENAS_INTERVAL 1

const float arenaFullPercentage           = 0.95;

const int heapInitNumArena = 1;

const int gcClassRelation = 10;

/*
 * Mark Stack
 *
 */

#define __GC_STACK_SPACE_SIZE (1<<21)
typedef struct GcStackSpaceStruct {
  int alloc;
  char * free;
  char space[__GC_STACK_SPACE_SIZE];
} GcStackSpaceStruct;
static GcStackSpaceStruct GcStackSpace;


void * markStackSpaceMalloc(size_t size) {
  void * p = GcStackSpace.free;
  GcStackSpace.alloc += size;
  if (GcStackSpace.alloc > __GC_STACK_SPACE_SIZE) {
    fatalError("GC panic: no temp mem available");
  }
  GcStackSpace.free += size;
  return p;
}

void markStackSpaceClear() {
  GcStackSpace.free  = GcStackSpace.space;
  GcStackSpace.alloc = 0;
}

static StackChunk * MarkStack;

void markStackPush(ObjectHeader * o) {
  stackPush(&MarkStack, o, &markStackSpaceMalloc);
}

ObjectHeader * markStackPop() {
  return stackPop(&MarkStack);
}

int markStackEmpty() {
  return stackEmpty(MarkStack);
}

void resetMarkStack() {
  markStackSpaceClear();
  MarkStack = allocStackChunk(&markStackSpaceMalloc);
}


/*
 * Arenas and memory access (helpers)
 *
 */

const int arenaStartAlign = 16;

extern inline char * getBytemap(ArenaHeader * base);
extern inline ArenaHeader * chunkFromPtr(void * base);
extern inline int getObjectBits(ArenaHeader * arena);
extern inline int getBytemapIndex(void * base, ArenaHeader * arena);
extern inline unsigned int arenaHeaderOffset(void * base);

extern inline char * getMark(void * ptr);

void nextObject(ObjectHeader ** o, ArenaHeader * arena) {
  *o = (ObjectHeader*)(((char*)(*o)) + arena->object_size);
}

int getObjectSize(ArenaHeader * arena) {
  return arena->object_size;
}

int getNumObjects(ArenaHeader * arena) {
  return arena->num_objects;
}

uintptr_t getArenaEnd(ArenaHeader * arena) {
  return (uintptr_t)arena->first + (getObjectSize(arena)*getNumObjects(arena));
}

int isAligned(void * base, unsigned int align) {
  return (uintptr_t)base % align == 0;
}

int isMaskable(void * ptr, ArenaHeader * chunk) {
  return (uintptr_t)ptr < (uintptr_t)chunk+GC_ARENA_ALIGNMENT;
}

uintptr_t nextAlignedAddress(uintptr_t base) {
  if ((base & ~GC_ARENA_ALIGN_MASK) == base) return base;
  return (base+GC_ARENA_ALIGNMENT) & ~GC_ARENA_ALIGN_MASK;
}

size_t roundUpMemory(size_t required, unsigned int align) {
  if (required % align == 0) return required;
  size_t diff = (required + align) % align;
  return required + align - diff;
}

int heapSegmentNodeSize(int segment) {
  assert(segment >= 0 && segment < NUM_FIXED_HEAP_SEGMENTS);
  return SMALLEST_SEGMENT_SIZE<<segment;
}

void clearAllMarks(ArenaHeader * arena) {
  memset(getBytemap(arena), 0, arena->num_objects);
  arena->was_full = 0;
}

size_t calcNumOfObjects(ArenaHeader * arena, int total_size, int object_size) {
  assert(total_size > 1024);

  // Area:
  // arena_offset | AreaHeader | bytemap | start_align | object_space | padding
  //
  // sizeof(bytemap)      = num_objects
  // sizeof(object_space) = num_objects * object_size

  int header = sizeof(ArenaHeader) + arenaHeaderOffset(arena) + arenaStartAlign;

  int num_objects = (total_size - header) / (1 + object_size);

  assert(num_objects > 0);
  assert(total_size >= header + num_objects +
                       (num_objects * object_size));

  // Ensure The bytemaps have a word aligned size
  num_objects &= ~(sizeof(void*) - 1);

  return (size_t)num_objects;
}

int __gcSegmentSizeLookupTable[LARGEST_FIXED_SEGMENT_SIZE+1];

int getFixedSegmentForSize(long length) {
  if (length > LARGEST_FIXED_SEGMENT_SIZE) return -1;
  return __gcSegmentSizeLookupTable[length];
}

void buildGcSegmentSizeLookupTable() {
  // Build the lookup tables
  int segment = 0;
  for (int size = 0; size <= LARGEST_FIXED_SEGMENT_SIZE; size++) {
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
    __gcSegmentSizeLookupTable[size] = segment;
  }
}


/*
 * Arenas (creation)
 *
 */

#ifndef USE_POSIX_MEMALIGN
static uintptr_t hint = 1;
#endif

uint32_t hash(uint32_t a) {
  a = (a+0x7ed55d16) + (a<<12);
  a = (a^0xc761c23c) ^ (a>>19);
  a = (a+0x165667b1) + (a<<5);
  a = (a+0xd3a2646c) ^ (a<<9);
  a = (a+0xfd7046c5) + (a<<3);
  a = (a^0xb55a4f09) ^ (a>>16);
  return a;
}

ArenaHeader * allocateAligned(size_t variable_length) {
  assert(1<<GC_ARENA_ALIGN_BITS == GC_ARENA_ALIGNMENT);
  assert(GC_ARENA_ALIGNMENT % sysconf(_SC_PAGESIZE) == 0);
  assert(GC_ARENA_SIZE      % sysconf(_SC_PAGESIZE) == 0);
  assert(GC_ARENA_SIZE <= GC_ARENA_ALIGNMENT);

  int OSPageAlignment = sysconf(_SC_PAGESIZE);

  size_t aligned_length = roundUpMemory(variable_length, OSPageAlignment);

  void * commited = NULL;

#ifdef USE_POSIX_MEMALIGN
  posix_memalign(&commited, GC_ARENA_ALIGNMENT, aligned_length);
  if(commited == NULL) return NULL;

#else

  // Length aligned to OS page size (required for mmap)
  assert(aligned_length % OSPageAlignment == 0);
  // Virtual memory needed to satisfy chunk alignment
  size_t request_length = roundUpMemory(aligned_length + GC_ARENA_ALIGNMENT,
                                        OSPageAlignment);

  hint = ((uintptr_t)hash(hint)) << 14;

  // Reserve virtual memory
  void * reserved = mmap((void*)hint,
                         request_length,
                         PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                         -1,
                         0);
  if (reserved == NULL) {
    return NULL;
  }

  // Determine next chunk aligned base address
  uintptr_t base          = (uintptr_t)reserved;
  uintptr_t aligned_base  = nextAlignedAddress(base);
  void * aligned_base_ptr = (void*)aligned_base;
  assert(aligned_base >= base);

  // Chop off unused pages before and after the aligned block
  long prefix = (size_t)aligned_base - base;
  assert(prefix >= 0);
  if (prefix > 0) {
    munmap((void*)base, prefix);
  }

  long suffix = (base + request_length) - (aligned_base + aligned_length);
  assert(suffix >= 0);
  if (suffix > 0) {
    munmap((void*)(aligned_base + aligned_length), suffix);
  }

  assert(request_length = prefix + aligned_length + suffix);

  // Reserve memory for the aligned block
  commited = mmap(aligned_base_ptr,
                  aligned_length,
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                  -1,
                  0);
  if (commited != aligned_base_ptr) {
    munmap(aligned_base_ptr, aligned_length);
    return NULL;
  }

  assert(commited == aligned_base_ptr);
#endif

  ArenaHeader * arena = (ArenaHeader*) ((uintptr_t)commited +
                                        arenaHeaderOffset(commited));

  arena->raw_size = aligned_length;
  arena->raw_base = commited;
  return arena;
}

ArenaHeader * allocateAlignedArena(int class, int segment) {
  ArenaHeader * chunk = NULL;
  assert(segment < NUM_FIXED_HEAP_SEGMENTS);

  chunk = allocateAligned(GC_ARENA_SIZE);
  if (chunk == NULL) return NULL;

  chunk->segment           = segment;
  chunk->gc_class          = class;
  int num_objects          = calcNumOfObjects(chunk,
                                              GC_ARENA_SIZE,
                                              heapSegmentNodeSize(segment));
  chunk->num_objects       = num_objects;

  chunk->first             = &getBytemap(chunk)[num_objects];

  uintptr_t f = (uintptr_t)chunk->first;
  if (f % arenaStartAlign != 0) {
    chunk->first = (void*)(f + f % arenaStartAlign);
  }

  chunk->free              = chunk->first;
  chunk->free_list         = NULL;
  chunk->num_alloc         = 0;
  chunk->object_size       = heapSegmentNodeSize(segment);
  chunk->object_bits       = log2(heapSegmentNodeSize(segment));

  assert(f + (chunk->num_objects * chunk->object_size) <=
         (uintptr_t)chunk->raw_base + chunk->raw_size);

  clearAllMarks(chunk);

  return chunk;
}

ArenaHeader * allocateAlignedChunk(int class, int segment, size_t object_size) {
  assert(segment >= NUM_FIXED_HEAP_SEGMENTS);

  size_t header = sizeof(ArenaHeader) + GC_ARENA_MAX_BYTEMAP_OFFSET +
                  arenaStartAlign;

  ArenaHeader * arena = allocateAligned(object_size + header);
  if (arena == NULL) return NULL;

  arena->num_alloc         = 1;
  arena->segment           = segment;
  arena->gc_class          = class;
  arena->num_objects       = 1;
  arena->object_size       = object_size;
  arena->object_bits       = GC_ARENA_ALIGN_BITS;
  arena->free_list         = NULL;
  arena->first             = &getBytemap(arena)[1];
  uintptr_t f = (uintptr_t)arena->first;
  if (f % arenaStartAlign != 0) {
    arena->first = (void*)(f + f % arenaStartAlign);
  }
  arena->free              = (void*)getArenaEnd(arena);

  clearAllMarks(arena);

  return arena;
}

ArenaHeader * newArena(int class, int segment) {
  ArenaHeader * new_arena = allocateAlignedArena(class, segment);
  if (new_arena == NULL) return NULL;

  ArenaHeader * first     = Heap[class][segment].free_arena;
  new_arena->next = first;
  Heap[class][segment].free_arena = new_arena;
  return new_arena;
}

void freeArena(ArenaHeader * arena) {
#ifdef USE_POSIX_MEMALIGN
  free(arena->raw_base);
#else
  munmap(arena->raw_base, arena->raw_size);
#endif
}


/*
 * Heuristics
 *
 */

static int  doFullGc              = FULL_GC_INTERVAL;
static int  releaseVariableArenas = RELEASE_VARIABLE_ARENAS_INTERVAL;

int isFullGcDue() {
  return doFullGc <= 0;
}

void fullGcDone() {
  doFullGc = FULL_GC_INTERVAL;
}

void growHeap(int class, int segment) {
  if (doFullGc > 0) doFullGc--;
  int grow = Heap[class][segment].size_limit * heapGrowFactor + 1;
  debug("class %d, segment %d : raising limit from %d to %d arenas. currently used: %d\n",
      class, segment, Heap[class][segment].size_limit,
      grow, Heap[class][segment].size);
  Heap[class][segment].size_limit = grow;
}

void tryShrinkHeap(int class, int segment) {
  int shrink = Heap[class][segment].size * heapShrinkFactor + 1;
  if (Heap[class][segment].size_limit > shrink && shrink >= heapInitNumArena) {
    if (doFullGc > 0) doFullGc--;
    Heap[class][segment].size_limit = shrink;
  }
}

int checkReleaseVariableArenas() {
  if(releaseVariableArenas > 0) {
    releaseVariableArenas--;
    return 0;
  }
  releaseVariableArenas = RELEASE_VARIABLE_ARENAS_INTERVAL;
  return 1;
}

int  isArenaConsideredFull(ArenaHeader * arena) {
  float population = (float)arena->num_alloc /
                     (float)getNumObjects(arena);
  return population >= arenaFullPercentage;
}

int  isSweepingCandidate(ArenaHeader * arena) {
  return arena->segment >= NUM_FIXED_HEAP_SEGMENTS ||
         !arena->was_full;
}

void sweepingDone(ArenaHeader * arena){
  if (isArenaConsideredFull(arena)) {
    arena->was_full = 1;
  }
}

static int currentGcClass = 1;
static int gcsSincMaxClass = 1;

int gcCurrentClass() {
  return currentGcClass;
}

void updateMaxClass() {
  if (currentGcClass == NUM_CLASSES) {
    gcsSincMaxClass = 1;
  } else {
    gcsSincMaxClass++;
  }

  currentGcClass = 1;
  for (int i = 1; i < NUM_CLASSES; i++) {
    if (gcsSincMaxClass % (i * gcClassRelation) == 0) {
      currentGcClass++;
    }
  }
}


/*
 * Object access
 *
 */

extern inline void gcWriteBarrier(ObjectHeader * parent, ObjectHeader * child);

void deferredWriteBarrier(ObjectHeader * parent,
                          ObjectHeader * child,
                          char * p_mark) {
  char * c_mark = getMark(child);
  if (*c_mark == WHITE_MARK) {
    *p_mark = GREY_MARK;
    markStackPush(parent);
  }
}


/*
 * Allocation
 *
 */

void verifyHeap();
void doGc(int full);

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

ObjectHeader * allocFromSegment(int class,
                                int segment,
                                size_t size,
                                int new_arena) {
  if (segment >= NUM_FIXED_HEAP_SEGMENTS) {
    ArenaHeader * arena = allocateAlignedChunk(class, segment, size);
    if (arena == NULL) {
      return NULL;
    }

    ArenaHeader * first = Heap[class][segment].full_arena;
    arena->next = first;
    Heap[class][segment].full_arena = arena;

    return arena->first;
  }

  ArenaHeader * arena = Heap[class][segment].free_arena;
  while (1) {
    if (arena == NULL && new_arena == 1) {
      arena = newArena(class, segment);
      if (arena != NULL) {
        Heap[class][segment].size++;
      }
    }
    if (arena == NULL) return NULL;

    assert(arena->gc_class == class);
    assert(arena->segment == segment);

    ObjectHeader * o = allocFromArena(arena);
    if (o != NULL) {
      assert(chunkFromPtr(o) == arena);
      assert(arena->object_size < 2 * size);
      return o;
    }

    // This arena is full. Move to full_arena to not have to search it again
    ArenaHeader * next = arena->next;

    ArenaHeader * full = Heap[class][segment].full_arena;
    arena->next = full;
    Heap[class][segment].full_arena = arena;

    Heap[class][segment].free_arena = next;
    arena = next;
  }
}

ObjectHeader * gcAlloc(size_t size, int class) {
  assert(size >= 0);
  assert(class >= 0 && class < NUM_CLASSES);

  int segment = getFixedSegmentForSize(size);
  if (segment == -1) {
    segment = VARIABLE_LARGE_NODE_SEGMENT;
  }

  // Assert the next smaller segment is actually smaller
  assert(segment == 0 ||
         (segment < NUM_FIXED_HEAP_SEGMENTS ?
          heapSegmentNodeSize(segment-1) < size :
          MAX_FIXED_NODE_SIZE            < size));
  // Assert segment is big enough
  assert(segment >= NUM_FIXED_HEAP_SEGMENTS ||
         (heapSegmentNodeSize(segment) >= size));

  int grow = segment < NUM_FIXED_HEAP_SEGMENTS ?
    Heap[class][segment].size < Heap[class][segment].size_limit : 0;

  ObjectHeader * o = allocFromSegment(class, segment, size, grow);
  if (o == NULL) {
    if (isFullGcDue()) {
      doGc(1);
      fullGcDone();
    } else {
      doGc(0);
    }

    o = allocFromSegment(class, segment, size, 0);
    if (o == NULL && segment < NUM_FIXED_HEAP_SEGMENTS) {
      growHeap(class, segment);
      o = allocFromSegment(class, segment, size, 1);
    }
  }

  if (o == NULL) {
    fatalError("Out of memory");
  }

  return o;
}


/*
 * Mark and Sweep
 *
 */

#ifdef DEBUG
static ObjectHeader * kGcZapPointer = ((ObjectHeader*)0xdeadbeef);
#endif

extern void gcForward(ObjectHeader * object) {
  markStackPush(object);
  *getMark(object) = GREY_MARK;
}

#define _FORWARD_CHILD_IF_UNMARKED(child, _) \
  if (child != NULL && *getMark(child) == WHITE_MARK) gcForward(child)
#define _ASSERT_CHILD_MARKED(child, _) \
  assert(child == NULL || *getMark(child) != WHITE_MARK)

void gcMark() {
  while(!markStackEmpty()) {
    ObjectHeader * cur = markStackPop();
    char * mark         = getMark(cur);
#ifdef DEBUG
    assert(*mark != WHITE_MARK);
    if (*mark == BLACK_MARK) {
      DO_CHILDREN(cur, _ASSERT_CHILD_MARKED, NULL);
    }
#endif
    if (*mark != BLACK_MARK) {
      DO_CHILDREN(cur, _FORWARD_CHILD_IF_UNMARKED, NULL);
      *mark = BLACK_MARK;
    }
  }

  resetMarkStack();
}

#define _ZAP_CHILD(c, _) c = kGcZapPointer;

void sweepArena(ArenaHeader * arena) {
  if (arena->segment >= NUM_FIXED_HEAP_SEGMENTS) {
    assert(getNumObjects(arena) == 1);
    assert(arena->num_alloc == 1);
  }
  arena->free_list = NULL;
  int num_alloc = 0;
  int create_freelist = arena->segment < NUM_FIXED_HEAP_SEGMENTS;
  if (create_freelist) {
    float bump_space = (float)((uintptr_t)getArenaEnd(arena) -
                               (uintptr_t)arena->free) /
                       (float)((uintptr_t)getArenaEnd(arena) -
                               (uintptr_t)arena->first);
    // Do not create freelist for an almost empty area.
    create_freelist = bump_space < 0.75;
  }

  FreeObject * free_list    = NULL;
  char * mark               = getBytemap(arena);
  ObjectHeader * finger     = arena->first;
  ObjectHeader * last_black = finger;
  uintptr_t             end = (uintptr_t)arena->free;

  while ((uintptr_t)finger < end) {
    assert(getMark(finger) == mark);
    assert((void*)mark < arena->first);
    assert((void*)mark >= (void*)(arena+1));
    assert(*mark != GREY_MARK);

    if (*mark == WHITE_MARK) {
#ifdef DEBUG
      FreeObject * f = (FreeObject*)finger;
      if (f->was_zapped != 119) {
        ObjectHeader * o  = (ObjectHeader*)finger;
        DO_CHILDREN(o, _ZAP_CHILD, NULL);
        // Mark object as zapped to avoid zapping twice
        f->was_zapped = 119;
      }
#endif
      if (create_freelist) {
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
      num_alloc++;
    }
    nextObject(&finger, arena);
    mark++;
  }
  arena->num_alloc = num_alloc;
  if (free_list != NULL) {
    free_list->next = NULL;
  }
  // If there happens to be an empty area at the end of the arena lets
  // reenable bump allocation for that part.
  nextObject(&last_black, arena);
  FreeObject * first_free = (FreeObject*)last_black;
  nextObject(&last_black, arena);
  FreeObject * second_free = (FreeObject*)last_black;
  if (arena->free > (void*)second_free) {
    first_free->next = NULL;
    arena->free = second_free;
  }
}

void removeArena(ArenaHeader * arena) {
  if (arena->segment < NUM_FIXED_HEAP_SEGMENTS) {
    Heap[arena->gc_class][arena->segment].size--;
  }
  freeArena(arena);
}

void gcSweep(int full_gc) {
  int max_class = gcCurrentClass();
  int release_variable_arenas = checkReleaseVariableArenas();
  assert(max_class <= NUM_CLASSES && max_class > 0);
  for (int class = 0; class < max_class; class++) {
    for (int i = 0; i < NUM_HEAP_SEGMENTS; i++) {
      if (i >= NUM_FIXED_HEAP_SEGMENTS && !release_variable_arenas) {
        continue;
      }

      ArenaHeader * arena     = Heap[class][i].free_arena;
      ArenaHeader * last_free = NULL;
      while (arena != NULL) {
        sweepArena(arena);
        sweepingDone(arena);
        // Release empty arenas
        if (arena->num_alloc == 0) {
          if (last_free != NULL) {
            last_free->next = arena->next;
          } else {
            Heap[class][i].free_arena = arena->next;
          }
          ArenaHeader * to_free = arena;
          arena = arena->next;
          removeArena(to_free);
        } else {
          last_free = arena;
          arena = arena->next;
        }
      }
      arena = Heap[class][i].full_arena;
      ArenaHeader * prev_full = NULL;
      while (arena != NULL) {
        if (full_gc || isSweepingCandidate(arena)) {
          sweepArena(arena);
          sweepingDone(arena);

          // Release empty arenas
          if (arena->num_alloc == 0) {
            if (prev_full != NULL) {
              prev_full->next = arena->next;
            } else {
              Heap[class][i].full_arena = arena->next;
            }
            ArenaHeader * to_free = arena;
            arena = arena->next;
            removeArena(to_free);
            continue;
          }

          // Move arenas with empty space to the free_arena list
          if (!isArenaConsideredFull(arena)) {
            if (last_free != NULL) {
              last_free->next = arena;
            } else {
              Heap[class][i].free_arena = arena;
            }
            last_free = arena;
            if (prev_full == NULL) {
              Heap[class][i].full_arena = arena->next;
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
        tryShrinkHeap(class, i);
      }
    }
  }
}

unsigned int getDiff(struct timespec a, struct timespec b) {
  if (a.tv_sec == b.tv_sec) {
    return (b.tv_nsec - a.tv_nsec) / 1000000;
  } else {
    return (1000000000L - a.tv_nsec + b.tv_nsec +
            (1000000000L * (b.tv_sec - a.tv_sec - 1))) / 1000000;
  }
}

void gcForceRun() {
  doGc(1);
}

void doGc(int full_gc) {
  updateMaxClass();

  assert(gcCurrentClass() <= NUM_CLASSES);

#ifdef DEBUG
  verifyHeap();
#endif

  static struct timespec a, b, c, d;
  if (gcReportingEnabled) clock_gettime(CLOCK_REALTIME, &a);

  if (full_gc) {
    resetMarkStack();
    for (int class = 0; class < gcCurrentClass(); class++) {
      for (int i = 0; i < NUM_HEAP_SEGMENTS; i++) {
        ArenaHeader * arena = Heap[class][i].free_arena;
        while (arena != NULL) {
          clearAllMarks(arena);
          arena = arena->next;
        }
        arena = Heap[class][i].full_arena;
        while (arena != NULL) {
          clearAllMarks(arena);
          arena = arena->next;
        }
      }
    }
  }

  if (gcReportingEnabled) clock_gettime(CLOCK_REALTIME, &b);
  gcMarkWrapper();
#ifdef DEBUG
  verifyHeap();
#endif
  if (gcReportingEnabled) clock_gettime(CLOCK_REALTIME, &c);
  gcSweep(full_gc);
  if (gcReportingEnabled) clock_gettime(CLOCK_REALTIME, &d);

#ifdef DEBUG
  verifyHeap();
#endif

  if (gcReportingEnabled) {
    printf("Gc report full: %d, max class: %d:\n", full_gc, gcCurrentClass());
    if (full_gc) {
      printf("clearing mark bits took: %d ms\n", getDiff(a, b));
    }
    printf("marking took: %d ms\n", getDiff(b, c));
    printf("sweeping took: %d ms\n", getDiff(c, d));
    printMemoryStatistics();
  }
}

void gcInit() {
  assert(heapInitNumArena > 0);
  for (int class = 0; class < NUM_CLASSES; class++) {
    for (int i = 0; i < NUM_HEAP_SEGMENTS; i++) {
      Heap[class][i].free_arena      = NULL;
      Heap[class][i].full_arena      = NULL;
      if (i < NUM_FIXED_HEAP_SEGMENTS) {
        Heap[class][i].size_limit = heapInitNumArena;
        Heap[class][i].size       = 0;
      }
    }
  }

  assert(1<<((int)log2(SMALLEST_SEGMENT_SIZE)) == SMALLEST_SEGMENT_SIZE);
  assert(SMALLEST_SEGMENT_SIZE >= sizeof(ObjectHeader));
  assert(SMALLEST_SEGMENT_SIZE <= (1<<(1+(int)log2(sizeof(ObjectHeader)))));

  buildGcSegmentSizeLookupTable();

  resetMarkStack();
}

void gcTeardown() {
  doGc(1);
  for (int class = 0; class < NUM_CLASSES; class++) {
    for (int i = 0; i < NUM_HEAP_SEGMENTS; i++) {
      ArenaHeader * arena = Heap[class][i].free_arena;
      while (arena != NULL) {
        ArenaHeader * next = arena->next;
        removeArena(arena);
        arena = next;
      }
      arena = Heap[class][i].full_arena;
      while (arena != NULL) {
        ArenaHeader * next = arena->next;
        removeArena(arena);
        arena = next;
      }
      if (i < NUM_FIXED_HEAP_SEGMENTS) {
        assert(Heap[class][i].size == 0);
      }
    }
  }
}


/*
 * Debug
 *
 */

#define _VERIFY_CHILD(child, parent) \
  if (child != NULL) _verifyChild(child, parent)

void _verifyChild(ObjectHeader * child, ObjectHeader * parent) {
#ifdef VERIFY_HEAP
  assert(child != kGcZapPointer);
  ArenaHeader * child_arena = chunkFromPtr(child);
  assert(child_arena->num_alloc > 0);
  if (*getMark(parent) == BLACK_MARK) assert(*getMark(child) != WHITE_MARK);
#endif
}

void verifyArena(ArenaHeader * arena) {
#ifdef VERIFY_HEAP
  ObjectHeader * o    = arena->first;
  char *         mark = getBytemap(arena);
  while((uintptr_t)o < getArenaEnd(arena) &&
        (uintptr_t)o < (uintptr_t)arena->free) {
    assert(getMark(o) == mark);
    if (*mark != WHITE_MARK) {
      DO_CHILDREN(o, _VERIFY_CHILD, o);
    }
    nextObject(&o, arena);
    mark++;
  }
#endif
}

void verifyHeap() {
  for (int class = 0; class < NUM_CLASSES; class++) {
    for (int i = 0; i < NUM_HEAP_SEGMENTS; i++) {
      ArenaHeader * arena = Heap[class][i].free_arena;
      while (arena != NULL) {
        verifyArena(arena);
        arena = arena->next;
      }
      arena = Heap[class][i].full_arena;
      while (arena != NULL) {
        verifyArena(arena);
        arena = arena->next;
      }
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
  for (int class = 0; class < NUM_CLASSES; class++) {
    unsigned long space  = 0;
    unsigned long usable = 0;
    unsigned long used   = 0;
    for (int i = 0; i < NUM_FIXED_HEAP_SEGMENTS; i++) {
      ArenaHeader * arena = Heap[class][i].free_arena;
      while (arena != NULL) {
        space  += arena->raw_size;
        usable += getNumObjects(arena) * getObjectSize(arena);
        used   += arena->num_alloc * getObjectSize(arena);
        arena   = arena->next;
      }
      arena = Heap[class][i].full_arena;
      while (arena != NULL) {
        space  += arena->raw_size;
        usable += getNumObjects(arena) * getObjectSize(arena);
        used   += arena->num_alloc * getObjectSize(arena);
        arena   = arena->next;
      }
    }
    printf("[%d] Fixed space:   Reserverd: %lu mb | Usable: %lu mb | Used: %lu mb\n",
        class, space / 1048576, usable / 1048576, used / 1048576);
    space  = 0;
    usable = 0;
    used   = 0;
    for (int i = NUM_FIXED_HEAP_SEGMENTS;
         i < NUM_FIXED_HEAP_SEGMENTS+NUM_VARIABLE_HEAP_SEGMENTS;
         i++) {
      ArenaHeader * arena = Heap[class][i].free_arena;
      while (arena != NULL) {
        space  += arena->raw_size;
        usable += getNumObjects(arena) * getObjectSize(arena);
        used   += arena->num_alloc * getObjectSize(arena);
        arena   = arena->next;
      }
      arena = Heap[class][i].full_arena;
      while (arena != NULL) {
        space  += arena->raw_size;
        usable += getNumObjects(arena) * getObjectSize(arena);
        used   += arena->num_alloc * getObjectSize(arena);
        arena   = arena->next;
      }
    }
    printf("[%d] Large Vectors: Reserverd: %lu mb | Usable: %lu mb | Used: %lu mb\n",
        class, space / 1048576, usable / 1048576, used / 1048576);
  }
}


/* utils */

void fatalError(const char * msg) {
  puts(msg);
#ifdef DEBUG
  __asm("int3");
#endif
  exit(1);
}

void gcEnableReporting() {
  gcReportingEnabled = 1;
}
