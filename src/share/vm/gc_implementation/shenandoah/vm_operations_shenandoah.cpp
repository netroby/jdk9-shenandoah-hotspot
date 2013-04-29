#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"

void VM_ShenandoahInitMark::doit() {
  ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
  sh->start_concurrent_marking();
}

void VM_ShenandoahFinishMark::doit() {
  ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
  sh->stop_concurrent_marking();
  sh->concurrentMark()->finishMarkFromRoots();
}
  
