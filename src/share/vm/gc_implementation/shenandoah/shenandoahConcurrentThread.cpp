/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */

#include "gc_implementation/shenandoah/shenandoahConcurrentThread.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "memory/iterator.hpp"
#include "memory/universe.hpp"
#include "runtime/vmThread.hpp"

ShenandoahConcurrentThread::ShenandoahConcurrentThread() :
  ConcurrentGCThread(),
  _epoch(0),
  _concurrent_mark_started(false),
  _concurrent_mark_in_progress(false)
{
  //  create_and_start();
}

ShenandoahConcurrentThread::~ShenandoahConcurrentThread() {
  // This is here so that super is called.
}

class SCMCheckpointRootsFinalClosure : public VoidClosure {
  ShenandoahConcurrentMark* _scm;
public:
  SCMCheckpointRootsFinalClosure(ShenandoahConcurrentMark* scm) : _scm(scm) {}
  
  void do_void() {
    _scm->checkpointRootsFinal();
  }
};

void ShenandoahConcurrentThread::run() {
  initialize_in_thread();

  wait_for_universe_init();
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  while (!_should_terminate) {
    if (heap->shenandoahPolicy()->should_start_concurrent_mark(heap->used(),
							       heap->capacity())) 
      {

	if (ShenandoahGCVerbose) 
	  tty->print("Capacity = "SIZE_FORMAT" Used = "SIZE_FORMAT"  doing initMark\n", heap->capacity(), heap->used());
 
	if (ShenandoahGCVerbose) tty->print("Starting a mark");

	VM_ShenandoahInitMark initMark;
	VMThread::execute(&initMark);

        if (ShenandoahConcurrentMarking) {
          ShenandoahHeap::heap()->concurrentMark()->markFromRoots();

          VM_ShenandoahFinishMark finishMark;
          VMThread::execute(&finishMark);

        }

        // Wait if necessary for JNI critical regions to be cleared. See ShenandoahHeap::collect().
        while (heap->is_waiting_for_jni_before_gc()) {
          Thread::current()->_ParkEvent->park(1) ;
        }

        // If we're not concurrently evacuating, evacuation is done
        // from VM_ShenandoahFinishMark within the VMThread above.
	if (ShenandoahConcurrentEvacuation) {
          VM_ShenandoahEvacuation evacuation;
          evacuation.doit();
	}

      } else {
      Thread::current()->_ParkEvent->park(10) ;
      // yield();
    }
  }
}


void ShenandoahConcurrentThread::print() const {
  print_on(tty);
}

void ShenandoahConcurrentThread::print_on(outputStream* st) const {
  st->print("Shenandoah Concurrent Thread");
  Thread::print_on(st);
  st->cr();
}

void ShenandoahConcurrentThread::sleepBeforeNextCycle() {
  assert(false, "Wake up in the GC thread that never sleeps :-)");
}

void ShenandoahConcurrentThread::set_cm_started() {
    assert(!_concurrent_mark_in_progress, "cycle in progress"); 
    _concurrent_mark_started = true;  
}
  
void ShenandoahConcurrentThread::clear_cm_started() { 
    assert(_concurrent_mark_in_progress, "must be starting a cycle"); 
    _concurrent_mark_started = false; 
}

bool ShenandoahConcurrentThread::cm_started() {
  return _concurrent_mark_started;
}

void ShenandoahConcurrentThread::set_cm_in_progress() { 
  assert(_concurrent_mark_started, "must be starting a cycle"); 
  _concurrent_mark_in_progress = true;  
}

void ShenandoahConcurrentThread::clear_cm_in_progress() { 
  assert(!_concurrent_mark_started, "must not be starting a new cycle"); 
  _concurrent_mark_in_progress = false; 
}

bool ShenandoahConcurrentThread::cm_in_progress() { 
  return _concurrent_mark_in_progress;  
}

void ShenandoahConcurrentThread::start() {
  create_and_start();
}

void ShenandoahConcurrentThread::yield() {
  _sts.yield("Concurrent Mark");
}

void ShenandoahConcurrentThread::safepoint_synchronize() {
  assert(UseShenandoahGC, "just checking");
  _sts.suspend_all();
}

void ShenandoahConcurrentThread::safepoint_desynchronize() {
  assert(UseShenandoahGC, "just checking");
  _sts.resume_all();
}
