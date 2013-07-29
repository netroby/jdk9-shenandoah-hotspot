#ifndef SHARE_VM_GC_IMPLEMENTATION_SHARED_OOPQUEUE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHARED_OOPQUEUE_HPP

#include "gc_implementation/g1/ptrQueue.hpp"

class ObjectClosure;
class JavaThread;
class OopQueueSet;

// A ptrQueue whose elements are "oops", pointers to object heads.
class OopQueue: public PtrQueue {
  friend class OopQueueSet;

private:
  // Filter out unwanted entries from the buffer.
  void filter();

  // Apply the closure to all elements.
  void apply_closure(ExtendedOopClosure* cl);

  // Apply the closure to all elements and empty the buffer;
  void apply_closure_and_empty(ExtendedOopClosure* cl);

  // Apply the closure to all elements of "buf", down to "index" (inclusive.)
  static void apply_closure_to_buffer(ExtendedOopClosure* cl,
                                      void** buf, size_t index, size_t sz);

public:
  OopQueue(PtrQueueSet* qset, bool perm = false) :
    // SATB queues are only active during marking cycles. We create
    // them with their active field set to false. If a thread is
    // created during a cycle and its SATB queue needs to be activated
    // before the thread starts running, we'll need to set its active
    // field to true. This is done in JavaThread::initialize_queues().
    PtrQueue(qset, perm, false /* active */) { }

  // Overrides PtrQueue::flush() so that it can filter the buffer
  // before it is flushed.
  virtual void flush();

  // Overrides PtrQueue::should_enqueue_buffer(). See the method's
  // definition for more information.
  virtual bool should_enqueue_buffer();

#ifndef PRODUCT
  // Helpful for debugging
  void print(const char* name);
  static void print(const char* name, void** buf, size_t index, size_t sz);
#endif // PRODUCT

  void verify_oops_in_buffer() NOT_DEBUG_RETURN;
};

class OopQueueSet: public PtrQueueSet {
  ExtendedOopClosure* _closure;
  ExtendedOopClosure** _par_closures;  // One per ParGCThread.

  OopQueue _shared_oop_queue;

  // Utility function to support sequential and parallel versions.  If
  // "par" is true, then "worker" is the par thread id; if "false", worker
  // is ignored.
  bool apply_closure_to_completed_buffer_work(bool par, int worker);

#ifdef ASSERT
  void dump_active_values(JavaThread* first, bool expected_active);
#endif // ASSERT

public:
  OopQueueSet();

  void initialize(Monitor* cbl_mon, Mutex* fl_lock,
                  int process_completed_threshold,
                  Mutex* lock);

  static void handle_zero_index_for_thread(JavaThread* t);

  // Apply "set_active(b)" to all Java threads' SATB queues. It should be
  // called only with the world stopped. The method will assert that the
  // SATB queues of all threads it visits, as well as the SATB queue
  // set itself, has an active value same as expected_active.
  void set_active_all_threads(bool b, bool expected_active);

  // Filter all the currently-active SATB buffers.
  void filter_thread_buffers();

  // Register "blk" as "the closure" for all queues.  Only one such closure
  // is allowed.  The "apply_closure_to_completed_buffer" method will apply
  // this closure to a completed buffer, and "iterate_closure_all_threads"
  // applies it to partially-filled buffers (the latter should only be done
  // with the world stopped).
  void set_closure(ExtendedOopClosure* closure);
  // Set the parallel closures: pointer is an array of pointers to
  // closures, one for each parallel GC thread.
  void set_par_closure(int i, ExtendedOopClosure* closure);

  // Apply the registered closure to all entries on each
  // currently-active buffer and then empty the buffer. It should only
  // be called serially and at a safepoint.
  void iterate_closure_all_threads();
  // Parallel version of the above.
  void par_iterate_closure_all_threads(int worker);

  // If there exists some completed buffer, pop it, then apply the
  // registered closure to all its elements, and return true.  If no
  // completed buffers exist, return false.
  bool apply_closure_to_completed_buffer() {
    return apply_closure_to_completed_buffer_work(false, 0);
  }
  // Parallel version of the above.
  bool par_apply_closure_to_completed_buffer(int worker) {
    return apply_closure_to_completed_buffer_work(true, worker);
  }

  // Apply the given closure on enqueued and currently-active buffers
  // respectively. Both methods are read-only, i.e., they do not
  // modify any of the buffers.
  void iterate_completed_buffers_read_only(ExtendedOopClosure* cl);
  void iterate_thread_buffers_read_only(ExtendedOopClosure* cl);

#ifndef PRODUCT
  // Helpful for debugging
  void print_all(const char* msg);
#endif // PRODUCT

  OopQueue* shared_oop_queue() { return &_shared_oop_queue; }

  // If a marking is being abandoned, reset any unprocessed log buffers.
  void abandon_partial_marking();
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHARED_OOPQUEUE_HPP
