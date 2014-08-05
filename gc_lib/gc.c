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

#include "object.h"
#include "debugging.h"
#include "stack.h"


/*
 * Globals
 *
 */

ObjectHeader * Nil;
ObjectHeader * Root;

static int gcReportingEnabled = 1;

static HeapStruct Heap;


/*
 * Heap Constants
 *
 */

#define SLOT_SIZE sizeof(void*)

#define NUM_FIXED_HEAP_SEGMENTS 8
#define NUM_VARIABLE_HEAP_SEGMENTS 1
#define NUM_HEAP_SEGMENTS (NUM_FIXED_HEAP_SEGMENTS + NUM_VARIABLE_HEAP_SEGMENTS)
#define VARIABLE_LARGE_NODE_SEGMENT NUM_FIXED_HEAP_SEGMENTS

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
};

struct HeapStruct {
  ArenaHeader * free_arena[NUM_HEAP_SEGMENTS];
  ArenaHeader * full_arena[NUM_HEAP_SEGMENTS];
  unsigned int heap_size_limit[NUM_FIXED_HEAP_SEGMENTS];
  unsigned int heap_size[NUM_FIXED_HEAP_SEGMENTS];
};


/*
 * Heuristic Constants
 *
 */

const float heapGrowFactor                = 1.4;
const float heapShrinkFactor              = 1.2;

#define FULL_GC_INTERVAL 10
#define RELEASE_VARIABLE_ARENAS_INTERVAL 4

const float arenaFullPercentage           = 0.95;

const int heapInitNumArena = 4;


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
  int diff = (required + align) % align;
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

#define __GC_SEGMENT_SIZE_LOOKUP_TABLE_SIZE \
  (LARGEST_FIXED_SEGMENT_SIZE/SLOT_SIZE)
int __gcSegmentSizeLookupTable[__GC_SEGMENT_SIZE_LOOKUP_TABLE_SIZE];

inline static int getFixedSegmentForLength(int length) {
  if (length >= __GC_SEGMENT_SIZE_LOOKUP_TABLE_SIZE) return -1;
  return __gcSegmentSizeLookupTable[length];
}

void buildGcSegmentSizeLookupTable() {
  // Build the lookup tables
  int segment = 0;
  for (int i = 0; i < __GC_SEGMENT_SIZE_LOOKUP_TABLE_SIZE; i++) {
    int size = objectLengthToSize(i);
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
    __gcSegmentSizeLookupTable[i] = segment;
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

ArenaHeader * allocateAligned(int variable_length) {
  assert(1<<GC_ARENA_ALIGN_BITS == GC_ARENA_ALIGNMENT);
  assert(GC_ARENA_ALIGNMENT % sysconf(_SC_PAGESIZE) == 0);
  assert(GC_ARENA_SIZE      % sysconf(_SC_PAGESIZE) == 0);
  assert(GC_ARENA_SIZE <= GC_ARENA_ALIGNMENT);

  int OSPageAlignment = sysconf(_SC_PAGESIZE);

  int aligned_length = roundUpMemory(variable_length, OSPageAlignment);

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
  int prefix = (size_t)aligned_base - base;
  assert(prefix >= 0);
  if (prefix > 0) {
    munmap((void*)base, prefix);
  }

  int suffix = (base + request_length) - (aligned_base + aligned_length);
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

ArenaHeader * allocateAlignedArena(int segment) {
  ArenaHeader * chunk = NULL;
  assert(segment < NUM_FIXED_HEAP_SEGMENTS);

  chunk = allocateAligned(GC_ARENA_SIZE);
  if (chunk == NULL) return NULL;

  chunk->segment           = segment;
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

ArenaHeader * allocateAlignedChunk(int segment, int length) {
  assert(segment >= NUM_FIXED_HEAP_SEGMENTS);

  int object_size = objectLengthToSize(length);

  int header = sizeof(ArenaHeader) + GC_ARENA_MAX_BYTEMAP_OFFSET +
               arenaStartAlign;

  ArenaHeader * arena = allocateAligned(object_size + header);
  if (arena == NULL) return NULL;

  arena->num_alloc         = 1;
  arena->segment           = segment;
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

ArenaHeader * newArena(int segment) {
  ArenaHeader * new_arena = allocateAlignedArena(segment);
  if (new_arena == NULL) return NULL;

  ArenaHeader * first     = Heap.free_arena[segment];
  new_arena->next = first;
  Heap.free_arena[segment] = new_arena;
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
 * Heuritics
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

void growHeap(HeapStruct * heap, int segment) {
  if (doFullGc > 0) doFullGc--;
  int grow = heap->heap_size_limit[segment] * heapGrowFactor + 1;
  debug("segment %d : raising limit from %d to %d arenas. currently used: %d\n",
      segment, heap->heap_size_limit[segment], grow, heap->heap_size[segment]);
  heap->heap_size_limit[segment] = grow;
}

void tryShrinkHeap(HeapStruct * heap, int segment) {
  int shrink = heap->heap_size[segment] * heapShrinkFactor + 1;
  if (heap->heap_size_limit[segment] > shrink && shrink >= heapInitNumArena) {
    if (doFullGc > 0) doFullGc--;
    heap->heap_size_limit[segment] = shrink;
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


/*
 * Object access
 *
 */

extern inline ObjectHeader ** getSlots(ObjectHeader * o);
extern inline ObjectHeader * getSlot(ObjectHeader * o, int i);
extern inline void setSlot(ObjectHeader * o, int i, ObjectHeader * c);
extern inline void writeBarrier(ObjectHeader * parent, ObjectHeader * child);

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
extern inline int getFixedSegmentForLength(int length);

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

  if (o == NULL) {
    fatalError("Out of memory");
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


/*
 * Mark and Sweep
 *
 */

#ifdef DEBUG
static ObjectHeader * kGcZapPointer = ((ObjectHeader*)0xdeadbeef);
#endif

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
      *mark = BLACK_MARK;
    }
  }

  resetMarkStack();
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
      if (bump_space < 0.75) {
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

unsigned int getDiff(struct timespec a, struct timespec b) {
  if (a.tv_sec == b.tv_sec) {
    return (b.tv_nsec - a.tv_nsec) / 1000000;
  } else {
    return (1000000000L - a.tv_nsec + b.tv_nsec +
            (1000000000L * (b.tv_sec - a.tv_sec - 1))) / 1000000;
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


/*
 * Debug
 *
 */

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


/* utils */

void fatalError(const char * msg) {
  puts(msg);
#ifdef DEBUG
  __asm("int3");
#endif
  exit(1);
}
