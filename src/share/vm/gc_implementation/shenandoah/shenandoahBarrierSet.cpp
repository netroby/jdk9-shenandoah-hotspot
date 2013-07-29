
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"

class UpdateRefsForOopClosure: public ExtendedOopClosure {

private:
  ShenandoahBarrierSet* _bs;

public:
  UpdateRefsForOopClosure(ShenandoahBarrierSet* bs) : _bs(bs)
    { }

  void do_oop(oop* p)       {
    _bs->enqueue_update_ref(p);
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

void ShenandoahBarrierSet::enqueue_update_ref(oop* ref) {

  if (!JavaThread::update_refs_queue_set().is_active()) return;
  // tty->print_cr("enqueueing update-ref: %p", ref);
  Thread* thr = Thread::current();
  if (thr->is_Java_thread()) {
    JavaThread* jt = (JavaThread*)thr;
    jt->update_refs_queue().enqueue((oop) ref);
  } else {
    MutexLocker x(Shared_SATB_Q_lock);
    JavaThread::update_refs_queue_set().shared_oop_queue()->enqueue((oop) ref);
  }
}


void ShenandoahBarrierSet::write_region_work(MemRegion mr) {

  // This is called for cloning an object (see jvm.cpp) after the clone
  // has been made. We are not interested in any 'previous value' because
  // it would be NULL in any case. But we *are* interested in any oop*
  // that potentially need to be updated.

  // tty->print_cr("write_region_work: %p, %p", mr.start(), mr.end());
  oop obj = oop(mr.start());
  assert(obj->is_oop(), "must be an oop");
  assert(ShenandoahBarrierSet::has_brooks_ptr(obj), "must have brooks pointer");
  UpdateRefsForOopClosure cl(this);
  obj->oop_iterate(&cl);
}
