/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */

#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_BROOKSPOINTER_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_BROOKSPOINTER_HPP

#include "oops/oop.hpp"
#include "utilities/globalDefinitions.hpp"

class BrooksPointer {

public:
  static const uint BROOKS_POINTER_OBJ_SIZE = 4;

private:

  uintptr_t* _heap_word;

  BrooksPointer(uintptr_t* heap_word);

public:

  bool check_forwardee_is_in_heap(oop forwardee);
  
  inline oop get_forwardee() {
    oop forwardee = (oop) (*_heap_word);
    assert(check_forwardee_is_in_heap(forwardee), "forwardee must be in heap");
    assert(forwardee->is_oop(), "forwardee must be valid oop");
    return forwardee;
  }

  void set_forwardee(oop forwardee);
  HeapWord* cas_forwardee(HeapWord* old, HeapWord* forwardee);

  static BrooksPointer get(oop obj);
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_BROOKSPOINTER_HPP
