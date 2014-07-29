#ifndef H_OBJECT
#define H_OBJECT

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
