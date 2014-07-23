#include "align.h"

#include <stdio.h>

int main(){
  alignedMemoryManagerInit();

  printf("chunksize is %dk\n", chunkAlignment/1024);

  int start      = chunkAlignment - 100;
  int step       = 6987;
  int end        = 4*chunkAlignment;
  int num_chunks = (end-start) / step;

  MemoryChunkHeader * chunks[num_chunks];

  printf("Allocating some pages\n");
  for (int i = 0; i < num_chunks; i++) {
    int allocate = start + (i * step);
    chunks[i] = allocateAligned(allocate);

    MemoryChunkHeader * chunk = chunks[i];
    if (chunk == NULL) {
      printf("could not get chunk of size %d\n", i);
      continue;
    }

    char * data = (char*)chunkData(chunk);

    float overalloc = 100.0 * (((float) chunk->length /
                                (float) allocate) - 1.0);
    // printf("%p : requested %d bytes got %d (%f%% overallocation)\n",
    //        chunk, allocate, chunk->length, overalloc);

    assert(overalloc < 1.0);
    assert(isAligned(chunk, chunkAlignment));

    for (int j = 0; j < chunk->length; j++, data++) {
      assert(chunkFromPtr(data) == chunk || !isMaskable(data, chunk));
      assert((unsigned int)*data == 0);
      *data = i%128;
      assert((unsigned int)*data == i%128);
    }
  }

  printf("Freeing memory\n");
  for (int i = num_chunks-1; i >= 0; i--) {
    MemoryChunkHeader * chunk = chunks[i];
    char *              data  = (char*)chunkData(chunk);

    for (int j = 0; j < chunk->length; j++, data++) {
      assert((unsigned int)*data == i%128);
    }

    freeChunk(chunk);
  }
}
