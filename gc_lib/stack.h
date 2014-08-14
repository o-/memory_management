#ifndef H_STACK
#define H_STACK

typedef struct StackChunk StackChunk;

#define StackChunkSize 490
struct StackChunk {
  StackChunk * prev;
  ObjectHeader * entry[StackChunkSize];
  int top;
};

inline int stackEmpty(StackChunk * stack) {
  return stack->top == 0 && stack->prev == NULL;
}

StackChunk * allocStackChunk() {
  assert(sizeof(StackChunk) <= 4016);
  StackChunk * stack = malloc(sizeof(StackChunk));
  stack->top = 0;
  stack->prev = NULL;
  return stack;
}


inline void stackPush(StackChunk ** stack_,
               ObjectHeader * o) {
  StackChunk * stack = *stack_;
  if (stack->top == StackChunkSize) {
    *stack_ = allocStackChunk();
    (*stack_)->prev = stack;
    stack = *stack_;
  }
  stack->entry[stack->top++] = o;
}

inline ObjectHeader * stackPop(StackChunk ** stack_) {
  StackChunk * stack = *stack_;
  if (stack->top == 0) {
    if (stack->prev == NULL) {
      return NULL;
    }
    *stack_ = stack->prev;
    free(stack);
    stack = *stack_;
  }
  return stack->entry[--stack->top];
}

void stackReset(StackChunk ** stack_) {
  StackChunk * stack = *stack_;
  if (stackEmpty(stack)) {
    return;
  }
  StackChunk * prev  = NULL;
  while(stack->prev != NULL) {
    prev = stack->prev;
    free(stack);
    stack = prev;
  }
  stack->top = 0;
  *stack_ = stack;
}

void stackFree(StackChunk * stack) {
  while(stack != NULL) {
    StackChunk * prev = stack->prev;
    free(stack);
    stack = prev;
  }
}

size_t stackSize(StackChunk * stack) {
  size_t size = 0;
  while(stack != NULL) {
    size += sizeof(StackChunk);
    stack = stack->prev; 
  }
  return size;
}

#endif
