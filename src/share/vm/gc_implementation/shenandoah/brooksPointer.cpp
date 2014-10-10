/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */

#include "gc_interface/collectedHeap.hpp"
#include "memory/universe.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"

BrooksPointer::BrooksPointer(HeapWord** hw) : _heap_word(hw) {}

BrooksPointer BrooksPointer::get(oop obj) {
  HeapWord* hw_obj = (HeapWord*) obj;
  HeapWord* brooks_ptr = hw_obj - 1;
  // We know that the value in that memory location is a pointer to another
  // heapword/oop.
  return BrooksPointer((HeapWord**) brooks_ptr);
}

void BrooksPointer::set_forwardee(oop forwardee) {
  assert(ShenandoahHeap::heap()->is_in(forwardee), "forwardee must be valid oop in the heap");
  *_heap_word = (HeapWord*) forwardee;
#ifdef ASSERT
  if (ShenandoahTraceBrooksPointers) {
    tty->print_cr("setting_forwardee to "PTR_FORMAT" = "PTR_FORMAT, p2i((HeapWord*) forwardee), p2i(*_heap_word));
  }
#endif
}

HeapWord* BrooksPointer::cas_forwardee(HeapWord* old, HeapWord* forwardee) {
  assert(ShenandoahHeap::heap()->is_in(forwardee), "forwardee must point to a heap address");
  


  HeapWord* o = old;
  HeapWord* n = forwardee;
  HeapWord* result;

#ifdef ASSERT
  if (ShenandoahTraceBrooksPointers) {
    tty->print_cr("Attempting to CAS "PTR_FORMAT" value "PTR_FORMAT" from "PTR_FORMAT" to "PTR_FORMAT, p2i(_heap_word), p2i(*_heap_word), p2i(o), p2i(n));
  }
#endif

#ifdef ASSERT  
  if (ShenandoahVerifyWritesToFromSpace || ShenandoahVerifyReadsToFromSpace) {
    ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
    ShenandoahHeapRegion* hr = sh->heap_region_containing(old);

    {
      hr->memProtectionOff();
      result =  (HeapWord*) (HeapWord*) Atomic::cmpxchg_ptr(n, _heap_word, o);
      hr->memProtectionOn();
    }
  } else {
    result =  (HeapWord*) (HeapWord*) Atomic::cmpxchg_ptr(n, _heap_word, o);
  }
#else 
  result =  (HeapWord*) (HeapWord*) Atomic::cmpxchg_ptr(n, _heap_word, o);
#endif
  
#ifdef ASSERT
  if (ShenandoahTraceBrooksPointers) {
    tty->print_cr("Result of CAS from "PTR_FORMAT" to "PTR_FORMAT" was "PTR_FORMAT" read value was "PTR_FORMAT, p2i(o), p2i(n), p2i(result), p2i(*_heap_word));
  }
#endif

  return result;
}					 

bool BrooksPointer::check_forwardee_is_in_heap(oop forwardee) {
   return Universe::heap()->is_in(forwardee);
}
