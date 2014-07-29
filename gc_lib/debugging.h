#ifndef H_DEBUGGING
#define H_DEBUGGING

#include <stdio.h>

#ifdef DEBUG
#define assert(t) \
    if (!(t)) { \
          printf("assertion '%s' failed: %s:%d\n", #t, __FILE__, __LINE__); \
          __asm("int3"); \
          printf(" "); \
        }
#else
#define assert(t) ((void)0)
#endif

#ifdef DEBUG_PRINT
#define debug(str, ...) (printf(str, __VA_ARGS__))
#else
#define debug(str, ...) ((void)0)
#endif

#endif

