#include "arena.h"
#include "time.h"

#include <stdio.h>


typedef struct TestObj {
  void * a;
  double b;
} TestObj;

typedef struct TestObj2 {
  void * a;
  double b[15];
} TestObj2;


int main(){
  alignedMemoryManagerInit();

  int rounds = 10000;
  int as     = 10;
  ArenaHeader * arenas[as];
  int           count[as];

  srand(time(NULL));

  for (int a = 0; a < as; a++) {
    count[a] = 0;
    if (a < as/2) {
      arenas[a] = allocateAlignedArena(sizeof(TestObj));
    } else {
      arenas[a] = allocateAlignedArena(sizeof(TestObj2));
    }
  }

  for (int j = 0; j < rounds; j++) {
    for (int a = 0; a < as; a++) {
      ArenaHeader * arena = arenas[a];

      void * first = arena->first;

      for (int i = 0; i < 4000; i++) {
        int r = rand() % arena->num_objects;

        void * obj = NULL;

        if (a < as/2) {
          obj = ((TestObj*)first)+r;
        } else {
          obj = ((TestObj2*)first)+r;
        }
        assert(chunkFromPtr(obj) == arena);

        if (getMark(obj) == 0) {
          mark(obj, 1);
          count[a]++;
        } else {
          mark(obj, 0);
          count[a]--;
        }
      }
      if (j % 50 == 0) {
        assert(count[a] == getNumberOfMarkBits(arena));
      }
    }
  }

  for (int a = 0; a < as; a++) {
    ArenaHeader * arena = arenas[a];
    inspectArena(arena);
    freeArena(arena);
  }
}
