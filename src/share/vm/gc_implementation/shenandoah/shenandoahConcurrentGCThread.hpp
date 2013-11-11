#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTGCTHREAD_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTGCTHREAD_HPP

#include "gc_implementation/shared/concurrentGCThread.hpp"


class ShenandoahConcurrentGCThread: public ConcurrentGCThread {
 public:
  ShenandoahConcurrentGCThread();
  ~ShenandoahConcurrentGCThread();

  void run();

  void start();
  void yield();
  
  static void safepoint_synchronize();
  static void safepoint_desynchronize();

};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTGCTHREAD_HPP
