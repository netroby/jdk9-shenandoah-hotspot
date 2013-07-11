
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHSATBQUEUE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHSATBQUEUE_HPP

#include "gc_implementation/shared/ptrQueue.hpp"
#include "memory/allocation.hpp"
#include "oops/oop.hpp"

class ShenandoahSATBElement : public CHeapObj<mtGC> {
private:
  oop _previous_value;
  oop* _referrer;

public:
  ShenandoahSATBElement(oop previous_value, oop* referrer)
    : _previous_value(previous_value), _referrer(referrer) {}

  oop get_previous_value() {
    return _previous_value;
  }

  oop* get_referrer() {
    return _referrer;
  }
};

class ShenandoahSATBElementClosure {
public:
  virtual void do_satb_element(ShenandoahSATBElement* satb_element) = 0;
};

class ShenandoahSATBQueue : public PtrQueue {
public:
  ShenandoahSATBQueue(PtrQueueSet* qset, bool perm = false);

  void apply_closure_and_empty(ShenandoahSATBElementClosure* cl);
  static void apply_closure_to_buffer(ShenandoahSATBElementClosure* cl,
                                      void** buf, size_t index, size_t sz);
};

class ShenandoahSATBQueueSet : public PtrQueueSet, public CHeapObj<mtGC>  {
private:
  ShenandoahSATBQueue _shared_satb_queue;
  ShenandoahSATBElementClosure* _closure;

public:
  ShenandoahSATBQueueSet();
  void initialize(Monitor* cbl_mon, Mutex* fl_lock, int process_completed_threshold, Mutex* lock);
  void set_closure(ShenandoahSATBElementClosure* cl);
  bool apply_closure_to_completed_buffer();
  void iterate_closure_all_threads();

  ShenandoahSATBQueue* shared_satb_queue() { return &_shared_satb_queue; }

};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAHSATBQUEUE_HPP
