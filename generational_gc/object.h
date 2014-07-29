#ifndef H_OBJECT
#define H_OBJECT

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

typedef struct ObjectHeader ObjectHeader;
struct ObjectHeader {
  unsigned int   some_header_bits : 31;
  unsigned int   old              : 1;
  size_t         length;
  ObjectHeader * attrib;
};

inline ObjectHeader ** getSlots(ObjectHeader * o) {
  return (ObjectHeader**)(o+1);
}

#endif
