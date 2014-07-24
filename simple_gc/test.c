#include "gc.h"
#include "time.h"

#include <stdio.h>

static ObjectHeader * Nil;

void releaseSomeNodes(ObjectHeader * o) {
  if (o == Nil) return;
  ObjectHeader ** s = getSlots(o);
  for (int i = 0; i < o->length; i++) {
    if (rand()%10 == 1) {
      s[i] = Nil;
    } else {
      releaseSomeNodes(s[i]);
    }
  }
}

ObjectHeader * allocNode(int depth) {
  if (depth == 0) {
    return Nil;
  }
  int size          = rand() % 12;
  ObjectHeader *  o = alloc(size);
  ObjectHeader ** s = getSlots(o);
  for (int i = 0; i < size; i++) {
    s[i] = allocNode(depth - 1);
  }
  return o;
}

unsigned long countNode(ObjectHeader * root) {
  if (root == Nil) return 0;

  ObjectHeader **   s = getSlots(root);
  unsigned long count = root->length;

  for (int i = 0; i < root->length; i++) {
    count += countNode(s[i]);
  }

  return count;
}

unsigned long getDiff(struct timespec a, struct timespec b) {
  if (a.tv_sec == b.tv_sec) {
    return b.tv_nsec - a.tv_nsec;
  } else {
    return 1000000000L - a.tv_nsec + b.tv_nsec +
           (1000000000L * (b.tv_sec - a.tv_sec));
  }
}

int main(){
  initGc();

  srand(90);

  Nil = alloc(0);

  int rounds = 10;

  ObjectHeader *  root  = alloc(rounds);
  ObjectHeader ** roots = getSlots(root);
  for (int i = 0; i < rounds; i++) {
    roots[i] = Nil;
  }


  for (int i = 0; i < rounds; i++) {
    static struct timespec a, b, c;

    clock_gettime(CLOCK_REALTIME, &a);
    ObjectHeader * tree = allocNode(10);
    clock_gettime(CLOCK_REALTIME, &b);

    printf("allocation took: %lu ms\n", getDiff(a, b) / 1000000);
    printMemoryStatistics();

    roots[i] = tree;
    releaseSomeNodes(tree);

    clock_gettime(CLOCK_REALTIME, &a);
    gcMark(root);
    clock_gettime(CLOCK_REALTIME, &b);
    gcSweep();
    clock_gettime(CLOCK_REALTIME, &c);

    printf("marking took: %lu ms\n", getDiff(a, b) / 1000000);
    printf("sweeping took: %lu ms\n", getDiff(b, c) / 1000000);
    printMemoryStatistics();

    printf("---------\n");
  }

  teardownGc();
}
