
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

  if (ShenandoahGCVerbose) {
    tty->print("Starting to run %s with number %ld YAY!!!, %s\n", name(), os::current_thread_id());
  }

  wait_for_universe_init();
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  size_t targetStartMarking = heap->max_capacity() / 4;
  size_t targetBytesAllocated = heap->max_capacity() / 8;


  while (!_should_terminate) {
    if (heap->used() > targetStartMarking && heap->_bytesAllocSinceCM > targetBytesAllocated) {
      heap->_bytesAllocSinceCM = 0;
      if (ShenandoahGCVerbose) 
        tty->print("Capacity = "SIZE_FORMAT" Used = "SIZE_FORMAT" Target = "SIZE_FORMAT" doing initMark\n", heap->capacity(), heap->used(), targetStartMarking);
 
      if (ShenandoahGCVerbose) tty->print("Starting a mark");

      VM_ShenandoahInitMark initMark;
      VMThread::execute(&initMark);

      ShenandoahHeap::heap()->concurrentMark()->markFromRoots();

      VM_ShenandoahFinishMark finishMark;
      VMThread::execute(&finishMark);

      ShenandoahHeap::heap()->parallel_evacuate();

      if (ShenandoahVerify) {
        VM_ShenandoahVerifyHeapAfterEvacuation verify_after_evacuation;
        VMThread::execute(&verify_after_evacuation);
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
