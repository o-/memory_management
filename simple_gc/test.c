#include "gc.h"
#include "time.h"

#include <stdio.h>

extern ObjectHeader * Nil;


static ObjectHeader * SomeReleasedNode;

void releaseSomeNodes(ObjectHeader * o) {
  if (o == Nil) return;
  ObjectHeader ** s = getSlots(o);
  for (int i = 1; i < o->length; i++) {
    if (s[i] != Nil && rand()%10 == 1) {
      SomeReleasedNode = s[i];
      s[i] = Nil;
    } else {
      releaseSomeNodes(s[i]);
    }
  }
}

ObjectHeader * allocTree(int depth, ObjectHeader * parent, long round) {
  if (depth == 0) {
    return Nil;
  }
  int size          = 2 + rand() % 11;
  ObjectHeader *  o = alloc(size);
  o->some_header_bits = round;
  ObjectHeader ** s = getSlots(o);
  // Keep a backpointer in slot 0
  s[0] = parent;
  for (int i = 1; i < size; i++) {
    s[i] = allocTree(depth - 1, o, round);
  }
  return o;
}

void verifyTree(ObjectHeader * node, ObjectHeader * parent, long round) {
  if (node == Nil) return;

  assert(node->some_header_bits == round);

  ObjectHeader **   s = getSlots(node);
  // Check the backpointer is still in place
  assert(s[0] == parent);

  for (int i = 1; i < node->length; i++) {
    verifyTree(s[i], node, round);
  }
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

  // rounds+1 to keep one additional slot containing Nil
  ObjectHeader *  root  = alloc(rounds+1);
  ObjectHeader ** roots = getSlots(root);
  for (int i = 0; i < rounds+1; i++) {
    roots[i] = Nil;
  }

  for (int i = 0; i < rounds; i++) {
    static struct timespec a, b, c;

    clock_gettime(CLOCK_REALTIME, &a);
    ObjectHeader * tree = allocTree(10, Nil, i);
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

    // Check if the sample released node was properly collected
    ObjectHeader ** s = getSlots(SomeReleasedNode);
    assert(s[0] == GC_ZAP_POINTER);

    printf("marking took: %lu ms\n", getDiff(a, b) / 1000000);
    printf("sweeping took: %lu ms\n", getDiff(b, c) / 1000000);
    printMemoryStatistics();

    for (int j = 0; j < i; j++) {
      verifyTree(roots[j], Nil, j);
    }

    printf("---------\n");
  }

  teardownGc();
}
