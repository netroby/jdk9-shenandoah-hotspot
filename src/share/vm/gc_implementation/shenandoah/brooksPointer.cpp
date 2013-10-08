
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
  *heap_word = (*heap_word & AGE_MASK) | ((uintptr_t) forwardee & FORWARDEE_MASK);
}
