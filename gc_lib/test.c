#include <stdlib.h>
#include <stdio.h>

#include "object.h"
#include "debugging.h"

#ifdef BOEHM_GC

#include <gc.h>
ObjectHeader * alloc(int len) {
  return GC_MALLOC(sizeof(ObjectHeader) + sizeof(ObjectHeader*) * len);
}
#define Nil NULL

void initGc() {
  GC_INIT();
}
void teardownGc() {}
#define writeBarrier(a, b) ((void*)0)
void deferredWriteBarrier(ObjectHeader * a, ObjectHeader * b) {}
ObjectHeader * Root;

#else

#include "gc.h"

#endif

void releaseSomeNodes(ObjectHeader * o) {
  if (o == Nil) return;
  for (int i = 1; i < o->length; i++) {
    if (getSlot(o, i) != Nil && rand()%10 == 1) {
      setSlot(o, i, Nil);
    } else {
      releaseSomeNodes(getSlot(o, i));
    }
  }
}

void allocTree(int depth,
               ObjectHeader * parent,
               int child_idx,
               long round) {
  if (depth == 0) {
    setSlot(parent, child_idx, Nil);
    return;
  }
  int length;
  if (rand()%1000000 == 1) {
    // Create large vectors once in a while
    length = 1 + rand() % 100000;
  } else if (rand()%10000 == 1) {
    length = 1 + rand() % 600;
  } else {
    length = 1 + rand() % 57;
  }

  ObjectHeader * o = alloc(length);
  setSlot(parent, child_idx, o);

  o->some_header_bits = round;
  // Keep a backpointer in slot 0
  setSlot(o, 0, parent);

  for (int i = 1; i < length; i++) {
    if (rand() % 20 > 5) {
      alloc(length);
    }
    if (rand() % 20 > 6) {
      allocTree(depth - 1, o, i, round);
    }
  }
}

void verifyTree(ObjectHeader * node, ObjectHeader * parent, long round) {
  if (node == Nil) return;

  assert(node->some_header_bits == round);

  // Check the backpointer is still in place
  assert(getSlot(node, 0) == parent);

  for (int i = 1; i < node->length; i++) {
    verifyTree(getSlot(node, i), node, round);
  }
}

int main(){
  initGc();

  srand(90);

  int rounds = 10;
  int depth  = 5;

  Root  = alloc(rounds);
  for (int i = 0; i < rounds; i++) {
    setSlot(Root, i, Nil);
  }

  for (int i = 0; i < rounds; i++) {
    allocTree(depth, Root, i, i);

    releaseSomeNodes(getSlot(Root, i));

    for (int j = 0; j < i; j++) {
      verifyTree(getSlot(Root, j), Root, j);
    }

    printf("---------\n");
  }

  teardownGc();
}
