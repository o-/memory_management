#include "gc.h"
#include "time.h"

#include <stdio.h>


ObjectHeader * allocNode(int depth) {
  if (depth == 0) {
    return NULL;
  }
  int size          = rand() % 12;
  ObjectHeader *  o = alloc(size);
  ObjectHeader ** s = (ObjectHeader**)(o+1);
  for (int i = 0; i < size; i++) {
    s[i] = allocNode(depth - 1);
  }
  return o;
}

unsigned long countNode(ObjectHeader * root) {
  if (root == NULL) return 0;

  ObjectHeader **   s = (ObjectHeader**)(root+1);
  unsigned long count = root->length;

  for (int i = 0; i < root->length; i++) {
    count += countNode(s[i]);
  }

  return count;
}

int main(){
  initGc();

  srand(time(NULL));

  ObjectHeader * root = allocNode(11);
  printf("%lu\n" , countNode(root));


  teardownGc();
}
