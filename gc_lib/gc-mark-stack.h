#ifndef H_GC_MARK_STACK
#define H_GC_MARK_STACK

#include "object.h"

void resetMarkStack();
void markStackPush(ObjectHeader * o);
ObjectHeader * markStackPop();
int markStackEmpty();


#endif
