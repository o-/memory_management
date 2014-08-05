#ifndef H_OBJECT
#define H_OBJECT

typedef struct ObjectHeader ObjectHeader;
struct ObjectHeader {
  unsigned int   some_header_bits : 31;
  size_t         length;
  ObjectHeader * attrib;
};

#endif
