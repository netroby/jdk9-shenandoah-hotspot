/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */

#include "gc_interface/collectedHeap.hpp"
#include "memory/universe.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"

BrooksPointer::BrooksPointer(uintptr_t* hw) : _heap_word(hw) {}

BrooksPointer BrooksPointer::get(oop obj) {
  return BrooksPointer(((uintptr_t*) obj) - 1);
}

void BrooksPointer::set_forwardee(oop forwardee) {
  assert(ShenandoahHeap::heap()->is_in(forwardee), "forwardee must be valid oop in the heap");
  *_heap_word = (uintptr_t) forwardee;
#ifdef ASSERT
  if (ShenandoahTraceBrooksPointers) {
    tty->print("setting_forwardee to %p = %p\n", forwardee, *_heap_word);
  }
#endif
}

HeapWord* BrooksPointer::cas_forwardee(HeapWord* old, HeapWord* forwardee) {
  assert(ShenandoahHeap::heap()->is_in(forwardee), "forwardee must point to a heap address");
  HeapWord* o = old;
  HeapWord* n = forwardee;

#ifdef ASSERT
  if (ShenandoahTraceBrooksPointers) {
    tty->print("Attempting to CAS %p value %p from %p to %p\n", _heap_word, *_heap_word, o, n);
  }
#endif

  HeapWord* result =  (HeapWord*) (HeapWord*) Atomic::cmpxchg_ptr(n, _heap_word, o);

#ifdef ASSERT
  if (ShenandoahTraceBrooksPointers) {
    tty->print("Result of CAS from %p to %p was %p read value was %p\n", o, n, result, *_heap_word);
  }
#endif

  return result;
}					 

bool BrooksPointer::check_forwardee_is_in_heap(oop forwardee) {
   return Universe::heap()->is_in(forwardee);
}
