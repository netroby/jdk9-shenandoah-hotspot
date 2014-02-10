/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */

#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_BROOKSPOINTER_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_BROOKSPOINTER_HPP

#include "utilities/globalDefinitions.hpp"
#include "oops/oop.hpp"

class BrooksPointer {

public:
  static const uint BROOKS_POINTER_OBJ_SIZE = 4;

private:

  uintptr_t* _heap_word;

  BrooksPointer(uintptr_t* heap_word);

public:

  static const uintptr_t AGE_MASK = 0x7;
  static const uintptr_t FORWARDEE_MASK = ~AGE_MASK;

  uint get_age();
  /*
   * Sets the age of the object. Return true if successful, false if another thread
   * was faster.
   */
  inline inline bool set_age(uint age) {
    uintptr_t old_brooks_ptr = *_heap_word;
    uint old_age = old_brooks_ptr & AGE_MASK;
    if (age == old_age) {
      // Object has already been marked.
      return false;
    }
    uintptr_t new_brooks_ptr = (old_brooks_ptr & FORWARDEE_MASK) | (age & AGE_MASK);
    uintptr_t other = (uintptr_t) Atomic::xchg_ptr((void*)new_brooks_ptr, (void*) _heap_word);
    assert(other == new_brooks_ptr || other == old_brooks_ptr, "can only be one of old or new value");
    return other == old_brooks_ptr;
  }

  inline oop get_forwardee() {
    oop forwardee = (oop) (*_heap_word & FORWARDEE_MASK);
    assert(Universe::heap()->is_in(forwardee), "forwardee must be in heap");
    assert(forwardee->is_oop(), "forwardee must be valid oop");
    return forwardee;
  }

  void set_forwardee(oop forwardee);
  HeapWord* cas_forwardee(HeapWord* old, HeapWord* forwardee);

  static BrooksPointer get(oop obj);
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_BROOKSPOINTER_HPP
