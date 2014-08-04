#ifndef H_OBJECT
#define H_OBJECT

typedef struct ObjectHeader ObjectHeader;
struct ObjectHeader {
  unsigned int   old              : 1;     // Must be bit 0
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

#define barrier() __asm__ __volatile__("": : :"memory")

void deferredWriteBarrier(ObjectHeader * parent, ObjectHeader * child);
inline void writeBarrier(ObjectHeader * parent, ObjectHeader * child) {
  barrier();
  if (parent->old > child->old) {
    deferredWriteBarrier(parent, child);
  }
}

inline void setSlot(ObjectHeader * o, int i, ObjectHeader * c) {
  getSlots(o)[i] = c;
  writeBarrier(o, c);
}

#endif
