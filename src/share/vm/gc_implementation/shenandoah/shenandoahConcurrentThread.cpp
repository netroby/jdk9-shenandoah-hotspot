
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
  create_and_start();
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
  tty->print("Starting to run %s with number %ld YAY!!!, %s\n", name(), os::current_thread_id());
  wait_for_universe_init();
  ShenandoahHeap* sh = ShenandoahHeap::heap();
  ShenandoahCollectorPolicy* sh_policy = sh->collector_policy();
  ShenandoahConcurrentMark* scm = sh->concurrentMark();
  Thread* current_thread = Thread::current();
  clear_cm_aborted();

  while (!_should_terminate) {
    sleepBeforeNextCycle();
    {
      ResourceMark rm;
      HandleMark hm;
      
      if (!cm_has_aborted()) {
	scm->scanRootRegions();
      }

      if (!cm_has_aborted()) {
         scm->markFromRoots();
      } 
      SCMCheckpointRootsFinalClosure final_cl(scm);
      /*
      VM_ShenandoahFinal op;
      VMThread::execute(&op);
      */
      }
   }
}

ShenandoahConcurrentThread* ShenandoahConcurrentThread::start() {
  ShenandoahConcurrentThread* th = new ShenandoahConcurrentThread();
  return th;
}

void ShenandoahConcurrentThread::stop() {
  tty->print("Attempt to stop concurrentThread");
}

void ShenandoahConcurrentThread::create_and_start() {
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

