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




/*
 * Heap Constants
 *
 */

#define NUM_FIXED_HEAP_SEGMENTS 12
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

struct HeapStruct {
  ArenaHeader * free_arena;
  ArenaHeader * full_arena;
  unsigned long size_limit;
  unsigned long size;
  unsigned long long alloc_count;
  unsigned long long object_count;
};


/*
 * Globals
 *
 */

static int gcReportingEnabled = 0;

static HeapStruct   Heap[NUM_CLASSES][NUM_HEAP_SEGMENTS];

static StackChunk * MarkStack;


/*
 * Heuristic Constants
 *
 */

const float heapGrowFactor                = 1.2;
const float heapShrinkFactor              = 1.1;

const float minFreedSpaceLimit = 0.15;
const float maxUsedSpaceLimit  = 0.8;

#define FULL_GC_INTERVAL 35
#define RELEASE_VARIABLE_ARENAS_INTERVAL 1

const float arenaFullPercentage           = 0.95;
const float arenaEmptyPercentage          = 0.05;
const float createFreelistThreshold       = 0.3;

const int heapInitNumArena = 1;
const int heapInitNumVarArena = 30;

const int gcClassRelation = 40;


/*
 * Arenas and memory access (helpers)
 *
 */

const int arenaStartAlign = 64;

extern inline char * getBytemap(ArenaHeader * base);
extern inline ArenaHeader * chunkFromPtr(void * base);
extern inline int getObjectBits(ArenaHeader * arena);
extern inline int getBytemapIndex(void * base, ArenaHeader * arena);
extern inline uintptr_t getArenaFirst(ArenaHeader * arena);

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
  return getArenaFirst(arena) + (getObjectSize(arena)*getNumObjects(arena));
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

  int header = sizeof(ArenaHeader) + arenaStartAlign;

  int num_objects = (total_size - header) / (1 + object_size);

  assert(num_objects > 0);
  assert(total_size >= header + num_objects +
                       (num_objects * object_size));

  return (size_t)num_objects;
}

int __gcSegmentSizeLookupTable[LARGEST_FIXED_SEGMENT_SIZE+1];

int getFixedSegmentForSize(long length) {
  return (length > LARGEST_FIXED_SEGMENT_SIZE) ?
    VARIABLE_LARGE_NODE_SEGMENT :
    __gcSegmentSizeLookupTable[length];
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

void * getRealPageStart(ArenaHeader * arena) {
  return (void*)(((uintptr_t)arena & ~GC_ARENA_ALIGN_MASK));
}

size_t getRealPageSize(int segment, size_t object_size) {
  size_t size = 0;
  if (segment < NUM_FIXED_HEAP_SEGMENTS) {
    size = GC_ARENA_SIZE;
  } else {
    int header = sizeof(ArenaHeader) + arenaStartAlign;
    size = object_size + header;
  }
  int OSPageAlignment = sysconf(_SC_PAGESIZE);
  return roundUpMemory(size, OSPageAlignment);
}

size_t getRealPageSizeFromArena(ArenaHeader * arena) {
  return getRealPageSize(arena->segment, arena->object_size);
}

ArenaHeader * allocateAligned(size_t variable_length) {
  assert(1<<GC_ARENA_ALIGN_BITS == GC_ARENA_ALIGNMENT);
  assert(GC_ARENA_ALIGNMENT % sysconf(_SC_PAGESIZE) == 0);
  assert(GC_ARENA_SIZE      % sysconf(_SC_PAGESIZE) == 0);
  assert(GC_ARENA_SIZE <= GC_ARENA_ALIGNMENT);

  int OSPageAlignment = sysconf(_SC_PAGESIZE);

  assert(variable_length % OSPageAlignment == 0);

  size_t aligned_length = variable_length;

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

  ArenaHeader * arena = (ArenaHeader*)((uintptr_t)commited);

  assert(getRealPageStart(arena) == commited);
  return arena;
}

ArenaHeader * allocateAlignedArena(int class, int segment) {
  ArenaHeader * chunk = NULL;
  assert(segment < NUM_FIXED_HEAP_SEGMENTS);

  chunk = allocateAligned(getRealPageSize(segment, -1));
  if (chunk == NULL) return NULL;

  chunk->segment            = segment;
  chunk->gc_class           = class;
  int num_objects           = calcNumOfObjects(chunk,
                                               GC_ARENA_SIZE,
                                               heapSegmentNodeSize(segment));
  chunk->object_size        = heapSegmentNodeSize(segment);
  chunk->object_bits        = log2(heapSegmentNodeSize(segment));

  chunk->num_objects        = num_objects;
  chunk->first_offset       = roundUpMemory(num_objects, arenaStartAlign);

  ObjectHeader * first      = (ObjectHeader*)getArenaFirst(chunk);
  chunk->free               = first;
  chunk->num_alloc          = 0;

  chunk->free_list = allocStackChunk();

  assert((uintptr_t)((char*)first +
                     (chunk->num_objects * chunk->object_size)) <=
         (uintptr_t)((char*)getRealPageStart(chunk) +
                     getRealPageSizeFromArena(chunk)));

  clearAllMarks(chunk);

  return chunk;
}

ArenaHeader * allocateAlignedChunk(int class, int segment, size_t object_size) {
  assert(segment >= NUM_FIXED_HEAP_SEGMENTS);

  ArenaHeader * arena = allocateAligned(getRealPageSize(segment, object_size));
  if (arena == NULL) return NULL;

  arena->segment           = segment;
  arena->gc_class          = class;
  arena->num_objects       = 1;
  arena->num_alloc         = 1;
  arena->first_offset      = arenaStartAlign;
  arena->object_size       = object_size;

  arena->object_bits       = GC_ARENA_ALIGN_BITS;
  arena->free_list         = NULL;

  ObjectHeader * first     = (ObjectHeader*)getArenaFirst(arena);
  arena->free              = (void*)getArenaEnd(arena);

  assert((uintptr_t)((char*)first +
                     (arena->num_objects * arena->object_size)) <=
         (uintptr_t)((char*)getRealPageStart(arena) +
                     getRealPageSizeFromArena(arena)));
  assert(getMark(first) == getBytemap(arena));

  clearAllMarks(arena);

  return arena;
}

ArenaHeader * newArena(int class, int segment) {
  ArenaHeader * new_arena = allocateAlignedArena(class, segment);
  if (new_arena == NULL) return NULL;

  ArenaHeader * first     = Heap[class][segment].free_arena;
  new_arena->next = first;
  Heap[class][segment].free_arena = new_arena;
  Heap[class][segment].object_count += new_arena->num_objects;
  return new_arena;
}

void freeArena(ArenaHeader * arena) {
  Heap[arena->gc_class][arena->segment].object_count -= arena->num_objects;
  stackFree(arena->free_list);
#ifdef USE_POSIX_MEMALIGN
  free(getRealPageStart(arena));
#else
  munmap(getRealPageStart(arena), getRealPageSizeFromArena(arena));
#endif
}


/*
 * Heuristics
 *
 */

static int  minorSinceFullGc      = FULL_GC_INTERVAL;
static int  releaseVariableArenas = RELEASE_VARIABLE_ARENAS_INTERVAL;

int isFullGcDue() {
  return minorSinceFullGc <= 0;
}

void growHeap(int class, int segment) {
  unsigned long grow = (double)Heap[class][segment].size_limit * heapGrowFactor + 1;
  if (gcReportingEnabled) {
    printf(
      "class %d, segment %d : raising limit from %lu to %lu arenas. currently used: %lu\n",
      class, segment, Heap[class][segment].size_limit,
      grow, Heap[class][segment].size);
  }
  Heap[class][segment].size_limit = grow;
}

void tryShrinkHeap(int class, int segment) {
  unsigned long shrink = (double)Heap[class][segment].size * heapShrinkFactor + 1;
  if (Heap[class][segment].size_limit > shrink &&
      (segment < NUM_FIXED_HEAP_SEGMENTS ?
        shrink >= heapInitNumArena :
        shrink >= heapInitNumVarArena)) {
    if (gcReportingEnabled) {
      printf(
        "class %d, segment %d : shrinking limit from %lu to %lu arenas. currently used: %lu\n",
        class, segment, Heap[class][segment].size_limit,
        shrink, Heap[class][segment].size);
    }
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
  float population = (float)arena->num_alloc /
                     (float)getNumObjects(arena);
  return arena->segment >= NUM_FIXED_HEAP_SEGMENTS ||
         (!arena->was_full && population > arenaEmptyPercentage);
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

void updateMaxClass(int full_gc) {
  if (currentGcClass == NUM_CLASSES) {
    gcsSincMaxClass = 1;
  } else {
    gcsSincMaxClass++;
  }

  if (full_gc) {
    currentGcClass = NUM_CLASSES;
  } else {
    currentGcClass = 1;
    for (int i = 1; i < NUM_CLASSES; i++) {
      if (gcsSincMaxClass % (i * gcClassRelation) == 0) {
        currentGcClass++;
      }
    }
  }
}


/*
 * Object access
 *
 */

extern inline void gcWriteBarrier(ObjectHeader * parent, ObjectHeader * child);


/*
 * Allocation
 *
 */

void verifyHeap();
void doGc(int full, int segment);

ObjectHeader * allocFromArena(ArenaHeader * arena) {
  if (!stackEmpty(arena->free_list)) {
    ObjectHeader * o = stackPop(&arena->free_list);
    arena->num_alloc++;
    return o;
  }
  if ((uintptr_t)arena->free < getArenaEnd(arena)) {
    // Bump pointer allocation in virgin space
    ObjectHeader * o = (ObjectHeader*)arena->free;
    arena->free = arena->free + getObjectSize(arena);
    arena->num_alloc++;
    return o;
  }
  return NULL;
}

static inline ObjectHeader * tryFastAllocFromSegment(int class,
                                              int segment) {
  ArenaHeader * arena = Heap[class][segment].free_arena;
  if (arena == NULL) return NULL;
  return allocFromArena(arena);
}

ObjectHeader * allocFromFixedSegment(int class,
                                     int segment,
                                     int new_arena) {
  assert(segment < NUM_FIXED_HEAP_SEGMENTS);
  ArenaHeader * arena = Heap[class][segment].free_arena;
  while (1) {
    if (arena == NULL) {
      if (new_arena == 1) {
        arena = newArena(class, segment);
        if (arena != NULL) {
          Heap[class][segment].size++;
        }
      } else {
        return NULL;
      }
    }

    assert(arena->gc_class == class);
    assert(arena->segment == segment);

    ObjectHeader * o = allocFromArena(arena);
    if (o != NULL) {
      assert(chunkFromPtr(o) == arena);

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

ObjectHeader * allocFromVariableSegment(int class,
                                        int segment,
                                        size_t size,
                                        int new_arena) {
  assert(segment >= NUM_FIXED_HEAP_SEGMENTS);

  if (!new_arena) {
    return NULL;
  }

  ArenaHeader * arena = allocateAlignedChunk(class, segment, size);
  if (arena == NULL) {
    return NULL;
  }

  Heap[class][segment].size++;
  ArenaHeader * first = Heap[class][segment].full_arena;
  arena->next = first;
  Heap[class][segment].full_arena = arena;

  return (ObjectHeader*)getArenaFirst(arena);
}

ObjectHeader * allocFromSegment(int class,
                                int segment,
                                size_t size,
                                int grow) {
  return (segment < NUM_FIXED_HEAP_SEGMENTS) ?
    allocFromFixedSegment(class, segment, grow) :
    allocFromVariableSegment(class, segment, size, grow);
}

ObjectHeader * gcAllocDeferred(size_t size, int class, int segment) {
  int grow = Heap[class][segment].size < Heap[class][segment].size_limit;
  ObjectHeader * o = allocFromSegment(class, segment, size, grow);

  if (o == NULL) {
    int full_gc = isFullGcDue();

    float used_space_before = (float)Heap[class][segment].alloc_count /
                              (float)Heap[class][segment].object_count;

    // No free space, do gc
    doGc(full_gc, segment);
    if (full_gc) minorSinceFullGc = FULL_GC_INTERVAL;

    float used_space_after  = (float)Heap[class][segment].alloc_count /
                              (float)Heap[class][segment].object_count;


    if (used_space_before - used_space_after < minFreedSpaceLimit ||
        used_space_after > maxUsedSpaceLimit) {
      if (full_gc) {
        growHeap(class, segment);
      } else {
        minorSinceFullGc--;
      }
    }

    int grow = Heap[class][segment].size < Heap[class][segment].size_limit;
    o = allocFromSegment(class, segment, size, grow);

    if (o == NULL) {
      // Still no free space, grow heap
      growHeap(class, segment);
      o = allocFromSegment(class, segment, size, 1);
    }
  }

  if (o == NULL) {
    fatalError("Out of memory");
  }

  return o;
}

double alloc_avg[NUM_HEAP_SEGMENTS];
unsigned long long alloc_cnt[NUM_HEAP_SEGMENTS];

ObjectHeader * gcAlloc(size_t size, int class) {
  assert(size >= 0);
  assert(class >= 0 && class < NUM_CLASSES);

  int segment = getFixedSegmentForSize(size);

  // Assert the next smaller segment is actually smaller
  assert(segment == 0 ||
         (segment < NUM_FIXED_HEAP_SEGMENTS ?
          heapSegmentNodeSize(segment-1) < size :
          MAX_FIXED_NODE_SIZE            < size));
  // Assert segment is big enough
  assert(segment >= NUM_FIXED_HEAP_SEGMENTS ||
         (heapSegmentNodeSize(segment) >= size));

  if (gcReportingEnabled) {
    double a = (float)size/(float)heapSegmentNodeSize(segment);
    alloc_avg[segment] = ((alloc_avg[segment] * alloc_cnt[segment]) + a);
    alloc_cnt[segment]++;
    alloc_avg[segment] /= alloc_cnt[segment];
  }

  if (segment < NUM_FIXED_HEAP_SEGMENTS) {
    ObjectHeader * o = tryFastAllocFromSegment(class, segment);
    if (o != NULL) return o;
  }

  return gcAllocDeferred(size, class, segment);
}


/*
 * Mark and Sweep
 *
 */

#ifdef DEBUG
const long  kGcZapPointer = 0xdeadbeef;
#endif

extern inline int stackEmpty(StackChunk * stack);
extern inline void stackPush(StackChunk ** stack_, ObjectHeader * o);
extern inline ObjectHeader * stackPop(StackChunk ** stack_);

void gcForward(ObjectHeader * object) {
  stackPush(&MarkStack, object);
  *getMark(object) = GREY_MARK;
}

#define _FORWARD_CHILD_IF_UNMARKED(child, _) \
  if (child != NULL && *getMark(child) == WHITE_MARK) { \
    stackPush(&MarkStack, child); \
    *getMark(child) = GREY_MARK; \
  }

#define _ASSERT_CHILD_MARKED(child, _) \
  assert(child == NULL || *getMark(child) != WHITE_MARK)

void gcMark() {
  while(!stackEmpty(MarkStack)) {
    ObjectHeader * cur = stackPop(&MarkStack);
    char * mark        = getMark(cur);
#ifdef DEBUG
    assert(*mark != WHITE_MARK);
    if (*mark == BLACK_MARK) {
      DO_CHILDREN(cur, _ASSERT_CHILD_MARKED, NULL);
    }
#endif
    if (*mark != BLACK_MARK) {
      DO_CHILDREN(cur, _FORWARD_CHILD_IF_UNMARKED, NULL);
      assert((uintptr_t)mark < getArenaFirst(chunkFromPtr(cur)));
      *mark = BLACK_MARK;
    }
  }
}

void sweepArena(ArenaHeader * arena) {
  if (arena->segment >= NUM_FIXED_HEAP_SEGMENTS) {
    assert(getNumObjects(arena) == 1);
    assert(arena->num_alloc == 1);
  }
  if (getArenaEnd(arena) > (uintptr_t)arena->free) {
    return;
  }

  int num_alloc = arena->num_objects;
  if (arena->segment < NUM_FIXED_HEAP_SEGMENTS) {
    stackReset(&arena->free_list);

    char * mark               = getBytemap(arena);
    char * end                = &getBytemap(arena)[arena->num_objects];
    ObjectHeader * finger     = (ObjectHeader*)getArenaFirst(arena);

    while (mark < end) {
      assert(getMark(finger) == mark);
      assert((uintptr_t)mark < getArenaFirst(arena));
      assert((void*)mark >= (void*)(arena+1));
      assert(*mark != GREY_MARK);

      if (*mark == WHITE_MARK) {
#ifdef DEBUG
        long * f = (long*)finger;
        for (int i = 0; i < getObjectSize(arena) / sizeof(long); i++) {
          *f = kGcZapPointer;
          f++;
        }
#endif
        num_alloc--;
        stackPush(&arena->free_list, finger);
      } else {
        assert(*mark == BLACK_MARK);
      }
      nextObject(&finger, arena);
      mark++;
    }
  } else {
    if (getBytemap(arena)[0] == WHITE_MARK) {
      num_alloc--;
    }
  }
  Heap[arena->gc_class][arena->segment].alloc_count -= arena->num_alloc;
  arena->num_alloc = num_alloc;
  Heap[arena->gc_class][arena->segment].alloc_count += num_alloc;
}

void removeArena(ArenaHeader * arena) {
  if (arena->segment < NUM_FIXED_HEAP_SEGMENTS) {
    Heap[arena->gc_class][arena->segment].size--;
  }
  freeArena(arena);
}

// sort by decreasing num_alloc (merge sort)
ArenaHeader * sortFreeArenas(ArenaHeader * list) {
  if (list == NULL) {
      return NULL;
  }

  int segmentation = 1;

  while (1) {
    int num_merges = 0;

    ArenaHeader * left = list;
                  list = NULL;
    ArenaHeader * tail = NULL;

    while (left != NULL) {
      num_merges++;

      ArenaHeader * right = left->next;
      int left_bucket_size = 1;

      while (left_bucket_size < segmentation && right != NULL) {
        left_bucket_size++;
        right = right->next;
      }

      int right_bucket_size = segmentation;

      while (left_bucket_size > 0 ||
             (right_bucket_size > 0 && right != NULL)) {

        ArenaHeader * next = NULL;
        if (right_bucket_size == 0 || right == NULL ||
            (left_bucket_size > 0 &&
             right->num_alloc < left->num_alloc)) {
          next = left;
          left = left->next;
          left_bucket_size--;
        } else {
          next  = right;
          right = right->next;
          right_bucket_size--;
        }

        if (tail != NULL) {
          tail->next = next;
        } else {
          list = next;
        }
        tail = next;
      }

      left = right;
    }
    tail->next = NULL;

    if (num_merges <= 1)
      return list;

    segmentation *= 2;
  }
}

void gcSweep(int full_gc, int segment) {
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
        if (full_gc || isSweepingCandidate(arena)) {
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
            continue;
          }
        }
        last_free = arena;
        arena = arena->next;
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
      tryShrinkHeap(class, i);

      // Sort arenas according to num_alloc: this creates more full arenas
      // (which do not have to be sweeped) since we first try to allocate in
      // more populated arenas.
      Heap[class][i].free_arena = sortFreeArenas(Heap[class][i].free_arena);
    }
  }
}

unsigned int getDiff(struct timespec a, struct timespec b) {
  if (a.tv_sec == b.tv_sec) {
    return (b.tv_nsec - a.tv_nsec) / 100000;
  } else {
    return (1000000000L - a.tv_nsec + b.tv_nsec +
            (1000000000L * (b.tv_sec - a.tv_sec - 1))) / 100000;
  }
}

void gcForceRun() {
  doGc(1, 0);
}

static unsigned long total_time = 0;

void doGc(int full_gc, int segment) {
  updateMaxClass(full_gc);

  assert(gcCurrentClass() <= NUM_CLASSES);

#ifdef DEBUG
  verifyHeap();
#endif

  if (gcReportingEnabled) {
    printf("--- %s GC initiated [max_class %d]:\n",
        full_gc ? "Full" : "Partial",
        gcCurrentClass());
    printMemoryStatistics();
  }

  static struct timespec a, b, c, d;
  if (gcReportingEnabled) clock_gettime(CLOCK_REALTIME, &a);

  if (full_gc) {
    stackReset(&MarkStack);
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
  gcSweep(full_gc, segment);
  if (gcReportingEnabled) clock_gettime(CLOCK_REALTIME, &d);

#ifdef DEBUG
  verifyHeap();
#endif

  if (gcReportingEnabled) {
    total_time += getDiff(a,d) / 10;
    if (full_gc) {
      printf("clearing mark bits took: %d 10ms\n", getDiff(a, b));
    }
    printf("marking took: %d 10ms\n", getDiff(b, c));
    printf("sweeping took: %d 10ms\n", getDiff(c, d));
    printf("total: %lu ms\n", total_time);
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
        Heap[class][i].size_limit    = heapInitNumArena;
      } else {
        Heap[class][i].size_limit    = heapInitNumVarArena;
      }
      Heap[class][i].size            = 0;
    }
  }

  assert(1<<((int)log2(SMALLEST_SEGMENT_SIZE)) == SMALLEST_SEGMENT_SIZE);
  //assert(SMALLEST_SEGMENT_SIZE >= sizeof(ObjectHeader));
  //assert(SMALLEST_SEGMENT_SIZE <= (1<<(1+(int)log2(sizeof(ObjectHeader)))));

  buildGcSegmentSizeLookupTable();

  MarkStack = allocStackChunk();
}

void gcTeardown() {
  doGc(1, 0);
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
  assert((long)child != kGcZapPointer);
  ArenaHeader * child_arena = chunkFromPtr(child);
  assert(child_arena->num_alloc > 0);
  if (*getMark(parent) == BLACK_MARK) assert(*getMark(child) != WHITE_MARK);
#endif
}

void verifyArena(ArenaHeader * arena) {
#ifdef VERIFY_HEAP
  ObjectHeader * o    = (ObjectHeader*)getArenaFirst(arena);
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
  for (int class = 0; class < 1; class++) {
    unsigned long space  = 0;
    unsigned long usable = 0;
    unsigned long used   = 0;
    printf("[%d] Fixed space:\n", class);
    for (int i = 0; i < NUM_FIXED_HEAP_SEGMENTS; i++) {
      int full_arenas      = 0;
      int free_arenas      = 0;
      float population     = 0;
      ArenaHeader * arena = Heap[class][i].free_arena;
      while (arena != NULL) {
        space  += getRealPageSizeFromArena(arena);
        space  += stackSize(arena->free_list);
        usable += getNumObjects(arena) * getObjectSize(arena);
        used   += arena->num_alloc * getObjectSize(arena);
        if (isArenaConsideredFull(arena)) {
          full_arenas++;
        } else {
          free_arenas++;
          population += (float)arena->num_alloc /
                        (float)getNumObjects(arena);
        }
        arena   = arena->next;
      }
      arena = Heap[class][i].full_arena;
      while (arena != NULL) {
        space  += getRealPageSizeFromArena(arena);
        space  += stackSize(arena->free_list);
        usable += getNumObjects(arena) * getObjectSize(arena);
        used   += arena->num_alloc * getObjectSize(arena);
        if (isArenaConsideredFull(arena)) {
          full_arenas++;
        } else {
          free_arenas++;
          population += (float)arena->num_alloc /
                        (float)getNumObjects(arena);
        }
        arena   = arena->next;
      }
      if (free_arenas > 0 || full_arenas > 0) {
        printf("     [%d] %d arenas average %0.2f, (used %f), %d arenas full\n",
            i, free_arenas,
            free_arenas > 0 ? population / (float)free_arenas : 0,
            alloc_avg[i],
            full_arenas);
      }

    }
    printf("     Reserverd: %lu mb | Usable: %lu mb | Used: %lu mb\n",
        space / 1048576, usable / 1048576, used / 1048576);
    space  = 0;
    usable = 0;
    used   = 0;
    for (int i = NUM_FIXED_HEAP_SEGMENTS;
         i < NUM_FIXED_HEAP_SEGMENTS+NUM_VARIABLE_HEAP_SEGMENTS;
         i++) {
      ArenaHeader * arena = Heap[class][i].free_arena;
      while (arena != NULL) {
        space  += getRealPageSizeFromArena(arena);
        space  += stackSize(arena->free_list);
        usable += getNumObjects(arena) * getObjectSize(arena);
        used   += arena->num_alloc * getObjectSize(arena);
        arena   = arena->next;
      }
      arena = Heap[class][i].full_arena;
      while (arena != NULL) {
        space  += getRealPageSizeFromArena(arena);
        space  += stackSize(arena->free_list);
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

void gcEnableReporting(int enable) {
  gcReportingEnabled = enable;
}
