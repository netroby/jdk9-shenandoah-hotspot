#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTTHREAD_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTTHREAD_HPP

#include "gc_implementation/shared/concurrentGCThread.hpp"

// For now we just want to have a concurrent marking thread. 
// Once we have that working we will build a concurrent evacuation thread.

class ShenandoahConcurrentThread: public ConcurrentGCThread {
  friend class VMStructs;

 public:
  virtual void run();

 private:
  volatile bool                    _started;
  volatile bool                    _in_progress;

  int _epoch;

  void sleepBeforeNextCycle();

 public:
  // Constructor
  ShenandoahConcurrentThread();

  // Printing
  void print_on(outputStream* st) const;
  void print() const;

  void set_started()       { assert(!_in_progress, "cycle in progress"); _started = true;  }
  void clear_started()     { assert(_in_progress, "must be starting a cycle"); _started = false; }
  bool started()           { return _started;  }

  void set_in_progress()   { assert(_started, "must be starting a cycle"); _in_progress = true;  }
  void clear_in_progress() { assert(!_started, "must not be starting a new cycle"); _in_progress = false; }
  bool in_progress()       { return _in_progress;  }

  // This flag returns true from the moment a marking cycle is
  // initiated (during the initial-mark pause when started() is set)
  // to the moment when the cycle completes (just after the next
  // marking bitmap has been cleared and in_progress() is
  // cleared). While this flag is true we will not start another cycle
  // so that cycles do not overlap. We cannot use just in_progress()
  // as the CM thread might take some time to wake up before noticing
  // that started() is set and set in_progress().
  bool during_cycle()      { return started() || in_progress(); }

  char* name() const { return (char*)"ShenandoahConcurrentThread";}

  // shutdown
  void stop();
  void create_and_start();
  ShenandoahConcurrentThread* start();

};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTTHREAD_HPP
