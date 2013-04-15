#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"

void VM_ShenandoahInitMark::doit() {
  ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
  sh->concurrentMark()->markFromRoots();
}

void VM_ShenandoahFinal::doit() {
  ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
  sh->concurrentMark()->finishMarkFromRoots();
}
  
