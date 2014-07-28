#include "gc.h"
#include "time.h"

#include <stdio.h>

void releaseSomeNodes(ObjectHeader * o) {
  if (o == Nil) return;
  ObjectHeader ** s = getSlots(o);
  for (int i = 1; i < o->length; i++) {
    if (s[i] != Nil && rand()%10 == 1) {
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
  int length;
  if (rand()%100000 == 1) {
    // Create large vectors once in a while
    length = 1 + rand() % 10000;
  } else if (rand()%10000 == 1) {
    length = 1 + rand() % 509;
  } else {
    length = 1 + rand() % 50;
  }
  ObjectHeader *  o = alloc(length);
  o->some_header_bits = round;
  ObjectHeader ** s = getSlots(o);
  // Keep a backpointer in slot 0
  s[0] = parent;
  writeBarrier(o, parent);
  for (int i = 1; i < length; i++) {
    ObjectHeader * c = allocTree(depth - 1, o, round);
    // Leak some sub-trees
    if (rand() % 20 > 10) {
      s[i] = c;
      writeBarrier(o, c);
    }
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

  int rounds = 10;
  int depth  = 5;

  ObjectHeader *  root  = alloc(rounds);
  ObjectHeader ** roots = getSlots(root);
  for (int i = 0; i < rounds; i++) {
    roots[i] = Nil;
  }

  for (int i = 0; i < rounds; i++) {
    static struct timespec a, b, c;

    clock_gettime(CLOCK_REALTIME, &a);
    ObjectHeader * tree = allocTree(depth, Nil, i);
    clock_gettime(CLOCK_REALTIME, &b);

    printf("allocation took: %lu ms\n", getDiff(a, b) / 1000000);
    printMemoryStatistics();

    roots[i] = tree;
    writeBarrier(root, tree);
    releaseSomeNodes(tree);

    clock_gettime(CLOCK_REALTIME, &a);
    gcMark(root);
    clock_gettime(CLOCK_REALTIME, &b);
    gcSweep();
    clock_gettime(CLOCK_REALTIME, &c);

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
