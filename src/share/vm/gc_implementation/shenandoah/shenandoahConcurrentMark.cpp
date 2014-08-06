/*
  Copyright 2014 Red Hat, Inc. and/or its affiliates.
*/
/*
 * Copyright (c) 2013, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "memory/referenceProcessor.hpp"
#include "classfile/symbolTable.hpp"

class SCMConcurrentMarkingTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _cm;
  ParallelTaskTerminator* _terminator;
  int _seed;

public:
  SCMConcurrentMarkingTask(ShenandoahConcurrentMark* cm, ParallelTaskTerminator* terminator) :
    AbstractGangTask("Root Region Scan"), _cm(cm), _terminator(terminator), _seed(17) {
  }

      
  void work(uint worker_id) {

    ShenandoahMarkRefsClosure cl1(worker_id);
    ShenandoahMarkRefsNoUpdateClosure cl2(worker_id);
    ExtendedOopClosure* cl;

    if (! ShenandoahUpdateRefsEarly) {
      cl = &cl1;
    } else {
      cl = &cl2;
    }

    while (true) {
      if (!_cm->try_queue(worker_id,cl) &&
	  !_cm->try_overflow_queue(worker_id, cl) &&
	  !_cm->try_to_steal(worker_id, cl, &_seed) &&
	  !_cm->try_draining_an_satb_buffer(worker_id)) {
	if (_terminator->offer_termination()) break;
      }
    }
  }
};

bool ShenandoahConcurrentMark::try_queue(uint worker_id, ExtendedOopClosure* cl) {
  oop obj;
  if (task_queues()->queue(worker_id)->pop_local(obj)) {
    traverse_object(cl, obj);
    return true;
  } else 
    return false;;
}

bool ShenandoahConcurrentMark::try_to_steal(uint worker_id, ExtendedOopClosure* cl, int *seed) {
  oop obj;
  if (task_queues()->steal(worker_id, seed, obj)) {
    traverse_object(cl, obj);
    return true;
  } else 
    return false;
}

bool ShenandoahConcurrentMark::try_overflow_queue(uint worker_id, ExtendedOopClosure* cl) {
  oop obj = overflow_queue()->pop();
  if (obj != NULL) {
    traverse_object(cl, obj);
    return true;
  } else
    return false;
}

bool ShenandoahConcurrentMark:: try_draining_an_satb_buffer(uint worker_id) {
  return drain_one_satb_buffer(worker_id);
}


void ShenandoahConcurrentMark::traverse_object(ExtendedOopClosure* cl, oop obj) {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();

  assert(sh->is_in(obj), "Should only traverse objects in the heap");

  if (obj != NULL) {
    assert(obj->is_oop(), "Oops, not an oop");
    assert(! sh->heap_region_containing(obj)->is_in_collection_set(), "we don't want to mark objects in from-space");
    obj->oop_iterate(cl);
  }
}

void ShenandoahConcurrentMark::initialize() {
  _max_worker_id = MAX2((uint)ShenandoahConcurrentGCThreads, 1U);
  _task_queues = new SCMObjToScanQueueSet((int) _max_worker_id);
  _overflow_queue = new SharedOverflowMarkQueue();

  for (uint i = 0; i < _max_worker_id; ++i) {
    SCMObjToScanQueue* task_queue = new SCMObjToScanQueue();
    task_queue->initialize();
    _task_queues->register_queue(i, task_queue);
  }
  JavaThread::satb_mark_queue_set().set_buffer_size(1014 /* G1SATBBufferSize */);
}

void ShenandoahConcurrentMark::markFromRoots() {
  if (ShenandoahGCVerbose) {
    tty->print_cr("STOPPING THE WORLD: before marking");
    tty->print_cr("Starting markFromRoots");
  }
  ShenandoahHeap* sh = (ShenandoahHeap *) Universe::heap();
  ParallelTaskTerminator terminator(_max_worker_id, _task_queues);
  ReferenceProcessor* rp = sh->ref_processor_cm();
  
  // enable ("weak") refs discovery
  rp->enable_discovery(true /*verify_disabled*/, true /*verify_no_refs*/);
  rp->setup_policy(false); // snapshot the soft ref policy to be used in this cycle

  
  SCMConcurrentMarkingTask markingTask = SCMConcurrentMarkingTask(this, &terminator);
  sh->conc_workers()->run_task(&markingTask);
  
  if (ShenandoahGCVerbose) {
    tty->print_cr("Finishing markFromRoots");
    tty->print_cr("RESUMING THE WORLD: after marking");
    
    TASKQUEUE_STATS_ONLY(print_taskqueue_stats());
    TASKQUEUE_STATS_ONLY(reset_taskqueue_stats());
  }

}

class FinishDrainSATBBuffersTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _cm;
  ParallelTaskTerminator* _terminator;
public:
  FinishDrainSATBBuffersTask(ShenandoahConcurrentMark* cm, ParallelTaskTerminator* terminator) :
    AbstractGangTask("Finish draining SATB buffers"), _cm(cm), _terminator(terminator) {
  }

  void work(uint worker_id) {
    _cm->drain_satb_buffers(worker_id, true);
  }
};

void ShenandoahConcurrentMark::finishMarkFromRoots() {
  if (ShenandoahGCVerbose) {
    tty->print_cr("Starting finishMarkFromRoots");
  }

  ShenandoahHeap* sh = (ShenandoahHeap *) Universe::heap();

  // Trace any (new) unmarked root references.
  sh->prepare_unmarked_root_objs();

  ParallelTaskTerminator terminator(_max_worker_id, _task_queues);
  {
    ShenandoahHeap::StrongRootsScope srs(sh);
    // drain_satb_buffers(0, true);
    FinishDrainSATBBuffersTask drain_satb_buffers(this, &terminator);
    sh->conc_workers()->run_task(&drain_satb_buffers);
  }

  // Also drain our overflow queue.
  ShenandoahMarkRefsClosure cl1(0);
  ShenandoahMarkRefsNoUpdateClosure cl2(0);
  ExtendedOopClosure* cl;
  if (! ShenandoahUpdateRefsEarly) {
    cl = &cl1;
  } else {
    cl = &cl2;
  }
  oop obj = _overflow_queue->pop();
  while (obj != NULL) {
    assert(obj->is_oop(), "Oops, not an oop");
    obj->oop_iterate(cl);
    obj = _overflow_queue->pop();
  }

  weakRefsWork(true, 0);
  
  // Finally mark everything else we've got in our queues during the previous steps.
  SCMConcurrentMarkingTask markingTask = SCMConcurrentMarkingTask(this, &terminator);
  sh->conc_workers()->run_task(&markingTask);

  assert(_task_queues->queue(0)->is_empty(), "Should be empty");

  if (ShenandoahGCVerbose) {
    tty->print_cr("Finishing finishMarkFromRoots");
#ifdef SLOWDEBUG
    for (int i = 0; i <(int)_max_worker_id; i++) {
      tty->print("Queue: %d:", i);
      _task_queues->queue(i)->stats.print(tty, 10);
      tty->print("\n");
      //    _task_queues->queue(i)->stats.verify();
    }
#endif
  }
#ifdef ASSERT
  if (ShenandoahDumpHeapAfterConcurrentMark) {
    sh->prepare_for_verify();
    sh->print_all_refs("post-mark");
  }
#endif
}

class ShenandoahSATBMarkObjsClosure : public ObjectClosure {
  uint _worker_id;
  ShenandoahMarkObjsClosure _wrapped;

public:
  ShenandoahSATBMarkObjsClosure(uint worker_id) :
    _worker_id(worker_id),
    _wrapped(ShenandoahMarkObjsClosure(worker_id)) { }

  void do_object(oop obj) {
    obj = ShenandoahBarrierSet::resolve_oop_static(obj);
    _wrapped.do_object(obj);
  }

};

void ShenandoahConcurrentMark::drain_satb_buffers(uint worker_id, bool remark) {

  // tty->print_cr("start draining SATB buffers");

  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  ShenandoahSATBMarkObjsClosure cl(worker_id);

  SATBMarkQueueSet& satb_mq_set = JavaThread::satb_mark_queue_set();
  satb_mq_set.set_par_closure(worker_id, &cl);
  while (satb_mq_set.par_apply_closure_to_completed_buffer(worker_id));

  if (remark) {
    satb_mq_set.par_iterate_closure_all_threads(worker_id);
    assert(satb_mq_set.completed_buffers_num() == 0, "invariant");
  }

  satb_mq_set.set_par_closure(worker_id, NULL);

  // tty->print_cr("end draining SATB buffers");

}

bool ShenandoahConcurrentMark::drain_one_satb_buffer(uint worker_id) {

  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  ShenandoahSATBMarkObjsClosure cl(worker_id);

  SATBMarkQueueSet& satb_mq_set = JavaThread::satb_mark_queue_set();
  satb_mq_set.set_par_closure(worker_id, &cl);
  bool result = satb_mq_set.par_apply_closure_to_completed_buffer(worker_id);
  satb_mq_set.set_par_closure(worker_id, NULL);
  return result;
}

void ShenandoahConcurrentMark::checkpointRootsFinal() {
  ParallelTaskTerminator terminator(_max_worker_id, _task_queues);
  SCMConcurrentMarkingTask markingTask(this, &terminator);
  markingTask.work(0);
}

void ShenandoahConcurrentMark::addTask(oop obj, int q) {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  assert(obj->is_oop(), "Oops, not an oop");
  assert(! sh->heap_region_containing(obj)->is_in_collection_set(), "we don't want to mark objects in from-space");

  assert(sh->is_in((HeapWord*) obj), "Only push heap objects on the queue");
#ifdef ASSERT
  if (ShenandoahTraceConcurrentMarking){
    tty->print_cr("Adding object %p to marking queue %d", (HeapWord*) obj, q);
  }
#endif

  if (!_task_queues->queue(q)->push(obj)) {
    //    tty->print_cr("WARNING: Shenandoah mark queues overflown overflow_queue: obj = %p", (HeapWord*) obj);
    _overflow_queue->push(obj);
  }
}

SharedOverflowMarkQueue* ShenandoahConcurrentMark::overflow_queue() {
  return _overflow_queue;
}

#if TASKQUEUE_STATS
void ShenandoahConcurrentMark::print_taskqueue_stats_hdr(outputStream* const st) {
  st->print_raw_cr("GC Task Stats");
  st->print_raw("thr "); TaskQueueStats::print_header(1, st); st->cr();
  st->print_raw("--- "); TaskQueueStats::print_header(2, st); st->cr();
}

void ShenandoahConcurrentMark::print_taskqueue_stats(outputStream* const st) const {
  print_taskqueue_stats_hdr(st);
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  TaskQueueStats totals;
  const int n = sh->workers() != NULL ? sh->workers()->total_workers() : 1;
  for (int i = 0; i < n; ++i) {
    st->print("%3d ", i); _task_queues->queue(i)->stats.print(st); st->cr();
    totals += _task_queues->queue(i)->stats;
  }
  st->print_raw("tot "); totals.print(st); st->cr();

  DEBUG_ONLY(totals.verify());
}

void ShenandoahConcurrentMark::reset_taskqueue_stats() {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  const int n = sh->workers() != NULL ? sh->workers()->total_workers() : 1;
  for (int i = 0; i < n; ++i) {
    _task_queues->queue(i)->stats.reset();
  }
}
#endif // TASKQUEUE_STATS

// Weak Reference Closures
class ShenandoahCMDrainMarkingStackClosure: public VoidClosure {
  ShenandoahHeap* _sh;
  ShenandoahConcurrentMark* _scm;

public:
ShenandoahCMDrainMarkingStackClosure() {
    _sh = (ShenandoahHeap*) Universe::heap();
    _scm = _sh->concurrentMark();
  }

      
  void do_void() {

    ShenandoahMarkRefsClosure cl1(0);
    ShenandoahMarkRefsNoUpdateClosure cl2(0);
    ExtendedOopClosure* cl;

    if (! ShenandoahUpdateRefsEarly) {
      cl = &cl1;
    } else {
      cl = &cl2;
    }

    while (true) {
      if (!_scm->try_queue(0,cl) &&
	  !_scm->try_overflow_queue(0, cl) &&
	  !_scm->try_draining_an_satb_buffer(0)) {
	break;
      }
    }
  }
};


class ShenandoahCMKeepAliveAndDrainClosure: public OopClosure {
  uint _worker_id;
  ShenandoahHeap* _sh;
  ShenandoahConcurrentMark* _scm;

  size_t _ref_count;

public:
  ShenandoahCMKeepAliveAndDrainClosure(uint worker_id) {
    _worker_id = worker_id;
    _sh = (ShenandoahHeap*) Universe::heap();
    _scm = _sh->concurrentMark();
    _ref_count = 0;
  }

  virtual void do_oop(oop* p){ do_oop_work(p);}
  virtual void do_oop(narrowOop* p) {  
    assert(false, "narrowOops Aren't implemented");
  }


  void do_oop_work(oop* p) {  
    oop obj = *p;

    if (obj != NULL) {
      if (Verbose && ShenandoahTraceWeakReferences) {
	gclog_or_tty->print_cr("\t[%u] we're looking at location "
			       "*"PTR_FORMAT" = "PTR_FORMAT,
			       _worker_id, p, (void*) obj);
	obj->print();
      }

      _sh->mark_current(obj);
      _scm->addTask(obj, _worker_id);

      _ref_count++;
    }    
  }

  size_t ref_count() { return _ref_count; }

};

class ShenandoahRefProcTaskExecutor : public AbstractRefProcTaskExecutor {

public:

  // Executes a task using worker threads.
  virtual void execute(ProcessTask& task) {
    assert(false, "Parallel Reference Processing not implemented");
  }
  virtual void execute(EnqueueTask& task) {
    assert(false, "Parallel Reference Processing not implemented");
  }
};


void ShenandoahConcurrentMark::weakRefsWork(bool clear_all_soft_refs, int worker_id) {
   ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
   ReferenceProcessor* rp = sh->ref_processor_cm();
   ShenandoahIsAliveClosure is_alive;
   ShenandoahCMKeepAliveAndDrainClosure keep_alive(worker_id);
   ShenandoahCMDrainMarkingStackClosure complete_gc;
   ShenandoahRefProcTaskExecutor par_task_executor;
   bool processing_is_mt = false;
   AbstractRefProcTaskExecutor* executor = (processing_is_mt ? &par_task_executor : NULL);


   // no timing for now.
   ConcurrentGCTimer gc_timer;

   if (ShenandoahTraceWeakReferences) {
     gclog_or_tty->print_cr("start processing references");
   }

   rp->process_discovered_references(&is_alive, &keep_alive, 
				     &complete_gc, &par_task_executor, &gc_timer);
   
   if (ShenandoahTraceWeakReferences) {
     gclog_or_tty->print_cr("finished processing references, processed %d refs", keep_alive.ref_count());
     gclog_or_tty->print_cr("start enqueuing references");
   }

   rp->enqueue_discovered_references(executor);

   if (ShenandoahTraceWeakReferences) {
     gclog_or_tty->print_cr("finished enqueueing references");
   }

   rp->verify_no_references_recorded();
   assert(!rp->discovery_enabled(), "Post condition");


  // Now clean up stale oops in StringTable
   StringTable::unlink(&is_alive);
  // Clean up unreferenced symbols in symbol table.
   SymbolTable::unlink();
}

