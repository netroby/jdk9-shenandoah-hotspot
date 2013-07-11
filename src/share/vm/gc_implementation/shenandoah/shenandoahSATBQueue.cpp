
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.hpp"

#include "gc_implementation/shenandoah/shenandoahSATBQueue.hpp"

ShenandoahSATBQueue::ShenandoahSATBQueue(PtrQueueSet* qset, bool perm) :
  PtrQueue(qset, perm, false) {}

void ShenandoahSATBQueue::apply_closure_to_buffer(ShenandoahSATBElementClosure* cl,
                                                  void** buf, size_t index, size_t sz) {
  if (cl == NULL) return;
  for (size_t i = index; i < sz; i += oopSize) {
    ShenandoahSATBElement* entry = (ShenandoahSATBElement*)buf[byte_index_to_index((int)i)];
    if (entry != NULL) {
      // assert(entry != NULL, "no null entries here");
      cl->do_satb_element(entry);
      delete entry;
    }
  }
}

void ShenandoahSATBQueue::apply_closure_and_empty(ShenandoahSATBElementClosure* cl) {
  if (_buf != NULL) {
    apply_closure_to_buffer(cl, _buf, _index, _sz);
    _index = _sz;
  }
}


ShenandoahSATBQueueSet::ShenandoahSATBQueueSet() : _shared_satb_queue(this, true /*perm*/) { }

void ShenandoahSATBQueueSet::initialize(Monitor* cbl_mon, Mutex* fl_lock,
                                        int process_completed_threshold,
                                        Mutex* lock) {
  PtrQueueSet::initialize(cbl_mon, fl_lock, process_completed_threshold, -1);
  _shared_satb_queue.set_lock(lock);
}

void ShenandoahSATBQueueSet::set_closure(ShenandoahSATBElementClosure* cl) {
  _closure = cl;
}

bool ShenandoahSATBQueueSet::apply_closure_to_completed_buffer() {
  BufferNode* nd = NULL;
  {
    MutexLockerEx x(_cbl_mon, Mutex::_no_safepoint_check_flag);
    if (_completed_buffers_head != NULL) {
      nd = _completed_buffers_head;
      _completed_buffers_head = nd->next();
      if (_completed_buffers_head == NULL) _completed_buffers_tail = NULL;
      _n_completed_buffers--;
      if (_n_completed_buffers == 0) _process_completed = false;
    }
  }
  ShenandoahSATBElementClosure* cl = _closure;
  if (nd != NULL) {
    void **buf = BufferNode::make_buffer_from_node(nd);
    ShenandoahSATBQueue::apply_closure_to_buffer(cl, buf, 0, _sz);
    deallocate_buffer(buf);
    return true;
  } else {
    return false;
  }
}

void ShenandoahSATBQueueSet::iterate_closure_all_threads() {
  for(JavaThread* t = Threads::first(); t; t = t->next()) {
    ((ShenandoahSATBQueue&) t->satb_mark_queue()).apply_closure_and_empty(_closure);
  }
  shared_satb_queue()->apply_closure_and_empty(_closure);
}

