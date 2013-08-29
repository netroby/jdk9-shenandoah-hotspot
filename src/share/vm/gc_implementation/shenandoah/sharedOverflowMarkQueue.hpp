#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHAREDOVERFLOWMARKQUEUE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHAREDOVERFLOWMARKQUEUE_HPP

#include "oops/oop.inline.hpp"

class SOMQItem : public CHeapObj<mtGC> {
public:
  oop _obj;
  SOMQItem* _next;
};

class SharedOverflowMarkQueue : public CHeapObj<mtGC> {

private:
  SOMQItem* _first;
  Mutex _mutex;

public:
  SharedOverflowMarkQueue();
  void push(oop o);
  oop pop();
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHAREDOVERFLOWMARKQUEUE_HPP
