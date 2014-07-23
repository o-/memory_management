#include "align.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>

#include <sys/mman.h>

static int OSPageAlignment;

#define _CHUNK_ALIGN_BITS 20
const int chunkAlignment = 1<<_CHUNK_ALIGN_BITS;
const int chunkAlignBits = _CHUNK_ALIGN_BITS;
const int chunkAlignMask = (1<<_CHUNK_ALIGN_BITS)-1;

size_t roundUpMemory(size_t required, unsigned int align);

extern inline int isAligned(void * base, unsigned int align);
extern inline int isMaskable(void * ptr, MemoryChunkHeader * chunk);
extern inline MemoryChunkHeader * chunkFromPtr(void * base);

void alignedMemoryManagerInit() {
  OSPageAlignment = sysconf(_SC_PAGESIZE);
  assert(1<<chunkAlignBits == chunkAlignment);
  assert(chunkAlignment % OSPageAlignment == 0);
}

void * chunkData(MemoryChunkHeader * base) {
  return (void*)(base + 1);
}

size_t usableChunkSize(unsigned total_size) {
  assert(total_size > sizeof(MemoryChunkHeader));

  return total_size - sizeof(MemoryChunkHeader);
}

size_t totalChunkSize(size_t length) {
  return length + sizeof(MemoryChunkHeader);
}

size_t roundUpMemory(size_t required, unsigned int align) {
  int diff = (required + align) % align;
  return required + align - diff;
}

uintptr_t nextAlignedAddress(uintptr_t base) {
  if ((base & ~chunkAlignMask) == base) return base;
  return (base+chunkAlignment) & ~chunkAlignMask;
}

#ifdef USE_POSIX_MEMALIGN

MemoryChunkHeader * allocateAligned(size_t usable_length) {
  // Space requirements with header
  size_t full_length    = totalChunkSize(usable_length);

  void * base = NULL;
  posix_memalign(&base, chunkAlignment, full_length);

  if (base == NULL) return NULL;

  MemoryChunkHeader * chunk = (MemoryChunkHeader*) base;

  chunk->length   = usable_length;

  return chunk;
}

void freeChunk(MemoryChunkHeader * chunk) {
  free(chunk);
}

#else

MemoryChunkHeader * allocateAligned(size_t min_usable_length) {
  // Space requirements with header
  size_t full_length    = totalChunkSize(min_usable_length);
  // Length aligned to OS page size (required for mmap)
  size_t aligned_length = roundUpMemory(full_length, OSPageAlignment);
  // Resulting space available to the user
  size_t usable_length  = usableChunkSize(aligned_length);
  // Virtual memory needed to satisfy chunk alignment
  size_t request_length = roundUpMemory(aligned_length + chunkAlignment,
                                        OSPageAlignment);

  assert(usable_length >= min_usable_length);

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
  MemoryChunkHeader * chunk = (MemoryChunkHeader*) aligned_base_ptr;

  chunk->length   = usable_length;
  chunk->raw_size = aligned_length;

  return chunk;
}

void freeChunk(MemoryChunkHeader * chunk) {
  munmap(chunk, chunk->raw_size);
}

#endif

MemoryChunkHeader * allocateAlignedChunk() {
  return allocateAligned(usableChunkSize(chunkAlignment));
}
