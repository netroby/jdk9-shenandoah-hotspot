
#include "gc_interface/collectedHeap.hpp"
#include "memory/universe.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"

BrooksPointer::BrooksPointer(uintptr_t* hw) : heap_word(hw) {}

BrooksPointer BrooksPointer::get(oop obj) {
  return BrooksPointer(((uintptr_t*) obj) - 1);
}

uint BrooksPointer::get_age() {
  assert(! ShenandoahBarrierSet::is_brooks_ptr(oop((HeapWord*) heap_word + 1)), "can't get age of brooks pointers");
  assert(ShenandoahBarrierSet::is_brooks_ptr(oop((HeapWord*) heap_word - 3)), "must be brooks pointer oop");
  return (uint) (*heap_word & AGE_MASK);
}

void BrooksPointer::set_age(uint age) {

  *heap_word = (*heap_word & FORWARDEE_MASK) | (age & AGE_MASK);
}

oop BrooksPointer::get_forwardee() {
  oop forwardee = (oop) (*heap_word & FORWARDEE_MASK);
  assert(Universe::heap()->is_in(forwardee), "forwardee must be in heap");
  assert(forwardee->is_oop(), "forwardee must be valid oop");
  return forwardee;
}

void BrooksPointer::set_forwardee(oop forwardee) {
  assert(ShenandoahHeap::heap()->is_in(forwardee), "forwardee must be valid oop in the heap");
  *heap_word = (*heap_word & AGE_MASK) | ((uintptr_t) forwardee & FORWARDEE_MASK);
  //  tty->print("setting_forwardee to %p = %p\n", forwardee, *heap_word);
}

HeapWord* BrooksPointer::cas_forwardee(HeapWord* old, HeapWord* forwardee) {
  assert(ShenandoahHeap::heap()->is_in(forwardee), "forwardee must point to a heap address");
  HeapWord* o = (HeapWord*) ((*heap_word & AGE_MASK) | ((uintptr_t) old & FORWARDEE_MASK));
  HeapWord* n = (HeapWord*) ((*heap_word & AGE_MASK) | ((uintptr_t) forwardee & FORWARDEE_MASK));

#ifdef ASSERT
  if (ShenandoahGCVerbose) {
    tty->print("Attempting to CAS %p value %p from %p to %p\n", heap_word, *heap_word, o, n);
  }
#endif

  HeapWord* result =  (HeapWord*) ((uintptr_t) Atomic::cmpxchg_ptr(n, heap_word, o) & FORWARDEE_MASK);

#ifdef ASSERT
  if (ShenandoahGCVerbose) {
    tty->print("Result of CAS from %p to %p was %p read value was %p\n", o, n, result, *heap_word);
  }
#endif

  return result;
}					 
  
