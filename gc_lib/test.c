#include <stdlib.h>
#include <stdio.h>

#include "object.h"
#include "debugging.h"

static TestObject * Nil;
static TestObject * Root;

#ifndef BOEHM_GC

#include "gc.h"

TestObject * _alloc(int size) {
  return gcAlloc(size);
}

void gcMarkWrapper() {
  gcForward(Root);
  gcMark();
}

#else

#include <gc.h>
TestObject * _alloc(int size) {
  return GC_MALLOC(size);
}

void gcInit() {
  GC_INIT();
}
void gcTeardown() {}
void gcWriteBarrier(TestObject * a, TestObject * b) {}

#endif

void setSlot(TestObject * parent, int index, TestObject * child) {
  ((TestObject**)(parent+1))[index] = child;
  gcWriteBarrier(parent, child);
}

TestObject * getSlot(TestObject * parent, int index) {
  return ((TestObject**)(parent+1))[index];
}

TestObject * alloc(long len) {
  TestObject * t = _alloc(sizeof(TestObject) + sizeof(TestObject*) * len);
  for (long i = 0; i < len; i++) {
    setSlot(t, i, Nil);
  }
  t->length = len;
  return t;
}

void releaseSomeNodes(TestObject * o) {
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
               TestObject * parent,
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

  TestObject * o = alloc(length);
  setSlot(parent, child_idx, o);

  o->round = round;
  // Keep a backpointer in slot 0
  setSlot(o, 0, parent);

  for (int i = 1; i < length; i++) {
    setSlot(o, i, Nil);
    if (rand() % 20 > 10) {
      alloc(rand()%30);
    }
    if (rand() % 20 > 6) {
      allocTree(depth - 1, o, i, round);
    }
  }
}

void verifyTree(TestObject * node, TestObject * parent, long round) {
  if (node == Nil) return;

  assert(node->round == round);

  // Check the backpointer is still in place
  assert(getSlot(node, 0) == parent);

  for (int i = 1; i < node->length; i++) {
    verifyTree(getSlot(node, i), node, round);
  }
}

int main(){
  gcInit();

  srand(90);

  Nil = alloc(0);

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

  gcTeardown();
}
