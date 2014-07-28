#include "gc.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <sys/mman.h>

ObjectHeader * Nil;

#define SlotSize sizeof(void*)
#define EmptyObjectSize 32

#define VariableHeapSegment 7
#define FixedHeapSegments   7
#define HeapSegments (VariableHeapSegment + 1)

static int HeapSegmentSize[FixedHeapSegments] = {EmptyObjectSize,
                                                 EmptyObjectSize<<1,
                                                 EmptyObjectSize<<2,
                                                 EmptyObjectSize<<3,
                                                 EmptyObjectSize<<4,
                                                 EmptyObjectSize<<5,
                                                 EmptyObjectSize<<6};

#define GcLookupSegmentSize ((EmptyObjectSize<<6)/SlotSize)
static int GcLookupNumObject[FixedHeapSegments];
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
    puts("GC panic: no temp mem available");
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

typedef struct FreeObject FreeObject;
struct FreeObject {
  FreeObject * next;
};

struct ArenaHeader {
  ArenaHeader * next;
  unsigned char segment;
  unsigned char object_bits;
  unsigned int  num_alloc;
  size_t        object_size;
  unsigned int  num_objects;
  size_t        raw_size;
  void *        first;
  void *        free;
  FreeObject *  free_list;
};

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

int getFixedSegment(int length) {
  if (length >= GcLookupSegmentSize) return -1;
  return GcLookupSegment[length];
}

int getNumObjects(ArenaHeader * arena) {
  return arena->num_objects;
}

int getObjectSize(ArenaHeader * arena) {
  return arena->object_size;
}

int getObjectSizeFromSegment(int segment) {
  return HeapSegmentSize[segment];
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

char * getBytemap(ArenaHeader * base, int generation) {
  return ((char*)(base + 1)) + (base->num_objects * generation);
}

inline int isAligned(void * base, unsigned int align) {
  return (uintptr_t)base % align == 0;
}

inline int isMaskable(void * ptr, ArenaHeader * chunk) {
  return (uintptr_t)ptr < (uintptr_t)chunk+arenaAlignment;
}

ArenaHeader * chunkFromPtr(void * base) {
  return (ArenaHeader*)((uintptr_t)base & ~arenaAlignMask);
}

char * getMark(void * ptr, ArenaHeader * arena, int generation) {
  char *        bm    = getBytemap(arena, generation);
  int           idx   = getBytemapIndex(ptr, arena);
  return bm + idx;
}

extern inline ObjectHeader ** getSlots(ObjectHeader * o);

size_t calcNumOfObjects(int total_size, int object_size) {
  assert(total_size > sizeof(ArenaHeader));

  // First estimate
  int usable       = total_size - sizeof(ArenaHeader);
  int bytemap_size = 2 * (usable / object_size);
  usable          -= bytemap_size;

  // Increase as long as the bytemap fits
  while (2 * ((float)usable / (float)object_size) < bytemap_size) {
    bytemap_size--;
    usable++;
  }
  bytemap_size++;
  usable--;

  assert(usable > 0);

  int num_objects = usable/object_size;

  assert(bytemap_size * 2 >= num_objects);

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

  ((ArenaHeader*)commited)->raw_size = aligned_length;
  return commited;
}

uintptr_t getArenaEnd(ArenaHeader * arena) {
  return (uintptr_t)arena->first + (getObjectSize(arena)*getNumObjects(arena));
}


ArenaHeader * allocateAlignedArena(int segment) {
  ArenaHeader * chunk = NULL;
  assert (segment < FixedHeapSegments);

  chunk = allocateAligned(arenaSize);

  chunk->segment           = segment;
  int num_objects          = GcLookupNumObject[segment];
  chunk->num_objects       = num_objects;

  chunk->first             = &getBytemap(chunk, 1)[num_objects];
  chunk->free              = chunk->first;
  chunk->free_list         = NULL;
  chunk->num_alloc         = 0;
  chunk->object_size       = HeapSegmentSize[segment];
  chunk->object_bits       = log2(HeapSegmentSize[segment]);

  assert(num_objects * chunk->object_size +
         sizeof(ArenaHeader) <= arenaSize);

  // Zero bytemap
  memset(getBytemap(chunk,0), 0, 2*num_objects);

  return chunk;
}

void freeArena(ArenaHeader * arena) {
#ifdef USE_POSIX_MEMALIGN
  free(arena);
#else
  munmap(arena, arena->raw_size);
#endif
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

ObjectHeader * allocFromSegment(int segment, int variable_lengt) {
  if (segment == VariableHeapSegment) {
    int object_size = objectLengthToSize(variable_lengt);
    ArenaHeader * arena = allocateAligned(object_size+ 1 + sizeof(ArenaHeader));

    arena->num_alloc         = 1;
    arena->segment           = segment;
    arena->num_objects       = 1;
    arena->object_size       = object_size;
    arena->object_bits       = arenaAlignBits;
    arena->free_list         = NULL;
    arena->first             = &getBytemap(arena, 1)[1];
    arena->free              = (void*)getArenaEnd(arena);

    ArenaHeader * first     = HEAP.full_arena[segment];
    arena->next = first;
    HEAP.full_arena[segment] = arena;

    // Zero bytemap
    memset(getBytemap(arena, 0), 0, 2);

    return arena->first;
  }

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

  ObjectHeader * o = allocFromSegment(segment, length);

 if (o == NULL) return NULL;

  ObjectHeader ** s = getSlots(o);
  // Initialize all slots to Nil
  for (int i = 0; i < length; i++) {
    s[i] = Nil;
  }

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

StackChunk * allocStackChunk() {
  assert(sizeof(StackChunk) <= 4016);
  StackChunk * stack = gcSpaceMalloc(sizeof(StackChunk));
  stack->top = 0;
  stack->prev = NULL;
  return stack;
}

int stackEmpty(StackChunk * stack) {
  return stack->top == 0 && stack->prev == NULL;
}

void stackPush(StackChunk ** stack_, ObjectHeader * o) {
  StackChunk * stack = *stack_;
  if (stack->top == StackChunkSize) {
    *stack_ = allocStackChunk();
    (*stack_)->prev = stack;
    stack = *stack_;
  }
  stack->entry[stack->top++] = o;
}

ObjectHeader * stackPop(StackChunk ** stack_) {
  StackChunk * stack = *stack_;
  if (stack->top == 0) {
    if (stack->prev == NULL) {
      return NULL;
    }
    stack = *stack_ = stack->prev;
  }
  return stack->entry[--stack->top];
}

static StackChunk * markStack = NULL;

void gcMark(ObjectHeader * root) {
  *getMark(Nil, chunkFromPtr(Nil), 0)   = greyMark;
  *getMark(root, chunkFromPtr(root), 0) = greyMark;
  stackPush(&markStack, Nil);
  stackPush(&markStack, root);

  while(!stackEmpty(markStack)) {
    ObjectHeader * cur = stackPop(&markStack);
    long length        = cur->length;
    ArenaHeader * arena = chunkFromPtr(cur);
    char * mark = getMark(cur, arena, 0);
#ifdef DEBUG
    ObjectHeader ** children = getSlots(cur);
    assert(*mark != whiteMark);
    if (*mark == blackMark) {
      for (int i = 0; i < length; i++) {
        ObjectHeader * child = children[i];
        assert(*getMark(child, chunkFromPtr(child), 0) != whiteMark);
      }
    }
#endif
    if (*mark != blackMark) {
      ObjectHeader ** children        = getSlots(cur);
      for (int i = 0; i < length; i++) {
        ObjectHeader * child = children[i];
        char * child_mark = getMark(child, chunkFromPtr(child), 0);
        if (*child_mark == whiteMark) {
          *child_mark = greyMark;
          stackPush(&markStack, child);
        }
      }
      *mark = blackMark;
    }
  }

  gcSpaceClear();
  markStack = allocStackChunk();
}

void writeBarrier(ObjectHeader * parent, ObjectHeader * child) {
  char * p_mark = getMark(parent, chunkFromPtr(parent), 0);
  char * c_mark = getMark(child,  chunkFromPtr(child), 0);
  if (*p_mark != whiteMark && *c_mark == whiteMark) {
    *p_mark = greyMark;
    stackPush(&markStack, parent);
  }
}

void sweepArena(ArenaHeader * arena) {
  char * finger = arena->first;
  arena->free_list = NULL;
  arena->num_alloc = 0;
  FreeObject * free_list = NULL;
  for (int i = 0;
      i < arena->num_objects && (uintptr_t)finger < (uintptr_t)arena->free;
      i++) {
    char * mark     = &getBytemap(arena, 0)[i];
    char * old_mark = &getBytemap(arena, 1)[i];
    assert((void*)mark < arena->first && (void*)old_mark < arena->first);
    assert(*mark != greyMark);
    if (*mark == whiteMark) {
      if (!*old_mark == whiteMark) {
        *old_mark = whiteMark;
      }
#ifdef DEBUG
      // Zap slots
      ObjectHeader * o  = (ObjectHeader*)finger;
      ObjectHeader ** s = getSlots(o);
      for (int i = 0; i < o->length; i++) {
        s[i] = GC_ZAP_POINTER;
      }
#endif
      FreeObject * f = (FreeObject*)finger;
      if (free_list == NULL) {
        free_list = f;
        arena->free_list = f;
      } else {
        free_list->next = f;
        free_list = f;
      }
    } else {
      if (*old_mark == whiteMark) {
        *mark     = whiteMark;
        *old_mark = blackMark;
      } else {
        *mark = blackMark;
      }
      arena->num_alloc++;
    }
    finger += getObjectSize(arena);
  }
  if (free_list != NULL) {
    free_list->next = NULL;
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
          HEAP.free_arena[i] = arena->next;
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
    assert (1<<(int)log2(object_size) == object_size);

    int num_objects = calcNumOfObjects(arenaSize, object_size);

    GcLookupNumObject[i]  = num_objects;
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

  Nil = alloc(0);
  markStack = allocStackChunk();
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
  unsigned long space  = GC_SPACE_SIZE;
  unsigned long usable = 0;
  unsigned long used   = 0;
  for (int i = 0; i < HeapSegments; i++) {
    ArenaHeader * arena = HEAP.free_arena[i];
    while (arena != NULL) {
      ArenaHeader * next = arena->next;
      space  += arena->raw_size;
      usable += getNumObjects(arena) * getObjectSize(arena);
      used   += arena->num_alloc * getObjectSize(arena);
      arena = next;
    }
    arena = HEAP.full_arena[i];
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


