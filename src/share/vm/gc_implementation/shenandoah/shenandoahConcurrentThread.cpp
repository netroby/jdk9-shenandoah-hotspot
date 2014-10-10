/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */

#include "gc_implementation/shenandoah/shenandoahConcurrentThread.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "memory/iterator.hpp"
#include "memory/universe.hpp"
#include "runtime/vmThread.hpp"

SurrogateLockerThread* ShenandoahConcurrentThread::_slt = NULL;

ShenandoahConcurrentThread::ShenandoahConcurrentThread() :
  ConcurrentGCThread(),
  _epoch(0),
  _concurrent_mark_started(false),
  _concurrent_mark_in_progress(false),
  _waiting_for_jni_critical(false),
  _do_full_gc(false)
{
  //  create_and_start();
}

ShenandoahConcurrentThread::~ShenandoahConcurrentThread() {
  // This is here so that super is called.
}

void ShenandoahConcurrentThread::notify_jni_critical() {

  assert(_waiting_for_jni_critical, "must be waiting for jni critical notification");  

  // tty->print_cr("doing GC after JNI critical");

  ShenandoahHeap* heap = ShenandoahHeap::heap();

  if (_do_full_gc) {
    VM_ShenandoahFullGC full_gc;
    VMThread::execute(&full_gc);
  } else {
    heap->set_evacuation_in_progress(true);

    // We need to do non-concurrent marking right now, before we release the flag below.
    // The GC background thread is waiting on it and would start another marking
    // cycle otherwise.
    if (! ShenandoahConcurrentEvacuation) {
      VM_ShenandoahEvacuation evacuation;
      VMThread::execute(&evacuation);
    }

  }
  MonitorLockerEx ml(ShenandoahJNICritical_lock);
  _waiting_for_jni_critical = false;
  ml.notify_all();
}

void ShenandoahConcurrentThread::run() {
  initialize_in_thread();

  wait_for_universe_init();

  while(_slt == NULL) {
    // Wait for initialization to finish.
  }

  ShenandoahHeap* heap = ShenandoahHeap::heap();

  while (!_should_terminate) {
    if (_do_full_gc) {
      {
        MonitorLockerEx ml(ShenandoahJNICritical_lock, true);
        VM_ShenandoahFullGC full_gc;
        VMThread::execute(&full_gc);
        while (_waiting_for_jni_critical) {
          ml.wait(true);
        }
      }
      MonitorLockerEx ml(ShenandoahFullGC_lock);
      _do_full_gc = false;
      ml.notify_all();
    } else if (heap->shenandoahPolicy()->should_start_concurrent_mark(heap->used(),
							       heap->capacity())) 
      {

	if (ShenandoahGCVerbose) 
	  tty->print("Capacity = "SIZE_FORMAT" Used = "SIZE_FORMAT"  doing initMark\n", heap->capacity(), heap->used());
 
	if (ShenandoahGCVerbose) tty->print("Starting a mark");

	VM_ShenandoahInitMark initMark;
	VMThread::execute(&initMark);

        if (ShenandoahConcurrentMarking) {
          ShenandoahHeap::heap()->concurrentMark()->mark_from_roots();

          VM_ShenandoahFinishMark finishMark;
          VMThread::execute(&finishMark);

        }

        {
          MonitorLockerEx ml(ShenandoahJNICritical_lock, true);
          while (_waiting_for_jni_critical) {
            ml.wait(true);
          }
        }

        // If we're not concurrently evacuating, evacuation is done
        // from VM_ShenandoahFinishMark within the VMThread above.
	if (ShenandoahConcurrentEvacuation) {
          VM_ShenandoahEvacuation evacuation;
          evacuation.doit();
	}

        if (ShenandoahUpdateRefsEarly) {
          if (ShenandoahConcurrentUpdateRefs) {
            VM_ShenandoahUpdateRefs update_refs;
            VMThread::execute(&update_refs);
            heap->update_references();
          }
        }

        heap->reset_mark_bitmap();

      } else {
      Thread::current()->_ParkEvent->park(10) ;
      // yield();
    }
  }
}

void ShenandoahConcurrentThread::do_full_gc() {

  assert(Thread::current()->is_Java_thread(), "expect Java thread here");

  MonitorLockerEx ml(ShenandoahFullGC_lock);
  _do_full_gc = true;
  while (_do_full_gc) {
    ml.wait();
  }
  assert(_do_full_gc == false, "expect full GC to have completed");
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
  _sts.yield();
}

void ShenandoahConcurrentThread::safepoint_synchronize() {
  assert(UseShenandoahGC, "just checking");
  _sts.synchronize();
}

void ShenandoahConcurrentThread::safepoint_desynchronize() {
  assert(UseShenandoahGC, "just checking");
  _sts.desynchronize();
}

void ShenandoahConcurrentThread::makeSurrogateLockerThread(TRAPS) {
  assert(UseShenandoahGC, "SLT thread needed only for concurrent GC");
  assert(THREAD->is_Java_thread(), "must be a Java thread");
  assert(_slt == NULL, "SLT already created");
  _slt = SurrogateLockerThread::make(THREAD);
}
