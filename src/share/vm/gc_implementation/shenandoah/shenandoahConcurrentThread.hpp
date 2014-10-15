/*
Copyright 2014 Red Hat, Inc. and/or its affiliates.
 */
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTTHREAD_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTTHREAD_HPP

#include "gc_implementation/shared/concurrentGCThread.hpp"
#include "memory/resourceArea.hpp"

// For now we just want to have a concurrent marking thread. 
// Once we have that working we will build a concurrent evacuation thread.

class ShenandoahConcurrentThread: public ConcurrentGCThread {
  friend class VMStructs;

 public:
  virtual void run();

 private:
  volatile bool                    _concurrent_mark_started;
  volatile bool                    _concurrent_mark_in_progress;
  volatile bool                    _concurrent_mark_aborted;

  volatile bool _waiting_for_jni_critical;

  int _epoch;

  static SurrogateLockerThread* _slt;

  bool _do_full_gc;

  void sleepBeforeNextCycle();

 public:
  // Constructor
  ShenandoahConcurrentThread();
  ~ShenandoahConcurrentThread();

  static void makeSurrogateLockerThread(TRAPS);
  static SurrogateLockerThread* slt() { return _slt; }

  // Printing
  void print_on(outputStream* st) const;
  void print() const;

  void set_cm_started();
  void clear_cm_started();
  bool cm_started();

  void set_cm_in_progress();
  void clear_cm_in_progress();
  bool cm_in_progress();

  void cm_abort() { _concurrent_mark_aborted = true;}
  bool cm_has_aborted() { return _concurrent_mark_aborted;}
  void clear_cm_aborted() { _concurrent_mark_aborted = false;}

  void do_full_gc();

  void schedule_full_gc();

  void set_waiting_for_jni_before_gc(bool wait) {
    _waiting_for_jni_critical = wait;
  }

  void notify_jni_critical();

  // This flag returns true from the moment a marking cycle is
  // initiated (during the initial-mark pause when started() is set)
  // to the moment when the cycle completes (just after the next
  // marking bitmap has been cleared and in_progress() is
  // cleared). While this flag is true we will not start another cycle
  // so that cycles do not overlap. We cannot use just in_progress()
  // as the CM thread might take some time to wake up before noticing
  // that started() is set and set in_progress().
  bool during_cycle()      { return cm_started() || cm_in_progress(); }

  char* name() const { return (char*)"ShenandoahConcurrentThread";}
  void start();
  void yield();

  static void safepoint_synchronize();
  static void safepoint_desynchronize();

};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHCONCURRENTTHREAD_HPP
