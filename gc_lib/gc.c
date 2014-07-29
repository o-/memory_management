#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <sys/mman.h>

#include "object.h"

#include "debugging.h"

#include "gc.h"
#include "gc-const.h"
#include "gc-declarations.h"
#include "gc-heap.h"
#include "gc-utils.h"
#include "gc-heuristic.h"
#include "gc-mark-stack.h"

ObjectHeader * Nil;
ObjectHeader * Root;

static HeapStruct Heap;

#define _CHUNK_ALIGN_BITS 20
const int arenaAlignment  = 1<<_CHUNK_ALIGN_BITS;
const int arenaSize       = 1<<_CHUNK_ALIGN_BITS;
const int arenaAlignBits  = _CHUNK_ALIGN_BITS;
const int arenaAlignMask  = (1<<_CHUNK_ALIGN_BITS)-1;
const int arenaStartAlign = 16;;

const int maxBytemapOffset = 2048;

size_t roundUpMemory(size_t required, unsigned int align);

int objectSizeToLength(int size) {
  return (size - sizeof(ObjectHeader)) / SLOT_SIZE;
}

int objectLengthToSize(int length) {
  return (length * SLOT_SIZE) + sizeof(ObjectHeader);
}


int getNumObjects(ArenaHeader * arena) {
  return arena->num_objects;
}

int getObjectSize(ArenaHeader * arena) {
  return arena->object_size;
}

int getMaxObjectLength(ArenaHeader * arena) {
  return objectSizeToLength(arena->object_size);
}

int getObjectBits(ArenaHeader * arena) {
  return arena->object_bits;
}

int getBytemapIndex(void * base, ArenaHeader * arena) {
  return ((uintptr_t)base - (uintptr_t)arena->first) >> getObjectBits(arena);
}

// Offset the beginning of the area to the aligned base address for better
// cache efficiency
unsigned int arenaHeaderOffset(ArenaHeader * base) {
  unsigned int offset = (((uintptr_t)base >> _CHUNK_ALIGN_BITS) &
                        (maxBytemapOffset - 1));
  // Ensure offset is word aligned
  offset &= ~(sizeof(void*) - 1);
  assert(offset >= 0 && offset < maxBytemapOffset);
  return offset;
}

char * getBytemap(ArenaHeader * base, int generation) {
  return ((char*)(base + 1)) + (base->num_objects * generation);
}

int isAligned(void * base, unsigned int align) {
  return (uintptr_t)base % align == 0;
}

int isMaskable(void * ptr, ArenaHeader * chunk) {
  return (uintptr_t)ptr < (uintptr_t)chunk+arenaAlignment;
}

ArenaHeader * chunkFromPtr(void * base) {
  base = (ArenaHeader*)(((uintptr_t)base & ~arenaAlignMask));
  return (ArenaHeader*) ((uintptr_t)base + arenaHeaderOffset(base));
}

char * getMark(void * ptr, ArenaHeader * arena, int generation) {
  char *        bm    = getBytemap(arena, generation);
  int           idx   = getBytemapIndex(ptr, arena);
  return bm + idx;
}

void setGen(ObjectHeader * o, int gen) {
  if (o->old != gen) {
    o->old = gen;
  }
}

extern inline ObjectHeader ** getSlots(ObjectHeader * o);

size_t calcNumOfObjects(ArenaHeader * arena, int total_size, int object_size) {
  assert(total_size > 1024);

  // Area Prelude:
  // AreaHeader | bytemap_offset | bytemap_gen0 | bytemap_gen1 | align

  int header = sizeof(ArenaHeader) + arenaHeaderOffset(arena) + arenaStartAlign;

  int num_objects = (total_size - header) / (2 + object_size);

  assert(num_objects > 0);
  assert(total_size >= header + (2 * num_objects) +
                       (num_objects * object_size));

  // Ensure The bytemaps have a word aligned size
  num_objects &= ~(sizeof(void*) - 1);

  return (size_t)num_objects;
}

size_t roundUpMemory(size_t required, unsigned int align) {
  if (required % align == 0) return required;
  int diff = (required + align) % align;
  return required + align - diff;
}

uintptr_t nextAlignedAddress(uintptr_t base) {
  if ((base & ~arenaAlignMask) == base) return base;
  return (base+arenaAlignment) & ~arenaAlignMask;
}

ArenaHeader * allocateAligned(int variable_length) {
  int OSPageAlignment = sysconf(_SC_PAGESIZE);

  int aligned_length = roundUpMemory(variable_length, OSPageAlignment);

  void * commited = NULL;

#ifdef USE_POSIX_MEMALIGN
  posix_memalign(&commited, arenaAlignment, aligned_length);
  if(commited == NULL) return NULL;

#else

  // Length aligned to OS page size (required for mmap)
  assert(aligned_length % OSPageAlignment == 0);
  // Virtual memory needed to satisfy chunk alignment
  size_t request_length = roundUpMemory(aligned_length + arenaAlignment,
                                        OSPageAlignment);

  // Reserve virtual memory
  void * reserved = mmap(NULL,
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

uintptr_t getArenaEnd(ArenaHeader * arena) {
  return (uintptr_t)arena->first + (getObjectSize(arena)*getNumObjects(arena));
}

ArenaHeader * allocateAlignedArena(int segment) {
  ArenaHeader * chunk = NULL;
  assert (segment < NUM_FIXED_HEAP_SEGMENTS);

  chunk = allocateAligned(arenaSize);

  chunk->segment           = segment;
  int num_objects          = calcNumOfObjects(chunk,
                                              arenaSize,
                                              heapSegmentNodeSize(segment));
  chunk->num_objects       = num_objects;

  chunk->first             = &getBytemap(chunk, 1)[num_objects];

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

  // Zero bytemap
  memset(getBytemap(chunk,0), 0, 2*num_objects);

  return chunk;
}

void freeArena(ArenaHeader * arena) {
  if (arena->segment < NUM_FIXED_HEAP_SEGMENTS) {
    Heap.heap_size[arena->segment]--;
  }
#ifdef USE_POSIX_MEMALIGN
  free(arena);
#else
  munmap(arena->raw_base, arena->raw_size);
#endif
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
  ArenaHeader * first     = Heap.free_arena[segment];
  new_arena->next = first;
  Heap.free_arena[segment] = new_arena;
  return new_arena;
}

ObjectHeader * allocFromSegment(int segment,
                                int length,
                                int new_arena) {
  if (segment >= NUM_FIXED_HEAP_SEGMENTS) {
    int object_size = objectLengthToSize(length);

    int header = sizeof(ArenaHeader) + maxBytemapOffset + arenaStartAlign;

    ArenaHeader * arena = allocateAligned(object_size + header);

    arena->num_alloc         = 1;
    arena->segment           = segment;
    arena->num_objects       = 1;
    arena->object_size       = object_size;
    arena->object_bits       = arenaAlignBits;
    arena->free_list         = NULL;
    arena->first             = &getBytemap(arena, 1)[1];
    uintptr_t f = (uintptr_t)arena->first;
    if (f % arenaStartAlign != 0) {
      arena->first = (void*)(f + f % arenaStartAlign);
    }
    arena->free              = (void*)getArenaEnd(arena);

    ArenaHeader * first     = Heap.full_arena[segment];
    arena->next = first;
    Heap.full_arena[segment] = arena;

    // Zero bytemap
    memset(getBytemap(arena, 0), 0, 2);

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
  *getMark(Nil, chunkFromPtr(Nil), 0)   = GREY_MARK;
  *getMark(root, chunkFromPtr(root), 0) = GREY_MARK;
  markStackPush(Nil);
  markStackPush(root);

  while(!markStackEmpty()) {
    ObjectHeader * cur  = markStackPop();
    setGen(cur, 1);
    long length         = cur->length;
    ArenaHeader * arena = chunkFromPtr(cur);
    char * mark         = getMark(cur, arena, 0);
#ifdef DEBUG
    ObjectHeader ** children = getSlots(cur);
    assert(*mark != WHITE_MARK);
    if (*mark == BLACK_MARK) {
      for (int i = 0; i < length; i++) {
        ObjectHeader * child = children[i];
        assert(*getMark(child, chunkFromPtr(child), 0) != WHITE_MARK);
      }
    }
#endif
    if (*mark != BLACK_MARK) {
      ObjectHeader ** children        = getSlots(cur);
      for (int i = 0; i < length; i++) {
        ObjectHeader * child = children[i];
        char * child_mark = getMark(child, chunkFromPtr(child), 0);
        if (*child_mark == WHITE_MARK) {
          *child_mark = GREY_MARK;
          markStackPush(child);
        }
      }
      *mark = BLACK_MARK;
    }
  }

  resetMarkStack();
}

void writeBarrier(ObjectHeader * parent, ObjectHeader * child) {
  if (parent->old > child->old) {
    char * p_mark = getMark(parent, chunkFromPtr(parent), 0);
    if (*p_mark != GREY_MARK) {
      *p_mark = GREY_MARK;
      markStackPush(parent);
    }
  }
}

void sweepArena(ArenaHeader * arena) {
  char * finger = arena->first;
  arena->free_list = NULL;
  arena->num_alloc = 0;
  float bump_space = (float)((uintptr_t)getArenaEnd(arena) -
                             (uintptr_t)arena->free) /
                     (float)((uintptr_t)getArenaEnd(arena) -
                             (uintptr_t)arena->first);

  FreeObject * free_list = NULL;
  for (int i = 0;
      i < arena->num_objects && (uintptr_t)finger < (uintptr_t)arena->free;
      i++) {
    char * mark     = &getBytemap(arena, 0)[i];
    char * old_mark = &getBytemap(arena, 1)[i];
    assert((void*)mark < arena->first && (void*)old_mark < arena->first);
    assert(*mark != GREY_MARK);

    if (*mark == WHITE_MARK) {
      if (!*old_mark == WHITE_MARK) {
        *old_mark = WHITE_MARK;
      }
#ifdef DEBUG
      // Zap slots
      ObjectHeader * o  = (ObjectHeader*)finger;
      ObjectHeader ** s = getSlots(o);
      for (int i = 0; i < o->length; i++) {
        s[i] = GC_ZAP_POINTER;
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
      if (*old_mark == WHITE_MARK) {
        *old_mark = BLACK_MARK;
      }
      arena->num_alloc++;
    }
    finger += getObjectSize(arena);
  }
  if (free_list != NULL) {
    free_list->next = NULL;
  }
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
      if (arena->num_alloc == 0) {
        if (last_free != NULL) {
          last_free->next = arena->next;
        } else {
          Heap.free_arena[i] = arena->next;
        }
        ArenaHeader * to_free = arena;
        arena = arena->next;
        freeArena(to_free);
      } else {
        last_free = arena;
        arena = arena->next;
      }
    }
    arena = Heap.full_arena[i];
    ArenaHeader * prev_full = NULL;
    while (arena != NULL) {
      if (full_gc || !arena->was_full || i >= NUM_FIXED_HEAP_SEGMENTS) {
        sweepArena(arena);
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
        // Arena almost full -> don't sweep on minor collections
        arena->was_full = 1;
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

  assert(1<<arenaAlignBits == arenaAlignment);
  assert(arenaAlignment % sysconf(_SC_PAGESIZE) == 0);
  assert(arenaSize      % sysconf(_SC_PAGESIZE) == 0);
  assert(arenaSize <= arenaAlignment);

  buildGcSegmentSizeLookupTable();

  Nil = alloc(0);
  resetMarkStack();
}

unsigned long getDiff(struct timespec a, struct timespec b) {
  if (a.tv_sec == b.tv_sec) {
    return b.tv_nsec - a.tv_nsec;
  } else {
    return 1000000000L - a.tv_nsec + b.tv_nsec +
           (1000000000L * (b.tv_sec - a.tv_sec - 1));
  }
}

void doGc(int full_gc) {
  if (full_gc) {
    resetMarkStack();

    for (int i = 0; i < NUM_HEAP_SEGMENTS; i++) {
      ArenaHeader * arena = Heap.free_arena[i];
      while (arena != NULL) {
        ArenaHeader * next = arena->next;
        arena->was_full = 0;
        memset(getBytemap(arena,0), 0, 2*arena->num_objects);
        arena = next;
      }
      arena = Heap.full_arena[i];
      while (arena != NULL) {
        ArenaHeader * next = arena->next;
        arena->was_full = 0;
        memset(getBytemap(arena,0), 0, 2*arena->num_objects);
        arena = next;
      }
    }
  }

  static struct timespec a, b, c;
  clock_gettime(CLOCK_REALTIME, &a);
  gcMark(Root);
  clock_gettime(CLOCK_REALTIME, &b);
  gcSweep(full_gc);
  clock_gettime(CLOCK_REALTIME, &c);

#ifdef DEBUG
  if (full_gc) {
    printf("full marking took: %lu ms\n", getDiff(a, b) / 1000000);
    printf("sweeping took: %lu ms\n", getDiff(b, c) / 1000000);
    printMemoryStatistics();
  }
#endif
}

void teardownGc() {
  doGc(1);
  for (int i = 0; i < NUM_HEAP_SEGMENTS; i++) {
    ArenaHeader * arena = Heap.free_arena[i];
    while (arena != NULL) {
      ArenaHeader * next = arena->next;
      freeArena(arena);
      arena = next;
    }
    arena = Heap.full_arena[i];
    while (arena != NULL) {
      ArenaHeader * next = arena->next;
      freeArena(arena);
      arena = next;
    }
  }
}

void inspectArena(ArenaHeader * arena) {
  printf("Arena : %p = {\n", arena);
  printf("  object_size  : %d\n", getObjectSize(arena));
  printf("  object_bits  : %d\n", getObjectBits(arena));
  printf("  num_objects  : %d\n", getNumObjects(arena));
  printf("  mark_bits    : [");
  char * bm    = getBytemap(arena, 0);
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
  char * bm    = getBytemap(arena, 0);
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
      ArenaHeader * next = arena->next;
      space  += arena->raw_size;
      usable += getNumObjects(arena) * getObjectSize(arena);
      used   += arena->num_alloc * getObjectSize(arena);
      arena = next;
    }
    arena = Heap.full_arena[i];
    while (arena != NULL) {
      ArenaHeader * next = arena->next;
      space  += arena->raw_size;
      usable += getNumObjects(arena) * getObjectSize(arena);
      used   += arena->num_alloc * getObjectSize(arena);
      arena = next;
    }
  }
  printf("Reserverd: %lu mb | Usable: %lu mb | Used: %lu mb\n",
      space / 1048576, usable / 1048576, used / 1048576);
}


