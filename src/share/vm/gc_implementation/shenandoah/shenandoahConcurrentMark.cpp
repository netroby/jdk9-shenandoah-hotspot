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

#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"

class SCMConcurrentMarkingTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _cm;
  ParallelTaskTerminator* _terminator;
public:
  SCMConcurrentMarkingTask(ShenandoahConcurrentMark* cm, ParallelTaskTerminator* terminator) :
    AbstractGangTask("Root Region Scan"), _cm(cm), _terminator(terminator) {
  }

  void work(uint worker_id) {
    int seed = 17;
    ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
    ShenandoahMarkRefsClosure cl(worker_id);

    while (true) {
      oop obj;
    
      bool success = _cm->task_queues()->queue(worker_id)->pop_local(obj);
      if (! success) {
        // If our queue runs empty, drain some of the SATB buffers, then try again.
        // tty->print_cr("draining SATB buffers while concurrently marking");
	while (_cm->drain_one_satb_buffer(worker_id) && !success)
	  success = _cm->task_queues()->queue(worker_id)->pop_local(obj);
      }
      if (! success) {
        if (!_cm->task_queues()->steal(worker_id, &seed, obj)) {
          obj = _cm->overflow_queue()->pop();
          if (obj == NULL) {
            if (_terminator->offer_termination())
              break;  
            else 
              continue;
          }
        }
      }
      // We got one.

      assert(obj->is_oop(), "Oops, not an oop");
      assert(! sh->heap_region_containing(obj)->is_in_collection_set(), "we don't want to mark objects in from-space");
      obj->oop_iterate(&cl);
    }
  }
};

// We need to revisit this  CHF
// class SCMTerminatorTerminator : public TerminatorTerminator  { // So good we named it twice
//   bool should_exit_termination() {
//     return true;
//   }
// };

void ShenandoahConcurrentMark::initialize(FlexibleWorkGang* workers) {
  if (ShenandoahGCVerbose) 
    tty->print("ActiveWorkers  = %d total workers = %d\n", 
	     workers->active_workers(), 
	     workers->total_workers());
  _max_worker_id = MAX2((uint)ParallelGCThreads, 1U);
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

  
  SCMConcurrentMarkingTask markingTask = SCMConcurrentMarkingTask(this, &terminator);
  sh->workers()->run_task(&markingTask);
  
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
    sh->workers()->run_task(&drain_satb_buffers);
  }

  // Also drain our overflow queue.
  ShenandoahMarkRefsClosure cl(0);
  oop obj = _overflow_queue->pop();
  while (obj != NULL) {
    assert(obj->is_oop(), "Oops, not an oop");
    obj->oop_iterate(&cl);
    obj = _overflow_queue->pop();
  }

  // Finally mark everything else we've got in our queues during the previous steps.
  SCMConcurrentMarkingTask markingTask = SCMConcurrentMarkingTask(this, &terminator);
  sh->workers()->run_task(&markingTask);

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
    tty->print_cr("Adding object %p to marking queue %d\n", (HeapWord*) obj, q);
  }
#endif

  if (!_task_queues->queue(q)->push(obj)) {
    tty->print_cr("WARNING: Shenandoah mark queues overflown");
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
