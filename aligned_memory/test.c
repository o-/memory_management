#include "align.h"

#include <stdio.h>

int main(){
  alignedMemoryManagerInit();
  int chunkAlignment = memoryChunkAlignment();

  for (int i = chunkAlignment - 100;
       i < 2*chunkAlignment;
       i += 6987) {
    MemoryChunkHeader * chunk = allocateAligned(i);
    if (chunk == NULL) {
      printf("could not get chunk of size %d\n", i);
      continue;
    }

    char              * data  = (char*)chunkData(chunk);

    float   overalloc = 100.0 * (((float) chunk->length /
                                  (float) i) - 1.0);
    assert(overalloc < 1.0);
    assert(isAligned(chunk, chunkAlignment));

    printf("chunksize is %dk\n", chunkAlignment/1024);
    printf("requested %d bytes got %d (%f%% overallocation)\n",
           i, chunk->length, overalloc);
    printf("%p : chunk header\n", chunk);

    for (int i = 0; i < chunk->length; i++, data++) {
      assert(chunkFromPtr(data) == chunk || !isMaskable(data, chunk));
      assert((unsigned int)*data == 0);
      *data = 6;
      assert((unsigned int)*data == 6);
    }

    freeChunk(chunk);
  }
}
