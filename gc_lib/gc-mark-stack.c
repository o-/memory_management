#include <stdlib.h>

#include "gc-mark-stack.h"

#include "debugging.h"

typedef struct StackChunk StackChunk;

#define StackChunkSize 490
struct StackChunk {
  StackChunk * prev;
  ObjectHeader * entry[StackChunkSize];
  int top;
};


#define __GC_STACK_SPACE_SIZE (1<<21)
typedef struct GcStackSpaceStruct {
  int alloc;
  char * free;
  char space[__GC_STACK_SPACE_SIZE];
} GcStackSpaceStruct;
static GcStackSpaceStruct GcStackSpace;


void * markStackSpaceMalloc(size_t size) {
  void * p = GcStackSpace.free;
  GcStackSpace.alloc += size;
  if (GcStackSpace.alloc > __GC_STACK_SPACE_SIZE) {
    puts("GC panic: no temp mem available");
    exit(-1);
  }
  GcStackSpace.free += size;
  return p;
}

void markStackSpaceClear() {
  GcStackSpace.free  = GcStackSpace.space;
  GcStackSpace.alloc = 0;
}

static StackChunk * MarkStack = NULL;

int stackEmpty(StackChunk * stack) {
  return stack->top == 0 && stack->prev == NULL;
}

StackChunk * allocStackChunk() {
  assert(sizeof(StackChunk) <= 4016);
  StackChunk * stack = markStackSpaceMalloc(sizeof(StackChunk));
  stack->top = 0;
  stack->prev = NULL;
  return stack;
}


void stackPush(StackChunk ** stack_, ObjectHeader * o) {
  StackChunk * stack = *stack_;
  if (stack->top == StackChunkSize) {
    *stack_ = allocStackChunk();
    (*stack_)->prev = stack;
    stack = *stack_;
  }
  stack->entry[stack->top++] = o;
}

ObjectHeader * stackPop(StackChunk ** stack_) {
  StackChunk * stack = *stack_;
  if (stack->top == 0) {
    if (stack->prev == NULL) {
      return NULL;
    }
    stack = *stack_ = stack->prev;
  }
  return stack->entry[--stack->top];
}

void markStackPush(ObjectHeader * o) {
  stackPush(&MarkStack, o);
}

ObjectHeader * markStackPop() {
  return stackPop(&MarkStack);
}

int markStackEmpty() {
  return stackEmpty(MarkStack);
}

void resetMarkStack() {
  markStackSpaceClear();
  MarkStack = allocStackChunk();
}


