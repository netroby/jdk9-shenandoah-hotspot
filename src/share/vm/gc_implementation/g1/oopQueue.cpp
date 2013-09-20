#include "precompiled.hpp"
#include "gc_implementation/g1/oopQueue.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/sharedHeap.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.hpp"
#include "runtime/vmThread.hpp"

void OopQueue::flush() {
  // The buffer might contain refs into the CSet. We have to filter it
  // first before we flush it, otherwise we might end up with an
  // enqueued buffer with refs into the CSet which breaks our invariants.
  filter();
  PtrQueue::flush();
}

// This method removes entries from an SATB buffer that will not be
// useful to the concurrent marking threads. An entry is removed if it
// satisfies one of the following conditions:
//
// * it points to an object outside the G1 heap (G1's concurrent
//     marking only visits objects inside the G1 heap),
// * it points to an object that has been allocated since marking
//     started (according to SATB those objects do not need to be
//     visited during marking), or
// * it points to an object that has already been marked (no need to
//     process it again).
//
// The rest of the entries will be retained and are compacted towards
// the top of the buffer. Note that, because we do not allow old
// regions in the CSet during marking, all objects on the CSet regions
// are young (eden or survivors) and therefore implicitly live. So any
// references into the CSet will be removed during filtering.

void OopQueue::filter() {
  SharedHeap* heap = SharedHeap::heap();
  void** buf = _buf;
  size_t sz = _sz;

  if (buf == NULL) {
    // nothing to do
    return;
  }

  // Used for sanity checking at the end of the loop.
  debug_only(size_t entries = 0; size_t retained = 0;)

  size_t i = sz;
  size_t new_index = sz;

  while (i > _index) {
    assert(i > 0, "we should have at least one more entry to process");
    i -= oopSize;
    debug_only(entries += 1;)
    oop** p = (oop**) &buf[byte_index_to_index((int) i)];
    oop* obj = *p;
    // NULL the entry so that unused parts of the buffer contain NULLs
    // at the end. If we are going to retain it we will copy it to its
    // final place. If we have retained all entries we have visited so
    // far, we'll just end up copying it to the same place.
    *p = NULL;

    bool retain = true; // TODO: Do we need any handling here?
    if (retain) {
      assert(new_index > 0, "we should not have already filled up the buffer");
      new_index -= oopSize;
      assert(new_index >= i,
             "new_index should never be below i, as we alwaysr compact 'up'");
      oop** new_p = (oop**) &buf[byte_index_to_index((int) new_index)];
      assert(new_p >= p, "the destination location should never be below "
             "the source as we always compact 'up'");
      assert(*new_p == NULL,
             "we should have already cleared the destination location");
      *new_p = obj;
      debug_only(retained += 1;)
    }
  }

#ifdef ASSERT
  size_t entries_calc = (sz - _index) / oopSize;
  assert(entries == entries_calc, "the number of entries we counted "
         "should match the number of entries we calculated");
  size_t retained_calc = (sz - new_index) / oopSize;
  assert(retained == retained_calc, "the number of retained entries we counted "
         "should match the number of retained entries we calculated");
#endif // ASSERT

  _index = new_index;
}

// This method will first apply the above filtering to the buffer. If
// post-filtering a large enough chunk of the buffer has been cleared
// we can re-use the buffer (instead of enqueueing it) and we can just
// allow the mutator to carry on executing using the same buffer
// instead of replacing it.

bool OopQueue::should_enqueue_buffer() {
  assert(_lock == NULL || _lock->owned_by_self(),
         "we should have taken the lock before calling this");

  // Even if G1SATBBufferEnqueueingThresholdPercent == 0 we have to
  // filter the buffer given that this will remove any references into
  // the CSet as we currently assume that no such refs will appear in
  // enqueued buffers.

  // This method should only be called if there is a non-NULL buffer
  // that is full.
  assert(_index == 0, "pre-condition");
  assert(_buf != NULL, "pre-condition");

  filter();

  size_t sz = _sz;
  size_t all_entries = sz / oopSize;
  size_t retained_entries = (sz - _index) / oopSize;
  size_t perc = retained_entries * 100 / all_entries;
  bool should_enqueue = perc > (size_t) G1SATBBufferEnqueueingThresholdPercent;
  return should_enqueue;
}

void OopQueue::apply_closure(ExtendedOopClosure* cl) {
  if (_buf != NULL) {
    apply_closure_to_buffer(cl, _buf, _index, _sz);
  }
}

void OopQueue::apply_closure_and_empty(ExtendedOopClosure* cl) {
  if (_buf != NULL) {
    apply_closure_to_buffer(cl, _buf, _index, _sz);
    _index = _sz;
  }
}

void OopQueue::apply_closure_to_buffer(ExtendedOopClosure* cl,
                                          void** buf, size_t index, size_t sz) {
  if (cl == NULL) return;
  for (size_t i = index; i < sz; i += oopSize) {
    oop* obj = (oop*)buf[byte_index_to_index((int)i)];
    // There can be NULL entries because of destructors.
    if (obj != NULL) {
      cl->do_oop(obj);
    }
  }
}

#ifndef PRODUCT
// Helpful for debugging

void OopQueue::print(const char* name) {
  print(name, _buf, _index, _sz);
}

void OopQueue::print(const char* name,
                        void** buf, size_t index, size_t sz) {
  gclog_or_tty->print_cr("  SATB BUFFER [%s] buf: "PTR_FORMAT" "
                         "index: "SIZE_FORMAT" sz: "SIZE_FORMAT,
                         name, buf, index, sz);
}
#endif // PRODUCT

#ifdef ASSERT
void OopQueue::verify_oops_in_buffer() {
  // TODO: What can we do here?
  
  // if (_buf == NULL) return;
  // for (size_t i = _index; i < _sz; i += oopSize) {
  //   oop* obj = (oop*)_buf[byte_index_to_index((int)i)];
  //   assert(obj != NULL && obj->is_oop(true /* ignore mark word */),
  //          "Not an oop");
  // }
}
#endif

#ifdef _MSC_VER // the use of 'this' below gets a warning, make it go away
#pragma warning( disable:4355 ) // 'this' : used in base member initializer list
#endif // _MSC_VER

OopQueueSet::OopQueueSet() :
  PtrQueueSet(), _closure(NULL), _par_closures(NULL),
  _shared_oop_queue(this, true /*perm*/) { }

void OopQueueSet::initialize(Monitor* cbl_mon, Mutex* fl_lock,
                                  int process_completed_threshold,
                                  Mutex* lock) {
  PtrQueueSet::initialize(cbl_mon, fl_lock, process_completed_threshold, -1);
  _shared_oop_queue.set_lock(lock);
  if (ParallelGCThreads > 0) {
    _par_closures = NEW_C_HEAP_ARRAY(ExtendedOopClosure*, ParallelGCThreads, mtGC);
  }
}

void OopQueueSet::handle_zero_index_for_thread(JavaThread* t) {
  DEBUG_ONLY(t->update_refs_queue().verify_oops_in_buffer();)
  t->update_refs_queue().handle_zero_index();
}

#ifdef ASSERT
void OopQueueSet::dump_active_values(JavaThread* first,
                                          bool expected_active) {
  gclog_or_tty->print_cr("SATB queue active values for Java Threads");
  gclog_or_tty->print_cr(" SATB queue set: active is %s",
                         (is_active()) ? "TRUE" : "FALSE");
  gclog_or_tty->print_cr(" expected_active is %s",
                         (expected_active) ? "TRUE" : "FALSE");
  for (JavaThread* t = first; t; t = t->next()) {
    bool active = t->update_refs_queue().is_active();
    gclog_or_tty->print_cr("  thread %s, active is %s",
                           t->name(), (active) ? "TRUE" : "FALSE");
  }
}
#endif // ASSERT

void OopQueueSet::set_active_all_threads(bool b,
                                              bool expected_active) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at safepoint.");
  JavaThread* first = Threads::first();

#ifdef ASSERT
  if (_all_active != expected_active) {
    dump_active_values(first, expected_active);

    // I leave this here as a guarantee, instead of an assert, so
    // that it will still be compiled in if we choose to uncomment
    // the #ifdef ASSERT in a product build. The whole block is
    // within an #ifdef ASSERT so the guarantee will not be compiled
    // in a product build anyway.
    guarantee(false,
              "SATB queue set has an unexpected active value");
  }
#endif // ASSERT
  _all_active = b;

  for (JavaThread* t = first; t; t = t->next()) {
#ifdef ASSERT
    bool active = t->update_refs_queue().is_active();
    if (active != expected_active) {
      dump_active_values(first, expected_active);

      // I leave this here as a guarantee, instead of an assert, so
      // that it will still be compiled in if we choose to uncomment
      // the #ifdef ASSERT in a product build. The whole block is
      // within an #ifdef ASSERT so the guarantee will not be compiled
      // in a product build anyway.
      guarantee(false,
                "thread has an unexpected active value in its SATB queue");
    }
#endif // ASSERT
    t->update_refs_queue().set_active(b);
  }
}

void OopQueueSet::filter_thread_buffers() {
  for(JavaThread* t = Threads::first(); t; t = t->next()) {
    t->update_refs_queue().filter();
  }
  shared_oop_queue()->filter();
}

void OopQueueSet::set_closure(ExtendedOopClosure* closure) {
  _closure = closure;
}

void OopQueueSet::set_par_closure(int i, ExtendedOopClosure* par_closure) {
  assert(ParallelGCThreads > 0 && _par_closures != NULL, "Precondition");
  _par_closures[i] = par_closure;
}

void OopQueueSet::iterate_closure_all_threads() {
  for(JavaThread* t = Threads::first(); t; t = t->next()) {
    t->update_refs_queue().apply_closure_and_empty(_closure);
  }
  shared_oop_queue()->apply_closure_and_empty(_closure);
}

void OopQueueSet::par_iterate_closure_all_threads(int worker) {
  SharedHeap* sh = SharedHeap::heap();
  int parity = sh->strong_roots_parity();

  for(JavaThread* t = Threads::first(); t; t = t->next()) {
    if (t->claim_oops_do(true, parity)) {
      t->update_refs_queue().apply_closure_and_empty(_par_closures[worker]);
    }
  }

  // We also need to claim the VMThread so that its parity is updated
  // otherwise the next call to Thread::possibly_parallel_oops_do inside
  // a StrongRootsScope might skip the VMThread because it has a stale
  // parity that matches the parity set by the StrongRootsScope
  //
  // Whichever worker succeeds in claiming the VMThread gets to do
  // the shared queue.

  VMThread* vmt = VMThread::vm_thread();
  if (vmt->claim_oops_do(true, parity)) {
    shared_oop_queue()->apply_closure_and_empty(_par_closures[worker]);
  }
}

bool OopQueueSet::apply_closure_to_completed_buffer_work(bool par,
                                                              int worker) {
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
  ExtendedOopClosure* cl = (par ? _par_closures[worker] : _closure);
  if (nd != NULL) {
    void **buf = BufferNode::make_buffer_from_node(nd);
    OopQueue::apply_closure_to_buffer(cl, buf, 0, _sz);
    deallocate_buffer(buf);
    return true;
  } else {
    return false;
  }
}

void OopQueueSet::iterate_completed_buffers_read_only(ExtendedOopClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at safepoint.");
  assert(cl != NULL, "pre-condition");

  BufferNode* nd = _completed_buffers_head;
  while (nd != NULL) {
    void** buf = BufferNode::make_buffer_from_node(nd);
    OopQueue::apply_closure_to_buffer(cl, buf, 0, _sz);
    nd = nd->next();
  }
}

void OopQueueSet::iterate_thread_buffers_read_only(ExtendedOopClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at safepoint.");
  assert(cl != NULL, "pre-condition");

  for (JavaThread* t = Threads::first(); t; t = t->next()) {
    t->update_refs_queue().apply_closure(cl);
  }
  shared_oop_queue()->apply_closure(cl);
}

#ifndef PRODUCT
// Helpful for debugging

#define SATB_PRINTER_BUFFER_SIZE 256

void OopQueueSet::print_all(const char* msg) {
  char buffer[SATB_PRINTER_BUFFER_SIZE];
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at safepoint.");

  gclog_or_tty->cr();
  gclog_or_tty->print_cr("SATB BUFFERS [%s]", msg);

  BufferNode* nd = _completed_buffers_head;
  int i = 0;
  while (nd != NULL) {
    void** buf = BufferNode::make_buffer_from_node(nd);
    jio_snprintf(buffer, SATB_PRINTER_BUFFER_SIZE, "Enqueued: %d", i);
    OopQueue::print(buffer, buf, 0, _sz);
    nd = nd->next();
    i += 1;
  }

  for (JavaThread* t = Threads::first(); t; t = t->next()) {
    jio_snprintf(buffer, SATB_PRINTER_BUFFER_SIZE, "Thread: %s", t->name());
    t->update_refs_queue().print(buffer);
  }

  shared_oop_queue()->print("Shared");

  gclog_or_tty->cr();
}
#endif // PRODUCT

void OopQueueSet::abandon_partial_marking() {
  BufferNode* buffers_to_delete = NULL;
  {
    MutexLockerEx x(_cbl_mon, Mutex::_no_safepoint_check_flag);
    while (_completed_buffers_head != NULL) {
      BufferNode* nd = _completed_buffers_head;
      _completed_buffers_head = nd->next();
      nd->set_next(buffers_to_delete);
      buffers_to_delete = nd;
    }
    _completed_buffers_tail = NULL;
    _n_completed_buffers = 0;
    DEBUG_ONLY(assert_completed_buffer_list_len_correct_locked());
  }
  while (buffers_to_delete != NULL) {
    BufferNode* nd = buffers_to_delete;
    buffers_to_delete = nd->next();
    deallocate_buffer(BufferNode::make_buffer_from_node(nd));
  }
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at safepoint.");
  // So we can safely manipulate these queues.
  for (JavaThread* t = Threads::first(); t; t = t->next()) {
    t->update_refs_queue().reset();
  }
 shared_oop_queue()->reset();
}