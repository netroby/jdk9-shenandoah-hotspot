
#include "gc_implementation/shenandoah/shenandoahConcurrentThread.hpp"

ShenandoahConcurrentThread::ShenandoahConcurrentThread() :
  ConcurrentGCThread(),
  _epoch(0),
  _started(false),
  _in_progress(false)
{
  if (os::create_thread(this, os::shenandoah_thread)) {
    os::start_thread(this);
  }
}

void ShenandoahConcurrentThread::run() {
  tty->print("Starting to run %s with number %ld YAY!!!, %s\n", name(), os::current_thread_id());
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


  
  
