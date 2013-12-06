#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"

void VM_ShenandoahInitMark::doit() {
  ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
  if (ShenandoahGCVerbose)
    tty->print("vm_ShenandoahInitMark\n");
  sh->start_concurrent_marking();
}

void VM_ShenandoahFinishMark::doit() {
  ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
  sh->concurrentMark()->finishMarkFromRoots();
  sh->stop_concurrent_marking();
  sh->prepare_for_concurrent_evacuation();

}
  
void VM_ShenandoahVerifyHeapAfterEvacuation::doit() {

  ShenandoahHeap *sh = ShenandoahHeap::heap();
  sh->verify_heap_after_evacuation();

}

void VM_ShenandoahGrowHeap::doit() {

  ShenandoahHeap *sh = ShenandoahHeap::heap();
  sh->grow_heap_by_impl();
}
