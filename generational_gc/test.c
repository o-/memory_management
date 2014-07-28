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

void allocTree(int depth,
               ObjectHeader * parent,
               ObjectHeader ** child,
               long round) {
  if (depth == 0) {
    *child = Nil;
    return;
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

  ObjectHeader * o = NULL;
  *child = alloc(length);
  o = *child;
  writeBarrier(parent, o);

  o->some_header_bits = round;
  ObjectHeader ** s = getSlots(o);
  // Keep a backpointer in slot 0
  s[0] = parent;
  writeBarrier(o, parent);

  for (int i = 1; i < length; i++) {
    if (rand() % 20 > 10) {
      alloc(length);
    }
    if (rand() % 20 > 7) {
      allocTree(depth - 1, o, &s[i], round);
    }
  }
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

int main(){
  initGc();

  srand(90);

  int rounds = 10;
  int depth  = 5;

  Root  = alloc(rounds);
  ObjectHeader ** roots = getSlots(Root);
  for (int i = 0; i < rounds; i++) {
    roots[i] = Nil;
  }

  for (int i = 0; i < rounds; i++) {
    allocTree(depth, Nil, &roots[i], i);

    releaseSomeNodes(roots[i]);

    for (int j = 0; j < i; j++) {
      verifyTree(roots[j], Nil, j);
    }

    printf("---------\n");
  }

  teardownGc();
}
