#ifndef H_OBJECT
#define H_OBJECT

typedef struct ObjectHeader ObjectHeader;
struct ObjectHeader {
  unsigned int   some_header_bits : 31;
  size_t         length;
  ObjectHeader * attrib;
};

inline ObjectHeader ** getSlots(ObjectHeader * o) {
  return (ObjectHeader**)(o+1);
}

inline ObjectHeader * getSlot(ObjectHeader * o, int i) {
  return getSlots(o)[i];
}

#endif
