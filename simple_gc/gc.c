#include "gc.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <sys/mman.h>

extern ObjectHeader * Nil;


#define SlotSize sizeof(double)
#define EmptyObjectSize 32

const int arenaAlignment;
const int arenaAlignBits;
const int arenaAlignMask;

#define VariableHeapSegment 6
#define FixedHeapSegments   6
#define HeapSegments (VariableHeapSegment + 1)

static int HeapSegmentSize[FixedHeapSegments] = {EmptyObjectSize,
                                                 EmptyObjectSize<<1,
                                                 EmptyObjectSize<<2,
                                                 EmptyObjectSize<<3,
                                                 EmptyObjectSize<<4,
                                                 EmptyObjectSize<<5};

static int GcLookupNumObject[FixedHeapSegments];
static int GcLookupObjectSize[FixedHeapSegments];
static int GcLookupObjectBits[FixedHeapSegments];
static int GcLookupMaxLength[FixedHeapSegments];
#define GcLookupSegmentSize ((EmptyObjectSize<<5)/SlotSize)
static int GcLookupSegment[GcLookupSegmentSize];

static char whiteMark  = 0;
static char greyMark   = 1;
static char blackMark  = 2;

typedef struct Heap {
  ArenaHeader * free_arena[HeapSegments];
  ArenaHeader * full_arena[HeapSegments];
} Heap;


#define GC_SPACE_SIZE (1<<21)
typedef struct GcSpace {
  int alloc;
  char * free;
  char space[GC_SPACE_SIZE];
} GcSpace;
static GcSpace GC_SPACE;


void * gcSpaceMalloc(size_t size) {
  void * p = GC_SPACE.free;
  GC_SPACE.alloc += size;
  if (GC_SPACE.alloc > GC_SPACE_SIZE) {
    exit(-1);
  }
  GC_SPACE.free += size;
  return p;
}

void gcSpaceClear() {
  GC_SPACE.free  = GC_SPACE.space;
  GC_SPACE.alloc = 0;
}

static Heap HEAP;

#define _CHUNK_ALIGN_BITS 20
const int arenaAlignment = 1<<_CHUNK_ALIGN_BITS;
const int arenaSize      = 1<<_CHUNK_ALIGN_BITS;
const int arenaAlignBits = _CHUNK_ALIGN_BITS;
const int arenaAlignMask = (1<<_CHUNK_ALIGN_BITS)-1;

size_t roundUpMemory(size_t required, unsigned int align);

int objectSizeToLength(int size) {
  return (size - sizeof(ObjectHeader)) / SlotSize;
}

int objectLengthToSize(int length) {
  return (length * SlotSize) + sizeof(ObjectHeader);
}

extern inline int getFixedSegment(int length) {
  if (length > GcLookupSegmentSize) return -1;
  return GcLookupSegment[length];
}

extern inline int getNumObjects(ArenaHeader * arena) {
  assert(arena->segment >= 0);
  assert(arena->segment < FixedHeapSegments);
  return GcLookupNumObject[arena->segment];
}

extern inline int getObjectSize(ArenaHeader * arena) {
  assert(arena->segment >= 0);
  assert(arena->segment < FixedHeapSegments);
  return GcLookupObjectSize[arena->segment];
}

extern inline int getMaxObjectLength(ArenaHeader * arena) {
  assert(arena->segment >= 0);
  assert(arena->segment < FixedHeapSegments);
  return GcLookupMaxLength[arena->segment];
}

extern inline int getObjectBits(ArenaHeader * arena) {
  assert(arena->segment >= 0);
  assert(arena->segment < FixedHeapSegments);
  return GcLookupObjectBits[arena->segment];
}

extern inline int getBytemapIndex(void * base, ArenaHeader * arena) {
  return ((uintptr_t)base - (uintptr_t)arena->first) >> getObjectBits(arena);
}

extern inline char * getBytemap(ArenaHeader * base) {
  return (char*)(base + 1);
}

inline int isAligned(void * base, unsigned int align) {
  return (uintptr_t)base % align == 0;
}

inline int isMaskable(void * ptr, ArenaHeader * chunk) {
  return (uintptr_t)ptr < (uintptr_t)chunk+arenaAlignment;
}

extern inline ArenaHeader * chunkFromPtr(void * base) {
  return (ArenaHeader*)((uintptr_t)base & ~arenaAlignMask);
}

extern inline char * getMark(void * ptr, ArenaHeader * arena) {
  char *        bm    = getBytemap(arena);
  int           idx   = getBytemapIndex(ptr, arena);
  return bm + idx;
}

extern inline ObjectHeader ** getSlots(ObjectHeader * o);

size_t calcNumOfObjects(int total_size, int object_size) {
  assert(total_size > sizeof(ArenaHeader));

  // First estimate
  int usable       = total_size - sizeof(ArenaHeader);
  int bytemap_size = usable / object_size;
  usable          -= bytemap_size;

  // Increase as long as the bytemap fits
  assert(((float)usable / (float)object_size) < bytemap_size);
  while (((float)usable / (float)object_size) < bytemap_size) {
    bytemap_size--;
    usable++;
  }
  bytemap_size++;
  usable--;

  assert(usable > 0);

  int num_objects = usable/object_size;

  assert(bytemap_size >= num_objects);

  return (size_t)num_objects;
}

size_t roundUpMemory(size_t required, unsigned int align) {
  int diff = (required + align) % align;
  return required + align - diff;
}

uintptr_t nextAlignedAddress(uintptr_t base) {
  if ((base & ~arenaAlignMask) == base) return base;
  return (base+arenaAlignment) & ~arenaAlignMask;
}

ArenaHeader * allocateAlignedArena(int segment) {
  int OSPageAlignment = sysconf(_SC_PAGESIZE);

  int aligned_length = arenaAlignment;
  // Length aligned to OS page size (required for mmap)
  assert(aligned_length % OSPageAlignment == 0);
  // Virtual memory needed to satisfy chunk alignment
  size_t request_length = roundUpMemory(2 * aligned_length, OSPageAlignment);

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
  void * commited = mmap(aligned_base_ptr,
                         aligned_length,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                         -1,
                         0);
  if (commited == NULL) {
    munmap(aligned_base_ptr, aligned_length);
    return NULL;
  }

  assert(commited == aligned_base_ptr);
  ArenaHeader * chunk = (ArenaHeader*) aligned_base_ptr;

  chunk->segment           = segment;
  int num_objects          = getNumObjects(chunk);

  chunk->first             = (void*) ((uintptr_t)(chunk + 1) + num_objects);
  chunk->free              = chunk->first;
  chunk->free_list         = NULL;
  chunk->num_alloc         = 0;

  assert((getNumObjects(chunk)+1) * getObjectSize(chunk) +
         sizeof(ArenaHeader) <= aligned_length);

  // Zero bytemap
  memset(getBytemap(chunk), 0, num_objects);

  return chunk;
}

void freeArena(ArenaHeader * chunk) {
  munmap(chunk, arenaSize);
}

extern inline uintptr_t getArenaEnd(ArenaHeader * arena) {
  return (uintptr_t)arena->first + (getObjectSize(arena) * getNumObjects(arena));
}

ObjectHeader * allocFromArena(ArenaHeader * arena) {
  ObjectHeader * o = NULL;
  if (arena->free_list != NULL) {
    // Freelist allocation in recycled space
    o = (ObjectHeader*)arena->free_list;
    arena->free_list = arena->free_list->next;
    arena->num_alloc++;
  }
  if ((uintptr_t)arena->free < getArenaEnd(arena)) {
    // Bump pointer allocation in virgin space
    o = (ObjectHeader*)arena->free;
    arena->free = arena->free + getObjectSize(arena);
    arena->num_alloc++;
  }

  if (o != NULL) {
    ObjectHeader ** s = getSlots(o);
    // Initialize all slots to Nil
    for (int i = 0; i < getMaxObjectLength(arena); i++) {
      s[i] = Nil;
    }
  }

  return o;
}

ArenaHeader * newArena(int segment) {
  ArenaHeader * new_arena = allocateAlignedArena(segment);
  ArenaHeader * first     = HEAP.free_arena[segment];
  new_arena->next = first;
  HEAP.free_arena[segment] = new_arena;
  debug("allocating new arena %p in segment %d\n", new_arena, segment);
  return new_arena;
}

ObjectHeader * allocFromSegment(int segment) {
  ArenaHeader * arena = HEAP.free_arena[segment];
  while (1) {
    if (arena == NULL) {
      arena = newArena(segment);
      if (arena == NULL) return NULL;
    }
      debug("allocating object in arena %p\n", arena);

    ObjectHeader * o = allocFromArena(arena);
    if (o != NULL) {
      assert(chunkFromPtr(o) == arena);
      return o;
    }

    // This arena is full. Move to full_arena to not have to search it again
    ArenaHeader * next = arena->next;

    ArenaHeader * full = HEAP.full_arena[segment];
    arena->next = full;
    HEAP.full_arena[segment] = arena;

    HEAP.free_arena[segment] = next;
    arena = next;
  }
}

ObjectHeader * alloc(size_t length) {
  assert(length >= 0);

  int segment = getFixedSegment(length);
  if (segment == -1) {
    segment = VariableHeapSegment;
  }

  assert(segment >=0 &&
         (segment < FixedHeapSegments || segment == VariableHeapSegment));
  assert(segment == 0 ||
         (HeapSegmentSize[segment-1] < objectLengthToSize(length)));
  assert(segment >= FixedHeapSegments ||
         (HeapSegmentSize[segment] >= objectLengthToSize(length)));

  ObjectHeader * o = allocFromSegment(segment);
  if (o == NULL) return NULL;
  o->length = length;
  return o;
}

#define StackChunkSize 500
typedef struct StackChunk StackChunk;
struct StackChunk {
  StackChunk * prev;
  ObjectHeader * entry[StackChunkSize];
  int top;
};

extern inline StackChunk * allocStackChunk() {
  assert(sizeof(StackChunk) <= 4016);
  StackChunk * stack = gcSpaceMalloc(sizeof(StackChunk));
  stack->top = 0;
  stack->prev = NULL;
  return stack;
}

extern inline int stackEmpty(StackChunk * stack) {
  return stack->top == 0 && stack->prev == NULL;
}

extern inline void stackPush(StackChunk ** stack_, ObjectHeader * o) {
  StackChunk * stack = *stack_;
  if (stack->top == StackChunkSize) {
    *stack_ = allocStackChunk();
    (*stack_)->prev = stack;
    stack = *stack_;
  }
  stack->entry[stack->top++] = o;
}

extern inline ObjectHeader * stackPop(StackChunk ** stack_) {
  StackChunk * stack = *stack_;
  if (stack->top == 0) {
    if (stack->prev == NULL) {
      return NULL;
    }
    stack = *stack_ = stack->prev;
  }
  return stack->entry[--stack->top];
}

void gcMark(ObjectHeader * root) {
  StackChunk * stack = allocStackChunk();
  stackPush(&stack, root);

  while(!stackEmpty(stack)) {
    ObjectHeader * cur = stackPop(&stack);
    ArenaHeader * arena = chunkFromPtr(cur);
    char * mark = getMark(cur, arena);
    if (*mark != blackMark) {
      ObjectHeader ** children = getSlots(cur);
      for (int i = 0; i < cur->length; i++) {
        ObjectHeader * child = children[i];
        char * child_mark = getMark(child, chunkFromPtr(child));
        if (*child_mark == whiteMark) {
          *child_mark = greyMark;
          stackPush(&stack, child);
        }
      }
      *mark = blackMark;
    }
  }

  gcSpaceClear();
}

void sweepArena(ArenaHeader * arena) {
  char * finger = arena->first;
  arena->free_list = NULL;
  arena->num_alloc = 0;
  while ((uintptr_t)finger < getArenaEnd(arena) &&
         (uintptr_t)finger < (uintptr_t)arena->free) {
    char * mark = getMark(finger, arena);
    assert(*mark != greyMark);
    if (*mark == whiteMark) {
#ifdef DEBUG
      // Zap slots
      ObjectHeader * o  = (ObjectHeader*)finger;
      ObjectHeader ** s = getSlots(o);
      for (int i = 0; i < o->length; i++) {
        s[i] = GC_ZAP_POINTER;
      }
#endif
      FreeObject * f = (FreeObject*)finger;
      f->next = arena->free_list;
      arena->free_list = f;
    } else {
      *mark = whiteMark;
      arena->num_alloc++;
    }
    finger += getObjectSize(arena);
  }
}

void gcSweep() {
  for (int i = 0; i < HeapSegments; i++) {
    ArenaHeader * arena     = HEAP.free_arena[i];
    ArenaHeader * last_free = NULL;
    while (arena != NULL) {
      sweepArena(arena);
      if (arena->num_alloc == 0) {
        if (last_free != NULL) {
          last_free->next = arena->next;
        } else {
          HEAP.free_arena[i] = arena;
        }
        ArenaHeader * to_free = arena;
        arena = arena->next;
        freeArena(to_free);
      } else {
        last_free = arena;
        arena = arena->next;
      }
    }
    arena = HEAP.full_arena[i];
    ArenaHeader * prev_full = NULL;
    while (arena != NULL) {
      sweepArena(arena);
      float population = (float)arena->num_alloc / (float)getNumObjects(arena);
      // Move arenas with empty space to the free_arena list
      if (population < 0.98) {
        if (last_free != NULL) {
          last_free->next = arena;
        } else {
          HEAP.free_arena[i] = arena;
        }
        last_free = arena;
        if (prev_full == NULL) {
          HEAP.full_arena[i] = arena->next;
        } else {
          prev_full->next = arena->next;
        }
        ArenaHeader * next = arena->next;
        arena->next = NULL;
        arena = next;
      } else {
        prev_full = arena;
        arena = arena->next;
      }
    }
  }
}

void initGc() {
  gcSpaceClear();
  for (int i = 0; i < HeapSegments; HEAP.free_arena[i++] = NULL);
  for (int i = 0; i < HeapSegments; HEAP.full_arena[i++] = NULL);

  assert(1<<((int)log2(EmptyObjectSize)) == EmptyObjectSize);
  assert(EmptyObjectSize >= sizeof(ObjectHeader));
  assert(EmptyObjectSize <= (1<<(1+(int)log2(sizeof(ObjectHeader)))));

  assert(1<<arenaAlignBits == arenaAlignment);
  assert(arenaAlignment % sysconf(_SC_PAGESIZE) == 0);
  assert(arenaSize      % sysconf(_SC_PAGESIZE) == 0);
  assert(arenaSize <= arenaAlignment);

  // Build the various lookup tables
  for (int i = 0; i < FixedHeapSegments; i++) {
    int object_size = HeapSegmentSize[i];
    int object_bits = log2(object_size);
    assert (1<<object_bits == object_size);

    int num_objects = calcNumOfObjects(arenaSize, object_size);
    int max_length  = objectSizeToLength(object_size);

    GcLookupNumObject[i]  = num_objects;
    GcLookupMaxLength[i]  = max_length;
    GcLookupObjectSize[i] = object_size;
    GcLookupObjectBits[i] = object_bits;
  }

  int segment = 0;
  for (int i = 0; i < GcLookupSegmentSize; i++) {
    int size = objectLengthToSize(i);
    if (HeapSegmentSize[segment] < size) {
      segment++;
    }
    GcLookupSegment[i] = segment < FixedHeapSegments ? segment : -1;

    assert(!(size < HeapSegmentSize[FixedHeapSegments] &&
             GcLookupSegment[i] == -1));
    assert(GcLookupSegment[i] == -1 ||
           (HeapSegmentSize[segment] >= size &&
            (segment == 0 || HeapSegmentSize[segment-1] < size)));
  }
}

void teardownGc() {
  for (int i = 0; i < HeapSegments; i++) {
    ArenaHeader * arena = HEAP.free_arena[i];
    while (arena != NULL) {
      ArenaHeader * next = arena->next;
      freeArena(arena);
      arena = next;
    }
    arena = HEAP.full_arena[i];
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
  unsigned long space  = GC_SPACE_SIZE;
  unsigned long usable = 0;
  unsigned long used   = 0;
  for (int i = 0; i < HeapSegments; i++) {
    ArenaHeader * arena = HEAP.free_arena[i];
    while (arena != NULL) {
      ArenaHeader * next = arena->next;
      space  += arenaSize;
      usable += getNumObjects(arena) * getObjectSize(arena);
      used   += arena->num_alloc * getObjectSize(arena);
      arena = next;
    }
    arena = HEAP.full_arena[i];
    while (arena != NULL) {
      ArenaHeader * next = arena->next;
      space  += arenaSize;
      usable += getNumObjects(arena) * getObjectSize(arena);
      used   += arena->num_alloc * getObjectSize(arena);
      arena = next;
    }
  }
  printf("Reserverd: %lu mb | Usable: %lu mb | Used: %lu mb\n",
      space / 1048576, usable / 1048576, used / 1048576);
}


