#ifndef H_OBJECT
#define H_OBJECT

typedef struct TestObject {
  int  foobar;
  long length;
  char round;
} TestObject;

#define DO_CHILDREN(p, action, arg) \
  for(int i = 0; i < p->length; i++) { \
    action((((TestObject**)(p+1))[i]), arg); \
  }

#endif
