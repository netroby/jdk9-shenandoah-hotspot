/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#include "gc_implementation/shenandoah/shenandoahMarkCompact.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "memory/gcLocker.hpp"

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

  if (! ShenandoahConcurrentMarking) {
    sh->concurrentMark()->mark_from_roots(! ShenandoahUpdateRefsEarly);
    VM_ShenandoahFinishMark finishMark;
    finishMark.doit();
  }
}

VM_Operation::VMOp_Type VM_ShenandoahFinishMark::type() const {
  return VMOp_ShenandoahFinishMark;
}

const char* VM_ShenandoahFinishMark::name() const {
  return "Shenandoah Finish Mark";
}

VM_Operation::VMOp_Type VM_ShenandoahFullGC::type() const {
  return VMOp_ShenandoahFullGC;
}

void VM_ShenandoahFullGC::doit() {

  if (! GC_locker::check_active_before_gc()) {
    ShenandoahMarkCompact mark_compact;
    mark_compact.do_mark_compact();
  } else {
    if (ShenandoahTraceJNICritical) {
      gclog_or_tty->print_cr("Deferring full GC because of active JNI critical regions");
    }
    // This makes the GC background thread wait, and kick off evacuation as
    // soon as JNI notifies us that critical regions have all been left.
    ShenandoahHeap::heap()->concurrent_thread()->set_waiting_for_jni_before_gc(true);
  }
}

const char* VM_ShenandoahFullGC::name() const {
  return "Shenandoah Full GC";
}


bool VM_ShenandoahReferenceOperation::doit_prologue() {
  ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
  sh->acquire_pending_refs_lock();
  return true;
}

void VM_ShenandoahReferenceOperation::doit_epilogue() {
  ShenandoahHeap *sh = ShenandoahHeap::heap();
  sh->release_pending_refs_lock();
}

void VM_ShenandoahFinishMark::doit() {
  ShenandoahHeap *sh = ShenandoahHeap::heap();
  if (ShenandoahGCVerbose)
    tty->print("vm_ShenandoahFinalMark\n");

  sh->shenandoahPolicy()->record_final_mark_start();  
  sh->concurrentMark()->finish_mark_from_roots();
  sh->stop_concurrent_marking();
  sh->prepare_for_concurrent_evacuation();
  sh->shenandoahPolicy()->record_final_mark_end();    

  if (! GC_locker::check_active_before_gc()) {
    VM_ShenandoahStartEvacuation start_evacuation;
    start_evacuation.doit();
  } else {

    if (ShenandoahTraceJNICritical) {
      gclog_or_tty->print_cr("Deferring concurrent evacuation because of active JNI critical regions");
    }

    // This makes the GC background thread wait, and kick off evacuation as
    // soon as JNI notifies us that critical regions have all been left.
    sh->concurrent_thread()->set_waiting_for_jni_before_gc(true);
  }

}

void VM_ShenandoahStartEvacuation::doit() {
  ShenandoahHeap *sh = ShenandoahHeap::heap();
  sh->set_evacuation_in_progress(true);

  if (! ShenandoahConcurrentEvacuation) {
    VM_ShenandoahEvacuation evacuation;
    evacuation.doit();
  } else {
    sh->evacuate_and_update_roots();
  }
}

VM_Operation::VMOp_Type VM_ShenandoahStartEvacuation::type() const {
  return VMOp_ShenandoahStartEvacuation;
}

const char* VM_ShenandoahStartEvacuation::name() const {
  return "Start shenandoah evacuation";
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

  if (! ShenandoahConcurrentUpdateRefs) {
    assert(! ShenandoahConcurrentEvacuation, "turn off concurrent evacuation");
    sh->prepare_for_update_references();
    sh->update_references();
  }
}

VM_Operation::VMOp_Type VM_ShenandoahVerifyHeapAfterUpdateRefs::type() const {
  return VMOp_ShenandoahVerifyHeapAfterUpdateRefs;
}

const char* VM_ShenandoahVerifyHeapAfterUpdateRefs::name() const {
  return "Shenandoah verify heap after updating references";
}

void VM_ShenandoahVerifyHeapAfterUpdateRefs::doit() {

  ShenandoahHeap *sh = ShenandoahHeap::heap();
  sh->verify_heap_after_update_refs();

}

VM_Operation::VMOp_Type VM_ShenandoahUpdateRootRefs::type() const {
  return VMOp_ShenandoahUpdateRootRefs;
}

const char* VM_ShenandoahUpdateRootRefs::name() const {
  return "Shenandoah update root references";
}

void VM_ShenandoahUpdateRootRefs::doit() {
  if (ShenandoahGCVerbose)
    tty->print("vm_ShenandoahUpdateRootRefs\n");

  ShenandoahHeap *sh = ShenandoahHeap::heap();
  sh->update_roots();

  sh->recycle_dirty_regions();
}

VM_Operation::VMOp_Type VM_ShenandoahUpdateRefs::type() const {
  return VMOp_ShenandoahUpdateRefs;
}

const char* VM_ShenandoahUpdateRefs::name() const {
  return "Shenandoah update references";
}

void VM_ShenandoahUpdateRefs::doit() {
  if (ShenandoahGCVerbose)
    tty->print("vm_ShenandoahUpdateRefs\n");

  ShenandoahHeap *sh = ShenandoahHeap::heap();
  sh->prepare_for_update_references();
  assert(ShenandoahConcurrentUpdateRefs, "only do this when concurrent update references is turned on");
}
