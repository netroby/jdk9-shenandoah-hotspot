/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"

VM_Operation::VMOp_Type VM_ShenandoahInitMark::type() const {
  return VMOp_ShenandoahInitMark;
}

const char* VM_ShenandoahInitMark::name() const {
  return "Shenandoah Initial Marking";
}

void VM_ShenandoahInitMark::doit() {
  ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
  sh->shenandoahPolicy()->record_init_mark_start();
  if (ShenandoahGCVerbose)
    tty->print("vm_ShenandoahInitMark\n");
  sh->start_concurrent_marking();
  sh->shenandoahPolicy()->record_init_mark_end();
}

VM_Operation::VMOp_Type VM_ShenandoahFinishMark::type() const {
  return VMOp_ShenandoahFinishMark;
}

const char* VM_ShenandoahFinishMark::name() const {
  return "Shenandoah Finish Mark";
}

void VM_ShenandoahFinishMark::doit() {
  ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
  if (ShenandoahGCVerbose)
    tty->print("vm_ShenandoahFinalMark\n");

  sh->shenandoahPolicy()->record_final_mark_start();  
  sh->concurrentMark()->finishMarkFromRoots();
  sh->stop_concurrent_marking();
  sh->prepare_for_concurrent_evacuation();
  sh->set_evacuation_in_progress(true);

  sh->shenandoahPolicy()->record_final_mark_end();    

  if (! ShenandoahConcurrentEvacuation) {
    sh->do_evacuation();
  }
}

VM_Operation::VMOp_Type VM_ShenandoahVerifyHeapAfterEvacuation::type() const {
  return VMOp_ShenandoahVerifyHeapAfterEvacuation;
}

const char* VM_ShenandoahVerifyHeapAfterEvacuation::name() const {
  return "Shenandoah verify heap after evacuation";
}

void VM_ShenandoahVerifyHeapAfterEvacuation::doit() {

  ShenandoahHeap *sh = ShenandoahHeap::heap();
  sh->verify_heap_after_evacuation();

}

VM_Operation::VMOp_Type VM_ShenandoahEvacuation::type() const {
  return VMOp_ShenandoahEvacuation;
}

const char* VM_ShenandoahEvacuation::name() const {
  return "Shenandoah evacuation";
}

void VM_ShenandoahEvacuation::doit() {
  if (ShenandoahGCVerbose)
    tty->print("vm_ShenandoahEvacuation\n");

  ShenandoahHeap *sh = ShenandoahHeap::heap();
  sh->do_evacuation();
}
