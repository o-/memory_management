#ifndef H_STACK
#define H_STACK

typedef struct StackChunk StackChunk;

#define StackChunkSize 490
struct StackChunk {
  StackChunk * prev;
  ObjectHeader * entry[StackChunkSize];
  int top;
};

int stackEmpty(StackChunk * stack) {
  return stack->top == 0 && stack->prev == NULL;
}

StackChunk * allocStackChunk(void * (*allocator)(size_t)) {
  assert(sizeof(StackChunk) <= 4016);
  StackChunk * stack = (*allocator)(sizeof(StackChunk));
  stack->top = 0;
  stack->prev = NULL;
  return stack;
}


void stackPush(StackChunk ** stack_,
               ObjectHeader * o,
               void * (*allocator)(size_t)) {
  StackChunk * stack = *stack_;
  if (stack->top == StackChunkSize) {
    *stack_ = allocStackChunk(allocator);
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

#endif
