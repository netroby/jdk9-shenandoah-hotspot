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
  bool set_age(uint age);

  oop get_forwardee();
  void set_forwardee(oop forwardee);
  HeapWord* cas_forwardee(HeapWord* old, HeapWord* forwardee);

  static BrooksPointer get(oop obj);
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_BROOKSPOINTER_HPP
